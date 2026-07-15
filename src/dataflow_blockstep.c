/*
 * dataflow_blockstep.c — the BLOCK-STEP + EMULATOR-REPLAY value tier (F1, increment 1):
 * a lower-perturbation scoped L0 value producer that fills the SAME asmtest_valtrace_t
 * the single-step (dataflow_ptrace.c) and emulator (dataflow_emu.c) producers fill, so
 * the shared L1 def-use + L2 slicer (dataflow.c) work UNCHANGED on its captures.
 * See docs/internal/plans/live-attach-dataflow-followup-plan.md (F1) and the increment-0
 * spike findings docs/internal/analysis/2026-07-15-blockstep-value-spike.md.
 *
 * THE PERTURBATION WIN. Direct PTRACE_SINGLESTEP traps on EVERY instruction — exactly the
 * stop density that widens the cross-thread deadlock window on a live runtime. This tier
 * decouples VALUES from STOPS: it drives the region with PTRACE_SINGLEBLOCK (one #DB per
 * TAKEN branch — an order of magnitude fewer stops), takes a full PTRACE_GETREGS snapshot
 * at each boundary, and REPLAYS each straight-line block between boundaries through a
 * Unicorn engine seeded with that real register state (its guest mapped AT THE REAL
 * addresses, memory faulted from the tracee via process_vm_readv) to reconstruct the
 * per-instruction values in the pure interior. The endpoints are always real observations;
 * replay only ever fills a bounded pure interior. The spike proved this reconstruction is
 * BYTE-IDENTICAL (down to a raw memcmp, modulo the stack-absolute delta two forks carry) to
 * true single-step on pure integer/flag code, while cutting the in-region stop count ~6x.
 *
 * PURITY GATE. Block-step advances the REAL process, so a syscall inside a block has
 * already retired by the boundary — emulating through it would be wrong. So the region's
 * (time-correct) bytes are static-scanned ONCE up front for OS-interacting / nondeterministic
 * instructions (syscall / sysenter / int 0x80 / rdtsc / rdtscp / rdrand / rdseed / cpuid):
 * a PURE region gets block-step + replay; an IMPURE one falls back to direct single-step
 * (the same shared capture core, driven one instruction at a time). F2 lifts the fallback
 * via record-and-inject.
 *
 * COHERENCE CANARY. Replay is only correct if the emulator's inputs match reality; a sibling
 * that rewrites a loaded byte between the boundary snapshot and the real block's execution
 * would make the replay silently wrong. So at every boundary the emulator's COMPUTED
 * end-of-block state is compared to the real next boundary (GP regs + rip + rsp + arithmetic
 * flags); a mismatch sets vt->truncated and returns DF_BLOCKSTEP_FAULT — never silently wrong.
 *
 * ORACLE. asmtest_dataflow_blockstep_run(..., force_singlestep) captures the same region by
 * true single-step (registers from GETREGS, memory from process_vm_readv), the ground-truth
 * the block-step trace is cross-validated against. The two are one process each, so they
 * differ only by an absolute stack-address delta (ASLR + frame depth); info.entry_rsp reports
 * the region-entry rsp so a caller can normalize stack-absolute values (rsp-relative) before
 * an equality check, exactly as the shipped slice-oracle sidesteps it by keying on locations.
 *
 * Reuses the shared capture core READ-ONLY: the L0 sink (dataflow.c) and the Capstone
 * operand read/write-set enumerator (dataflow_operands.c). The per-step open/finalize glue
 * over a struct user_regs_struct is producer-local (as it is in dataflow_ptrace.c and the
 * spike) — a value-trace PRODUCER is a tier, not part of the shared asmtest_valtrace.h sink
 * API, so this file ships no header; its test re-declares the entry points and codes below.
 *
 * Requires Linux x86-64 + Capstone (operand enumerator + purity scan) + Unicorn (replay);
 * off-platform / without those it compiles to a DF_BLOCKSTEP_ENOSYS stub and callers
 * self-skip. At runtime it self-skips (DF_BLOCKSTEP_ETRACE / a 0 from the probe) where
 * ptrace is blocked (seccomp/yama) or PTRACE_SINGLEBLOCK is non-functional (a hypervisor
 * masking DEBUGCTL.BTF degrades block-step to per-instruction stepping).
 *
 * Scope (this increment): a deterministic, single-threaded leaf routine of up to six integer
 * arguments, executed from an inherited executable mapping; GP registers + rflags + memory
 * operands <= 8 bytes. Vector/XSTATE (YMM/ZMM) seeding and F2 record-inject for impure
 * methods are bounded follow-ons the spike mapped; neither re-opens the increment-0 go/no-go.
 */
#define _GNU_SOURCE

#include <sys/types.h> /* pid_t — part of the entry-point signatures on every platform */

#include "asmtest_valtrace.h"

/* Return codes from the block-step producer (kept in step with the test's copy). Mirrors
 * dataflow_ptrace.c's DF_PTRACE_* vocabulary so a caller can share the self-skip logic. */
#define DF_BLOCKSTEP_OK 0 /* clean scoped capture                           */
#define DF_BLOCKSTEP_FAULT                                                     \
    1 /* fault / divergence: a partial trace, truncated */
#define DF_BLOCKSTEP_EINVAL                                                    \
    (-1) /* bad arguments                                  */
#define DF_BLOCKSTEP_ENOSYS                                                    \
    (-3) /* off Linux x86-64 / no Capstone / no Unicorn    */
#define DF_BLOCKSTEP_ETRACE                                                    \
    (-4) /* ptrace / SINGLEBLOCK unavailable: self-skip    */

/* Capture options. A zero-initialized struct is the production tier: purity-gated
 * block-step+replay, unbounded, no test injection. */
typedef struct {
    uint64_t
        max_insns; /* 0 = unbounded (still bounded by the hard step backstop) */
    int force_singlestep; /* skip the purity gate; single-step (the ground-truth oracle) */
    int inject_divergence; /* test hook: corrupt a replay seed to fire the coherence canary */
    int inject_block; /* which 0-based interior block's replay seed to corrupt */
} asmtest_blockstep_opts_t;

/* Capture telemetry, filled on every non-EINVAL return. */
typedef struct {
    int pure; /* 1 = block-step+replay was used; 0 = single-stepped (fallback or forced) */
    const char *
        reason; /* offending mnemonic when the purity gate forced single-step, else NULL */
    uint64_t
        stops; /* in-region ptrace stops taken (the perturbation measure) */
    uint64_t steps; /* in-region instructions captured */
    uint64_t
        entry_rsp; /* rsp at the region entry — the rsp-relative normalization anchor */
} asmtest_blockstep_info_t;

#if defined(__linux__) && defined(__x86_64__) &&                               \
    defined(ASMTEST_HAVE_CAPSTONE) && defined(ASMTEST_HAVE_UNICORN)

#include <capstone/capstone.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unicorn/unicorn.h>
#include <unistd.h>

#ifndef PTRACE_SINGLEBLOCK
#define PTRACE_SINGLEBLOCK 33 /* <sys/ptrace.h> omits it; the kernel wires it */
#endif

/* Hard backstop on TOTAL stops (prologue/glue to region entry, plus the region): bounds
 * wall time if the tracee never enters or never leaves [base, base+len). */
#define DFB_STOP_BACKSTOP (1u << 20)

/* TF (single-step) and RF (resume) are DEBUG-MECHANISM bits, not program semantics: a
 * single-stepped tracee can surface TF in its GETREGS eflags where a SINGLEBLOCK boundary
 * need not, and a #DB can set RF. The value trace records PROGRAM values, so both paths mask
 * these out of every captured eflags via the one shared gp_value below. */
#define EFLAGS_STEP_BITS                                                       \
    0x00010100ULL /* TF (bit 8) | RF (bit 16)                */
#define EFLAGS_ARITH_MASK                                                      \
    0x00000CD5ULL /* CF PF AF ZF SF DF OF — the canary's mask */

/* ------------------------------------------------------------------ */
/* Shared capture core: one record stream, two value sources           */
/* ------------------------------------------------------------------ */

/* A pluggable memory reader — the single-step path reads the tracee (process_vm_readv), the
 * replay path the Unicorn guest. Returns 1 iff all n bytes were read. */
typedef int (*mem_reader_fn)(void *ctx, uint64_t addr, void *buf, size_t n);

typedef struct {
    at_val_rec_t *v;
    size_t n, cap;
} recbuf;

static void recbuf_push(recbuf *rb, const at_val_rec_t *r) {
    if (rb->n == rb->cap) {
        size_t nc = rb->cap ? rb->cap * 2 : 16;
        at_val_rec_t *nv = (at_val_rec_t *)realloc(rb->v, nc * sizeof *nv);
        if (nv == NULL)
            return;
        rb->v = nv;
        rb->cap = nc;
    }
    rb->v[rb->n++] = *r;
}

typedef struct {
    asmtest_valtrace_t *vt;
    const uint8_t *code;
    size_t code_len;
    uint64_t base;
    int have_cur;
    uint64_t cur_off;
    recbuf cur;
    mem_reader_fn mr;
    void *mr_ctx;
} cap_ctx;

/* Map a Capstone x86 register id to its 64-bit container value in a GP register file,
 * folding sub-registers to the container. EFLAGS is masked of the debug-stepping bits so
 * both value sources agree. Returns 0 for regs not in this file (vector / segment
 * selectors), whose value is then left uncaptured — none of the pure fixtures hit it. */
static int gp_value(const struct user_regs_struct *r, uint32_t reg,
                    uint64_t *out) {
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        *out = r->rax;
        return 1;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        *out = r->rbx;
        return 1;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        *out = r->rcx;
        return 1;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        *out = r->rdx;
        return 1;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        *out = r->rsi;
        return 1;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        *out = r->rdi;
        return 1;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        *out = r->rbp;
        return 1;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        *out = r->rsp;
        return 1;
    case X86_REG_R8:
        *out = r->r8;
        return 1;
    case X86_REG_R9:
        *out = r->r9;
        return 1;
    case X86_REG_R10:
        *out = r->r10;
        return 1;
    case X86_REG_R11:
        *out = r->r11;
        return 1;
    case X86_REG_R12:
        *out = r->r12;
        return 1;
    case X86_REG_R13:
        *out = r->r13;
        return 1;
    case X86_REG_R14:
        *out = r->r14;
        return 1;
    case X86_REG_R15:
        *out = r->r15;
        return 1;
    case X86_REG_RIP:
        *out = r->rip;
        return 1;
    case X86_REG_EFLAGS:
        *out = (uint64_t)r->eflags & ~EFLAGS_STEP_BITS;
        return 1;
    default:
        return 0;
    }
}

/* Resolve a memory operand's effective address from a register file: seg_base + base +
 * index*scale + disp, with fs_base/gs_base segment resolution and the RIP-relative
 * next-instruction fixup. Mirrors dataflow_ptrace.c resolve_ea so the core is drop-in. */
static uint64_t resolve_ea(const struct user_regs_struct *regs,
                           const at_val_rec_t *r, size_t insn_len) {
    uint64_t ea = (uint64_t)r->disp;
    if (r->reg == X86_REG_GS)
        ea += regs->gs_base;
    else if (r->reg == X86_REG_FS)
        ea += regs->fs_base;
    if (r->base != 0) {
        uint64_t bv;
        if (gp_value(regs, r->base, &bv)) {
            ea += bv;
            if (r->base == X86_REG_RIP)
                ea += insn_len;
        }
    }
    if (r->index != 0 && r->scale != 0) {
        uint64_t iv;
        if (gp_value(regs, r->index, &iv))
            ea += iv * (uint64_t)r->scale;
    }
    return ea;
}

static void fill_mem_value(cap_ctx *c, at_val_rec_t *r) {
    uint16_t sz = r->size;
    if (sz == 0 || sz > 8)
        return; /* this increment captures memory operands <= 8 bytes */
    uint8_t buf[8] = {0};
    if (!c->mr(c->mr_ctx, r->addr, buf, sz))
        return;
    r->value = 0;
    memcpy(&r->value, buf, sz);
    r->value_valid = true;
}

/* Finalize the current step's deferred WRITE values from the POST-instruction state
 * (`regs`) and append the step. Mirrors dataflow_ptrace.c finalize_step. */
static void finalize_step(cap_ctx *c, const struct user_regs_struct *regs) {
    for (size_t i = 0; i < c->cur.n; i++) {
        at_val_rec_t *r = &c->cur.v[i];
        if (!r->is_write || r->value_valid)
            continue;
        if (r->kind == AT_LOC_REG) {
            uint64_t v;
            if (gp_value(regs, r->reg, &v)) {
                r->value = v;
                r->value_valid = true;
            }
        } else {
            fill_mem_value(c, r); /* addr resolved when the step opened */
        }
    }
    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v, c->cur.n);
    c->have_cur = 0;
    c->cur.n = 0;
}

/* Open the step at `regs->rip`: enumerate its read/write set, capture READ values from this
 * PRE-state (registers via gp_value, memory via the reader at the resolved EA), resolve store
 * addresses, defer WRITE values. Returns the instruction byte length. Mirrors dataflow_ptrace.c
 * open_step. */
static size_t open_step(cap_ctx *c, const struct user_regs_struct *regs) {
    uint64_t off = regs->rip - c->base;
    c->cur.n = 0;
    c->cur_off = off;
    c->have_cur = 1;

    at_val_rec_t rd[64], wr[64];
    size_t nr = 64, nw = 64;
    size_t insn_len = asmtest_operands(ASMTEST_ARCH_X86_64, c->code,
                                       c->code_len, off, rd, &nr, wr, &nw);
    for (size_t i = 0; i < nr; i++) {
        at_val_rec_t r = rd[i];
        if (r.kind == AT_LOC_REG) {
            uint64_t v;
            if (gp_value(regs, r.reg, &v)) {
                r.value = v;
                r.value_valid = true;
            }
        } else {
            r.addr = resolve_ea(regs, &r, insn_len);
            fill_mem_value(c, &r); /* load value is in memory pre-instruction */
        }
        recbuf_push(&c->cur, &r);
    }
    for (size_t i = 0; i < nw; i++) {
        at_val_rec_t r = wr[i];
        r.value_valid = false;
        if (r.kind != AT_LOC_REG)
            r.addr = resolve_ea(regs, &r, insn_len);
        recbuf_push(&c->cur, &r);
    }
    return insn_len;
}

/* One captured step: finalize the previous step with this stop's regs (its post-state) and
 * open the current one (its pre-state) — the single point of contact for both paths. Returns
 * the current instruction's byte length. */
static size_t capture_at(cap_ctx *c, const struct user_regs_struct *regs) {
    if (c->have_cur)
        finalize_step(c, regs);
    return open_step(c, regs);
}

/* ------------------------------------------------------------------ */
/* Memory readers                                                       */
/* ------------------------------------------------------------------ */
static int mr_tracee(void *ctx, uint64_t addr, void *buf, size_t n) {
    pid_t pid = *(pid_t *)ctx;
    struct iovec l = {buf, n};
    struct iovec r = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)n;
}
static int mr_uc(void *ctx, uint64_t addr, void *buf, size_t n) {
    uc_engine *uc = (uc_engine *)ctx;
    return uc_mem_read(uc, addr, buf, n) == UC_ERR_OK;
}

/* ------------------------------------------------------------------ */
/* Purity static scan (F1 region-granularity classifier)               */
/* ------------------------------------------------------------------ */

/* Linearly disassemble the region's bytes ONCE and flag the first OS-interacting /
 * nondeterministic instruction (syscall / sysenter / int 0x80 / rdtsc / rdtscp / rdrand /
 * rdseed / cpuid). Returns 1 for PURE (block-step+replay eligible), 0 for IMPURE (*reason,
 * when non-NULL, receives the offending mnemonic as a static string). A linear sweep is exact
 * for a straight instruction stream; a production classifier would follow the JIT method-map's
 * real instruction extents so embedded data / bytes past an indirect branch cannot misdecode. */
static int region_is_pure(const uint8_t *code, size_t len,
                          const char **reason) {
    if (reason != NULL)
        *reason = NULL;
    csh h;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK)
        return 1; /* no decoder: treat as pure — the caller still validates via the canary */
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn *insn = cs_malloc(h);
    uint64_t addr = 0;
    const uint8_t *p = code;
    size_t remaining = len;
    const char *why = NULL;
    while (remaining > 0 && cs_disasm_iter(h, &p, &remaining, &addr, insn)) {
        switch (insn->id) {
        case X86_INS_SYSCALL:
            why = "syscall";
            break;
        case X86_INS_SYSENTER:
            why = "sysenter";
            break;
        case X86_INS_RDTSC:
            why = "rdtsc";
            break;
        case X86_INS_RDTSCP:
            why = "rdtscp";
            break;
        case X86_INS_RDRAND:
            why = "rdrand";
            break;
        case X86_INS_RDSEED:
            why = "rdseed";
            break;
        case X86_INS_CPUID:
            why = "cpuid";
            break;
        case X86_INS_INT:
            /* int 0x80 is the legacy syscall gate; the plan names it specifically. */
            if (insn->detail->x86.op_count == 1 &&
                insn->detail->x86.operands[0].type == X86_OP_IMM &&
                insn->detail->x86.operands[0].imm == 0x80)
                why = "int 0x80";
            break;
        default:
            break;
        }
        if (why != NULL)
            break;
    }
    cs_free(insn, 1);
    cs_close(&h);
    if (why != NULL) {
        if (reason != NULL)
            *reason = why;
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Tracee spawn / teardown                                              */
/* ------------------------------------------------------------------ */
typedef long (*fn6_t)(long, long, long, long, long, long);

/* Map the routine's bytes into an inherited executable page (RW then R+X, so it works on a
 * W^X kernel). Returns the mapping or NULL. */
static void *map_exec(const uint8_t *code, size_t code_len) {
    void *ex = mmap(NULL, code_len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ex == MAP_FAILED)
        return NULL;
    memcpy(ex, code, code_len);
    if (mprotect(ex, code_len, PROT_READ | PROT_EXEC) != 0) {
        munmap(ex, code_len);
        return NULL;
    }
    return ex;
}

/* Fork a self-owned tracee that TRACEME's, SIGSTOPs for us, then calls the routine at `base`
 * on its natural (inherited) stack. Both the single-step and block-step captures spawn through
 * THIS one function, so the fixture's `call` return address is a single fixed code site across
 * captures — only the stack ABSOLUTE addresses differ (ASLR + this run's frame depth), which
 * info.entry_rsp lets a caller normalize away. Returns the pid stopped at the initial SIGSTOP
 * (EXITKILL applied), or -1. Only rdi..r9 are wired — the fixtures take at most six integer
 * args. */
static pid_t spawn_tracee(uint64_t base, const long *a) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)base)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w >= 0 || errno != EINTR)
            break;
    }
    if (!WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);
    return pid;
}

static void reap(pid_t pid) {
    int status;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

/* ------------------------------------------------------------------ */
/* Path A — true single-step (the ground-truth oracle + impure fallback) */
/* ------------------------------------------------------------------ */
/* Drive PTRACE_SINGLESTEP over the region of an already-stopped tracee, capturing each
 * in-region step. Returns DF_BLOCKSTEP_OK on a clean region exit (*result = rax), else
 * DF_BLOCKSTEP_ETRACE / _FAULT. *stops = in-region single-step stops; *steps = captured
 * steps; *entry_rsp = rsp at the first in-region stop. */
static int capture_singlestep(cap_ctx *c, pid_t pid, uint64_t base, size_t len,
                              uint64_t max_insns, long *result, uint64_t *stops,
                              uint64_t *steps, uint64_t *entry_rsp) {
    int rc = DF_BLOCKSTEP_ETRACE, entered = 0;
    uint64_t nstop = 0, guard = 0;
    struct user_regs_struct regs;
    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0)
            break;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (c->have_cur)
                c->vt->truncated = true;
            rc = entered ? DF_BLOCKSTEP_FAULT : DF_BLOCKSTEP_ETRACE;
            break;
        }
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP)
            break;
        if (++guard > DFB_STOP_BACKSTOP) {
            c->vt->truncated = true;
            break;
        }
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
            break;
        uint64_t pc = regs.rip;
        if (pc >= base && pc < base + len) {
            if (!entered && entry_rsp != NULL)
                *entry_rsp = regs.rsp;
            capture_at(c,
                       &regs); /* finalize prev (post) + open current (pre) */
            entered = 1;
            nstop++;
            if (max_insns != 0 && nstop >= max_insns) {
                if (c->have_cur) {
                    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v,
                                            c->cur.n);
                    c->have_cur = 0;
                }
                c->vt->truncated = true;
                rc = DF_BLOCKSTEP_FAULT;
                break;
            }
        } else if (entered) {
            if (c->have_cur)
                finalize_step(c, &regs); /* last step's post-state */
            if (result != NULL)
                *result = (long)regs.rax;
            rc = DF_BLOCKSTEP_OK;
            break;
        }
    }
    if (stops != NULL)
        *stops = nstop;
    if (steps != NULL)
        *steps = c->vt->steps_len;
    return rc;
}

/* ------------------------------------------------------------------ */
/* Path B — block-step + Unicorn replay                                */
/* ------------------------------------------------------------------ */

/* Copy Unicorn's GP file into a struct user_regs_struct (for capture + the canary). */
static void uc_get_regs(uc_engine *uc, struct user_regs_struct *r) {
    memset(r, 0, sizeof *r);
    uint64_t v;
#define RD(ID, F)                                                              \
    do {                                                                       \
        v = 0;                                                                 \
        uc_reg_read(uc, ID, &v);                                               \
        r->F = v;                                                              \
    } while (0)
    RD(UC_X86_REG_RAX, rax);
    RD(UC_X86_REG_RBX, rbx);
    RD(UC_X86_REG_RCX, rcx);
    RD(UC_X86_REG_RDX, rdx);
    RD(UC_X86_REG_RSI, rsi);
    RD(UC_X86_REG_RDI, rdi);
    RD(UC_X86_REG_RBP, rbp);
    RD(UC_X86_REG_RSP, rsp);
    RD(UC_X86_REG_R8, r8);
    RD(UC_X86_REG_R9, r9);
    RD(UC_X86_REG_R10, r10);
    RD(UC_X86_REG_R11, r11);
    RD(UC_X86_REG_R12, r12);
    RD(UC_X86_REG_R13, r13);
    RD(UC_X86_REG_R14, r14);
    RD(UC_X86_REG_R15, r15);
    RD(UC_X86_REG_RIP, rip);
    RD(UC_X86_REG_EFLAGS, eflags);
    RD(UC_X86_REG_FS_BASE, fs_base);
    RD(UC_X86_REG_GS_BASE, gs_base);
#undef RD
}

/* Seed Unicorn's GP file from a real boundary snapshot. */
static void uc_set_regs(uc_engine *uc, const struct user_regs_struct *r) {
    uint64_t v;
#define WR(ID, F)                                                              \
    do {                                                                       \
        v = r->F;                                                              \
        uc_reg_write(uc, ID, &v);                                              \
    } while (0)
    WR(UC_X86_REG_RAX, rax);
    WR(UC_X86_REG_RBX, rbx);
    WR(UC_X86_REG_RCX, rcx);
    WR(UC_X86_REG_RDX, rdx);
    WR(UC_X86_REG_RSI, rsi);
    WR(UC_X86_REG_RDI, rdi);
    WR(UC_X86_REG_RBP, rbp);
    WR(UC_X86_REG_RSP, rsp);
    WR(UC_X86_REG_R8, r8);
    WR(UC_X86_REG_R9, r9);
    WR(UC_X86_REG_R10, r10);
    WR(UC_X86_REG_R11, r11);
    WR(UC_X86_REG_R12, r12);
    WR(UC_X86_REG_R13, r13);
    WR(UC_X86_REG_R14, r14);
    WR(UC_X86_REG_R15, r15);
    WR(UC_X86_REG_RIP, rip);
    WR(UC_X86_REG_EFLAGS, eflags);
    WR(UC_X86_REG_FS_BASE, fs_base);
    WR(UC_X86_REG_GS_BASE, gs_base);
#undef WR
}

/* The coherence CANARY: does Unicorn's computed end-of-block state agree with the real next
 * boundary? Compares the GP regs, rip, rsp and the arithmetic flags (ignoring IF / reserved /
 * debug bits). A mismatch means the replay's inputs diverged from reality (e.g. a sibling
 * rewrote a loaded byte) and the block drops to `truncated`. */
static int regs_coherent(const struct user_regs_struct *uc,
                         const struct user_regs_struct *real) {
    const uint64_t U[] = {uc->rax, uc->rbx, uc->rcx, uc->rdx, uc->rsi, uc->rdi,
                          uc->rbp, uc->rsp, uc->r8,  uc->r9,  uc->r10, uc->r11,
                          uc->r12, uc->r13, uc->r14, uc->r15, uc->rip};
    const uint64_t R[] = {real->rax, real->rbx, real->rcx, real->rdx, real->rsi,
                          real->rdi, real->rbp, real->rsp, real->r8,  real->r9,
                          real->r10, real->r11, real->r12, real->r13, real->r14,
                          real->r15, real->rip};
    for (size_t i = 0; i < sizeof U / sizeof U[0]; i++)
        if (U[i] != R[i])
            return 0;
    return ((uint64_t)uc->eflags & EFLAGS_ARITH_MASK) ==
           ((uint64_t)real->eflags & EFLAGS_ARITH_MASK);
}

/* Replay one straight-line block through Unicorn, capturing each interior instruction, until a
 * TAKEN transfer whose target is `pc_next`. Returns 0 on the clean terminator, 1 if Unicorn
 * branched somewhere OTHER than the real boundary (divergence), -1 on a Unicorn fault /
 * undecodable step. The terminating branch is left as the open step (finalized by the next
 * block's seed, or at region exit). */
static int step_block(cap_ctx *c, uc_engine *uc, uint64_t pc_next) {
    struct user_regs_struct R;
    for (size_t guard = 0; guard <= c->code_len + 4; guard++) {
        uc_get_regs(uc, &R);
        uint64_t pc = R.rip;
        size_t len = capture_at(c, &R); /* finalize prev + open current */
        if (len == 0)
            return -1;
        if (uc_emu_start(uc, pc, (uint64_t)-1, 0, 1) != UC_ERR_OK)
            return -1;
        uint64_t next = 0;
        uc_reg_read(uc, UC_X86_REG_RIP, &next);
        if (next != pc + len) { /* a taken control transfer */
            if (next == pc_next)
                return 0; /* the block terminator: reached the real boundary */
            return 1;     /* diverged to a different target than reality */
        }
    }
    return -1; /* no terminator within the region bound */
}

/* Block-step the real tracee and replay each block through Unicorn. Returns DF_BLOCKSTEP_OK on
 * a clean region exit (*result = rax), DF_BLOCKSTEP_FAULT when the coherence canary fires
 * (vt->truncated set), or DF_BLOCKSTEP_ETRACE on setup/ptrace failure. *stops = in-region
 * block boundaries; *steps = captured steps; *entry_rsp = rsp at the entry boundary.
 * inject_block >= 0 corrupts Unicorn's seed rax at that 0-based interior block to SIMULATE a
 * concurrent-divergence input, exercising the canary. */
static int capture_blockstep(cap_ctx *c, pid_t pid, uint64_t base, size_t len,
                             long *result, uint64_t *stops, uint64_t *steps,
                             uint64_t *entry_rsp, int inject_block) {
    int ret = DF_BLOCKSTEP_ETRACE;
    uc_engine *uc = NULL;
    uint8_t *stackbuf = NULL;
    uint64_t win_base = 0, win_size = 0;
    struct user_regs_struct S_cur;
    int at_entry = 0;
    uint64_t nstop = 1; /* the entry boundary itself */
    int block_idx = 0;
    long real_result = 0;

    /* 1) Block-step through the entry glue until the first IN-REGION stop = boundary 0. */
    for (uint64_t g = 0; g < DFB_STOP_BACKSTOP; g++) {
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) != 0)
            goto out;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            goto out;
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP)
            goto out;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &S_cur) != 0)
            goto out;
        if (S_cur.rip >= base && S_cur.rip < base + len) {
            at_entry = 1;
            break;
        }
    }
    if (!at_entry)
        goto out;
    if (entry_rsp != NULL)
        *entry_rsp = S_cur.rsp;

    /* 2) Stand up Unicorn: code mapped at the REAL base, a stack window at the REAL rsp, both
     *    copied from the (stopped) tracee, and the GP file seeded from the entry boundary. Real
     *    addresses => effective addresses + values compare directly against the oracle. */
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK)
        goto out;
    {
        uint64_t code_base = base & ~0xFFFULL;
        uint64_t code_size = ((base + len + 0xFFFULL) & ~0xFFFULL) - code_base;
        if (uc_mem_map(uc, code_base, code_size, UC_PROT_ALL) != UC_ERR_OK)
            goto out;
        uint8_t *cb = (uint8_t *)malloc(code_size);
        if (cb == NULL)
            goto out;
        if (!mr_tracee(&pid, code_base, cb, code_size)) {
            memset(cb, 0, code_size);
            memcpy(cb + (base - code_base), c->code, len);
        }
        uc_mem_write(uc, code_base, cb, code_size);
        free(cb);

        win_base = (S_cur.rsp - 0x1000) & ~0xFFFULL;
        win_size =
            0x3000; /* [rsp-0x1000, rsp+0x2000): rsp-8 and the ret slot */
        if (uc_mem_map(uc, win_base, win_size, UC_PROT_READ | UC_PROT_WRITE) !=
            UC_ERR_OK)
            goto out;
        stackbuf = (uint8_t *)malloc(win_size);
        if (stackbuf == NULL)
            goto out;
    }
    c->mr_ctx = uc;

    /* Snapshot the tracee's stack window as of the entry boundary and seed Unicorn. */
    if (!mr_tracee(&pid, win_base, stackbuf, win_size))
        memset(stackbuf, 0, win_size);
    uc_mem_write(uc, win_base, stackbuf, win_size);
    uc_set_regs(uc, &S_cur);

    for (;;) {
        /* Advance the REAL tracee one block; this is the perturbing stop we count. */
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) != 0)
            goto out;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            goto out;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            c->vt->truncated = true; /* ended before a clean region return */
            ret = DF_BLOCKSTEP_FAULT;
            goto out;
        }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
            goto out;
        struct user_regs_struct S_next;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &S_next) != 0)
            goto out;
        int in_region_next = (S_next.rip >= base && S_next.rip < base + len);

        /* The region-EXIT terminator (a ret / tail jump) transfers to a real address OUTSIDE
         * the mapped region — Unicorn would UC_ERR_FETCH_UNMAPPED trying to fetch there even
         * under count=1. Map a one-page landing pad at that boundary so the terminator's data
         * effects (pop rsp, [rsp] read) execute and Unicorn halts cleanly AT the boundary.
         * Best-effort: an already-mapped page is fine. */
        if (!in_region_next) {
            uint64_t pad = S_next.rip & ~0xFFFULL;
            uc_mem_map(uc, pad, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
        }

        /* Seed this block from the real starting boundary + its memory snapshot, then replay
         * it. (The seed also finalizes the previous block's branch with real ground truth on
         * the next capture_at.) */
        uc_set_regs(uc, &S_cur);
        uc_mem_write(uc, win_base, stackbuf, win_size);
        if (inject_block >= 0 && block_idx == inject_block) {
            uint64_t bad =
                S_cur.rax + 1; /* simulate a diverging replay input */
            uc_reg_write(uc, UC_X86_REG_RAX, &bad);
        }

        int brc = step_block(c, uc, S_next.rip);
        if (brc < 0)
            goto out;

        struct user_regs_struct ucR;
        uc_get_regs(uc, &ucR);
        if (brc == 1 || !regs_coherent(&ucR, &S_next)) {
            c->vt->truncated =
                true; /* divergence detected: never silently wrong */
            ret = DF_BLOCKSTEP_FAULT;
            goto out;
        }

        if (!in_region_next) {
            /* Region return: finalize the terminating step with Unicorn's post-state. */
            if (c->have_cur)
                finalize_step(c, &ucR);
            real_result = (long)S_next.rax;
            ret = DF_BLOCKSTEP_OK;
            break;
        }

        /* Advance to the next block: resnapshot the tracee's stack (stopped at S_next now) so
         * the next block's loads see ground-truth memory. */
        nstop++;
        block_idx++;
        if (!mr_tracee(&pid, win_base, stackbuf, win_size))
            memset(stackbuf, 0, win_size);
        S_cur = S_next;
    }

    if (result != NULL)
        *result = real_result;

out:
    if (stops != NULL)
        *stops = nstop;
    if (steps != NULL)
        *steps = c->vt->steps_len;
    free(stackbuf);
    if (uc != NULL)
        uc_close(uc);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Functional self-skip probes (hang-proof)                            */
/* ------------------------------------------------------------------ */
static int wait_stop_sigtrap(pid_t pid) {
    int st;
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid)
            return WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP;
        if (w < 0)
            return 0;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}
static int probe_ptrace(void) {
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(1);
        raise(SIGSTOP);
        _exit(0);
    }
    int status = 0, ok = 0;
    if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status))
        ok = 1;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    return ok;
}
static int probe_singleblock(void) {
    static const uint8_t blob[] = {0xCC, 0x90, 0x90, 0x90, 0x90, 0x90, 0xC3};
    void *p = mmap(NULL, sizeof blob, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    memcpy(p, blob, sizeof blob);
    pid_t pid = fork();
    if (pid < 0) {
        munmap(p, sizeof blob);
        return 0;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        ((void (*)(void))p)();
        _exit(0);
    }
    int functional = 0;
    struct user_regs_struct regs;
    if (wait_stop_sigtrap(pid) &&
        ptrace(PTRACE_GETREGS, pid, NULL, &regs) == 0 &&
        regs.rip == (uint64_t)(uintptr_t)p + 1 &&
        ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) == 0 &&
        wait_stop_sigtrap(pid) && ptrace(PTRACE_GETREGS, pid, NULL, &regs) == 0)
        functional = regs.rip < (uint64_t)(uintptr_t)p ||
                     regs.rip >= (uint64_t)(uintptr_t)p + sizeof blob;
    int st;
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    munmap(p, sizeof blob);
    return functional;
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

int asmtest_dataflow_blockstep_probe(void) {
    if (!probe_ptrace())
        return 0;
    if (!probe_singleblock())
        return 0;
    return 1;
}

int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason) {
    if (code == NULL || code_len == 0)
        return DF_BLOCKSTEP_EINVAL;
    return region_is_pure(code, code_len, reason);
}

int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info) {
    if (vt == NULL || code == NULL || code_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_BLOCKSTEP_EINVAL;

    asmtest_blockstep_opts_t o = {0, 0, 0, -1};
    if (opts != NULL)
        o = *opts;
    if (info != NULL) {
        info->pure = 0;
        info->reason = NULL;
        info->stops = 0;
        info->steps = 0;
        info->entry_rsp = 0;
    }
    vt->mem_space = AT_LOC_MEM_ABS;

    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    /* Purity gate: pure -> block-step + replay; impure -> single-step fallback. force_singlestep
     * always takes the oracle path but the scan still runs so info.reason is informative. */
    const char *reason = NULL;
    int pure = region_is_pure(code, code_len, &reason);
    int use_replay = pure && !o.force_singlestep;

    void *ex = map_exec(code, code_len);
    if (ex == NULL)
        return DF_BLOCKSTEP_ETRACE;
    uint64_t base = (uint64_t)(uintptr_t)ex;
    pid_t pid = spawn_tracee(base, a);
    if (pid < 0) {
        munmap(ex, code_len);
        return DF_BLOCKSTEP_ETRACE;
    }

    cap_ctx c;
    memset(&c, 0, sizeof c);
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base;

    uint64_t stops = 0, steps = 0, entry_rsp = 0;
    int rc;
    if (use_replay) {
        c.mr = mr_uc; /* mr_ctx set once the engine stands up */
        int inj = o.inject_divergence ? o.inject_block : -1;
        rc = capture_blockstep(&c, pid, base, code_len, result, &stops, &steps,
                               &entry_rsp, inj);
    } else {
        c.mr = mr_tracee;
        c.mr_ctx = &pid;
        rc = capture_singlestep(&c, pid, base, code_len, o.max_insns, result,
                                &stops, &steps, &entry_rsp);
    }

    if (info != NULL) {
        info->pure = use_replay;
        info->reason = (!pure && !o.force_singlestep) ? reason : NULL;
        info->stops = stops;
        info->steps = steps;
        info->entry_rsp = entry_rsp;
    }

    free(c.cur.v);
    reap(pid);
    munmap(ex, code_len);
    return rc;
}

#else /* not (Linux x86-64 + Capstone + Unicorn): ENOSYS stubs */

int asmtest_dataflow_blockstep_probe(void) { return DF_BLOCKSTEP_ENOSYS; }

int asmtest_dataflow_blockstep_is_pure(const uint8_t *code, size_t code_len,
                                       const char **reason) {
    (void)code;
    (void)code_len;
    if (reason != NULL)
        *reason = NULL;
    return DF_BLOCKSTEP_ENOSYS;
}

int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info) {
    (void)code;
    (void)code_len;
    (void)args;
    (void)nargs;
    (void)opts;
    (void)result;
    (void)vt;
    if (info != NULL)
        memset(info, 0, sizeof *info);
    return DF_BLOCKSTEP_ENOSYS;
}

#endif
