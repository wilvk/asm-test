/*
 * blockstep_value_spike.c — F1 spike (increment 0): block-step + emulator-replay
 * value optimization. Self-contained probe for
 * docs/internal/plans/live-attach-dataflow-followup-plan.md (F1) and its findings
 * doc docs/internal/analysis/2026-07-15-blockstep-value-spike.md.
 *
 * THE BET F1 makes: direct PTRACE_SINGLESTEP traps on EVERY instruction, which is
 * exactly the stop density that widens the cross-thread deadlock window on a live
 * runtime. F1 proposes: drive the region with PTRACE_SINGLEBLOCK (one #DB per TAKEN
 * branch — an order of magnitude fewer stops), read a real GETREGS snapshot at each
 * boundary, and REPLAY the straight-line block between boundaries through Unicorn,
 * seeded with that real register state, to reconstruct the per-instruction values.
 * The endpoints are always real observations; replay only ever fills a bounded pure
 * interior. This is the ONE genuinely unproven claim in the whole design.
 *
 * THE SPIKE QUESTION (increment 0): capture one deterministic block BOTH ways —
 * block-step + replay vs. true single-step — on the oracle fixture and assert the
 * value traces are BYTE-IDENTICAL. If they diverge, the optimization does not land.
 * Also measure the STOP-COUNT reduction (single-step stops vs block-step stops), the
 * perturbation win F1 exists to capture.
 *
 * METHODOLOGY. The two shipped producers (src/dataflow_ptrace.c single-step and
 * src/dataflow_emu.c Unicorn) do NOT emit byte-identical records for memory operands
 * by design — ptrace inlines a memory record in read/write-set order, the emulator
 * appends it via a UC hook in execution order; their SLICES match (that is the shipped
 * oracle cross-check), not their bytes. So a faithful byte-identical spike must hold
 * the record-construction code CONSTANT across both paths and vary ONLY the value
 * source. This probe therefore uses ONE capture core (open_step/finalize_step over a
 * `struct user_regs_struct` + a pluggable memory reader) driven two ways:
 *   Path A — true single-step: registers from PTRACE_GETREGS, memory from
 *            process_vm_readv, one PTRACE_SINGLESTEP per instruction (ground truth).
 *   Path B — block-step + replay: PTRACE_SINGLEBLOCK to each boundary + a real
 *            GETREGS(+memory) snapshot there; the interior reconstructed by stepping
 *            Unicorn one instruction at a time, Unicorn seeded from the real boundary
 *            snapshot and its guest mapped AT THE REAL addresses so effective addresses
 *            and values are directly comparable. A coherence CANARY compares Unicorn's
 *            computed end-of-block state to the real next boundary; a mismatch (e.g. a
 *            sibling rewrote a loaded byte) drops the capture to `truncated`.
 * Byte-identical between A and B <=> Unicorn reproduced the real CPU's per-instruction
 * state across the block. That is the bet, isolated.
 *
 * Also implements the F1 region-granularity PURITY static scan (syscall / sysenter /
 * int 0x80 / rdtsc / rdtscp / rdrand / rdseed / cpuid) that decides block-step+replay
 * vs single-step fallback, and demonstrates it on pure and impure fixtures.
 *
 * Self-skips cleanly where ptrace is blocked (seccomp/yama), PTRACE_SINGLEBLOCK is
 * non-functional (some hypervisors mask DEBUGCTL.BTF), or the build lacks Unicorn /
 * Capstone. Linux x86-64 only; every other host prints a SKIP and returns 0.
 *
 * Build (host), see the findings doc; essentially:
 *   gcc -O2 -D_GNU_SOURCE -DASMTEST_HAVE_CAPSTONE -DSPIKE_HAVE_UNICORN -Iinclude \
 *       $(pkg-config --cflags capstone) \
 *       examples/blockstep_value_spike.c src/dataflow.c src/dataflow_operands.c \
 *       $(pkg-config --libs capstone) -lunicorn -o blockstep_value_spike
 */
#define _GNU_SOURCE

#include "asmtest_valtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fixtures (x86-64 SysV; rdi = arg0, rsi = arg1)                       */
/* ------------------------------------------------------------------ */

/*
 * loop_poly(n): the PRIMARY oracle fixture. A register-only accumulator loop whose
 * every taken back-edge is one block-step stop but SIX single-step stops, so it makes
 * both the byte-identical claim (arithmetic + DEFINED flags across many steps) and the
 * stop-count reduction (~mean block length) measurable on one region. Deliberately
 * uses only instructions with fully-DEFINED flag results (add/cmp) and flag-neutral
 * moves/lea — never `xor`-to-zero, whose AF is architecturally UNDEFINED and is the
 * documented emulator-vs-silicon divergence boundary (see the findings doc).
 *
 *   0x00 mov  eax, 0            b8 00 00 00 00     acc = 0          (no flags)
 *   0x05 mov  ecx, 0            b9 00 00 00 00     i   = 0          (no flags)
 *   0x0a lea  rdx, [rcx+rcx*2]  48 8d 14 49        tmp = i*3        (no flags)  <-.loop
 *   0x0e add  rax, rdx          48 01 d0           acc += tmp       (all flags)
 *   0x11 add  rax, 1            48 83 c0 01        acc += 1         (all flags)
 *   0x15 add  rcx, 1            48 83 c1 01        i   += 1         (all flags)
 *   0x19 cmp  rcx, rdi          48 39 f9           i < n ?          (all flags)
 *   0x1c jl   .loop             7c ec              taken back-edge  (reads OF/SF)
 *   0x1e ret                    c3                 pop [rsp]        (mem read)
 *
 * returns sum_{i=0}^{n-1} (3*i + 1) = 3*n*(n-1)/2 + n. For n=50 => 3725.
 */
static const uint8_t loop_poly[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00, /* 0x00 mov eax, 0            */
    0xb9, 0x00, 0x00, 0x00, 0x00, /* 0x05 mov ecx, 0            */
    0x48, 0x8d, 0x14, 0x49,       /* 0x0a lea rdx, [rcx+rcx*2]  */
    0x48, 0x01, 0xd0,             /* 0x0e add rax, rdx          */
    0x48, 0x83, 0xc0, 0x01,       /* 0x11 add rax, 1            */
    0x48, 0x83, 0xc1, 0x01,       /* 0x15 add rcx, 1            */
    0x48, 0x39, 0xf9,             /* 0x19 cmp rcx, rdi          */
    0x7c, 0xec,                   /* 0x1c jl  0x0a              */
    0xc3,                         /* 0x1e ret                  */
};

/*
 * mem_chain(a,b): the SECONDARY fixture — a straight-line load-after-store + register
 * move chain (the shipped ptrace/emulator oracle's df_chain). Exercises the MEMORY
 * path of the replay (a store, a dependent load, and the ret's stack pop) with NO
 * flag-affecting instruction at all, so it isolates memory-value fidelity. One block
 * (ret terminator): 6 single-step stops collapse to 1 block-step stop. Returns a+b.
 *
 *   0x00 mov rax, rdi
 *   0x03 mov [rsp-8], rax
 *   0x08 mov rcx, [rsp-8]
 *   0x0d lea rdx, [rcx+rsi]
 *   0x11 mov rax, rdx
 *   0x14 ret
 */
static const uint8_t mem_chain[] = {
    0x48, 0x89, 0xf8,             /* 0x00 mov rax, rdi       */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax   */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]   */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi] */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx       */
    0xc3,                         /* 0x14 ret                */
};

/* Impure fixtures for the purity classifier (SCANNED, not executed). Each carries one
 * of the F1-listed OS-interacting / nondeterministic instructions. */
static const uint8_t imp_syscall[] = {
    0xb8, 0x27, 0x00, 0x00, 0x00, /* mov eax, 39 (getpid) */
    0x0f, 0x05,                   /* syscall              */
    0xc3,                         /* ret                  */
};
static const uint8_t imp_rdtsc[] = {
    0x0f, 0x31, /* rdtsc */
    0xc3,       /* ret   */
};
static const uint8_t imp_cpuid[] = {
    0xb8, 0x00, 0x00, 0x00, 0x00, /* mov eax, 0 */
    0x0f, 0xa2,                   /* cpuid      */
    0xc3,                         /* ret        */
};
static const uint8_t imp_rdrand[] = {
    0x0f, 0xc7, 0xf0, /* rdrand eax */
    0xc3,             /* ret        */
};
static const uint8_t imp_int80[] = {
    0xb8, 0x14, 0x00, 0x00, 0x00, /* mov eax, 20 */
    0xcd, 0x80,                   /* int 0x80    */
    0xc3,                         /* ret         */
};

/* ------------------------------------------------------------------ */
/* Tiny TAP-ish harness                                                 */
/* ------------------------------------------------------------------ */
static int checks, failures;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - " : "not ok %d - ", checks);                     \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/* Purity static scan (needs only Capstone; shared by both mains).      */
/* ------------------------------------------------------------------ */
#ifdef ASMTEST_HAVE_CAPSTONE
#include <capstone/capstone.h>

typedef struct {
    int impure;
    const char *reason; /* the offending mnemonic, or NULL                   */
    uint64_t off;       /* its region offset when impure                     */
} purity_t;

/*
 * F1 region-granularity purity classifier: linearly disassemble the region's
 * (time-correct) bytes ONCE and flag the first OS-interacting / nondeterministic
 * instruction — syscall / sysenter / int 0x80 / rdtsc / rdtscp / rdrand / rdseed /
 * cpuid. A PURE region gets block-step + replay; an IMPURE one falls back to direct
 * single-step (F2 lifts that). Classifying per region UP FRONT sidesteps the ordering
 * trap the plan names: block-step advances the REAL process, so a syscall inside a
 * block has already retired by the boundary — never emulate through it.
 *
 * Limitation (documented): a linear sweep can misdecode bytes past an unconditional
 * indirect branch or embedded data; a production classifier should sweep the method's
 * real instruction extents (it has the JIT method-map) or follow decoded control flow.
 * The fixtures here are straight byte streams, so the linear sweep is exact for them.
 */
static purity_t scan_purity(const uint8_t *code, size_t len) {
    purity_t r = {0, NULL, 0};
    csh h;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK)
        return r;
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn *insn = cs_malloc(h);
    uint64_t addr = 0;
    const uint8_t *p = code;
    size_t remaining = len;
    while (remaining > 0 && cs_disasm_iter(h, &p, &remaining, &addr, insn)) {
        int hit = 0;
        const char *why = NULL;
        switch (insn->id) {
        case X86_INS_SYSCALL:
            hit = 1;
            why = "syscall";
            break;
        case X86_INS_SYSENTER:
            hit = 1;
            why = "sysenter";
            break;
        case X86_INS_RDTSC:
            hit = 1;
            why = "rdtsc";
            break;
        case X86_INS_RDTSCP:
            hit = 1;
            why = "rdtscp";
            break;
        case X86_INS_RDRAND:
            hit = 1;
            why = "rdrand";
            break;
        case X86_INS_RDSEED:
            hit = 1;
            why = "rdseed";
            break;
        case X86_INS_CPUID:
            hit = 1;
            why = "cpuid";
            break;
        case X86_INS_INT:
            /* int 0x80 is the legacy syscall gate; the plan names it specifically. */
            if (insn->detail->x86.op_count == 1 &&
                insn->detail->x86.operands[0].type == X86_OP_IMM &&
                insn->detail->x86.operands[0].imm == 0x80) {
                hit = 1;
                why = "int 0x80";
            }
            break;
        default:
            break;
        }
        if (hit) {
            r.impure = 1;
            r.reason = why;
            r.off = insn->address;
            break;
        }
    }
    cs_free(insn, 1);
    cs_close(&h);
    return r;
}

static void run_purity_case(const char *name, const uint8_t *code, size_t len,
                            int want_impure, const char *want_reason) {
    purity_t p = scan_purity(code, len);
    if (want_impure)
        CHECK(p.impure && want_reason && p.reason &&
                  strcmp(p.reason, want_reason) == 0,
              "purity: %s classified IMPURE (%s) -> single-step fallback", name,
              p.reason ? p.reason : "?");
    else
        CHECK(!p.impure,
              "purity: %s classified PURE -> block-step+replay eligible", name);
}
#endif /* ASMTEST_HAVE_CAPSTONE */

/* ================================================================== */
/* The live block-step + replay probe (Linux x86-64 + Unicorn + Capstone) */
/* ================================================================== */
#if defined(__linux__) && defined(__x86_64__) &&                               \
    defined(ASMTEST_HAVE_CAPSTONE) && defined(SPIKE_HAVE_UNICORN)

#include <errno.h>
#include <signal.h>
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

/* TF (single-step) and RF (resume) are DEBUG-MECHANISM bits, not program semantics: a
 * PTRACE_SINGLESTEP tracee surfaces TF in its GETREGS eflags where a SINGLEBLOCK
 * boundary need not, and a #DB can set RF. The byte-identical comparison is about
 * PROGRAM values, so both paths mask these out of every captured eflags via the one
 * shared gp_value below. See the findings doc for why this normalization is honest. */
#define EFLAGS_STEP_BITS                                                       \
    0x00010100ULL /* TF (bit 8) | RF (bit 16)                */
#define EFLAGS_ARITH_MASK                                                      \
    0x00000CD5ULL /* CF PF AF ZF SF DF OF — the canary's mask */

/* ------------------------------------------------------------------ */
/* Shared capture core: one record stream, two value sources           */
/* ------------------------------------------------------------------ */

/* A pluggable memory reader — Path A reads the tracee (process_vm_readv), Path B the
 * Unicorn guest. Returns 1 iff all n bytes were read. */
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
 * folding sub-registers to the container. EFLAGS is masked of the debug-stepping bits
 * so both value sources agree. Returns false for regs not in this file (vector/segment
 * selectors), whose value is then left uncaptured — none of the spike fixtures hit it. */
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
 * index*scale + disp, with the RIP-relative next-instruction fixup. Mirrors
 * src/dataflow_ptrace.c resolve_ea so the core is drop-in. */
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
        return; /* the spike fixtures are all <= 8-byte memory operands */
    uint8_t buf[8] = {0};
    if (!c->mr(c->mr_ctx, r->addr, buf, sz))
        return;
    r->value = 0;
    memcpy(&r->value, buf, sz);
    r->value_valid = true;
}

/* Finalize the current step's deferred WRITE values from the POST-instruction state
 * (`regs`) and append the step. Mirrors dataflow_ptrace.c finalize_step exactly. */
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

/* Open the step at `regs->rip`: enumerate its read/write set, capture READ values from
 * this PRE-state (registers via gp_value, memory via the reader at the resolved EA),
 * resolve store addresses, defer WRITE values. Returns the instruction byte length (so
 * the replay driver can detect a taken transfer as rip != pc+len). Mirrors
 * dataflow_ptrace.c open_step. */
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

/* One captured step: finalize the previous step with this stop's regs (its post-state)
 * and open the current one (its pre-state). The SINGLE point of contact for both paths.
 * Returns the current instruction's byte length. */
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
/* ptrace helpers                                                       */
/* ------------------------------------------------------------------ */
typedef long (*fn6_t)(long, long, long, long, long, long);

static void *map_exec(const uint8_t *code, size_t len) {
    void *ex = mmap(NULL, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ex == MAP_FAILED)
        return NULL;
    memcpy(ex, code, len);
    if (mprotect(ex, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(ex, len);
        return NULL;
    }
    return ex;
}

/* A stack shared by BOTH children (single-step and block-step). Two independent forks
 * would otherwise call the fixture at DIFFERENT absolute stack addresses (ASLR + the
 * differing caller-frame depth of run_singlestep vs run_blockstep_replay), so every
 * rsp/rbp read and stack effective-address would differ between the traces — a
 * process-identity artifact, not a replay failure. Mapping ONE stack in the parent and
 * letting both forks inherit it (COW), then switching each child onto it at the SAME
 * offset, makes the absolute addresses identical too, so the byte-identical assertion
 * is LITERAL rather than "modulo the stack base". */
static void *ensure_fixed_stack(void) {
    static void *top = NULL;
    if (top == NULL) {
        size_t sz = 256 * 1024;
        void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            return NULL;
        top = (void *)((uintptr_t)p + sz); /* page-aligned => 16-aligned */
    }
    return top;
}

/* Fork a self-owned tracee that TRACEME's, SIGSTOPs for us, then calls the fixture at
 * `base` ON THE SHARED FIXED STACK. Returns the pid with the tracee stopped at the
 * initial SIGSTOP (SETOPTIONS EXITKILL applied), or -1. (Only rdi/rsi are wired — the
 * spike fixtures take at most two integer args.) */
static pid_t spawn_tracee(uint64_t base, const long *a) {
    void *stack_top = ensure_fixed_stack();
    if (stack_top == NULL)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        /* Switch onto the shared stack and `call` the fixture, so its entry rsp (and
         * therefore every stack address it computes) is identical across both forks;
         * the return address `call` pushes is a fixed code address in this binary,
         * identical across forks too. Then restore and _exit. */
        register uint64_t rfn __asm__("r14") = base;
        register uint64_t rtop __asm__("r13") = (uint64_t)(uintptr_t)stack_top;
        register long ra0 __asm__("r12") = a[0];
        register long ra1 __asm__("rbx") = a[1];
        __asm__ volatile("mov %%rsp, %%r15\n\t"
                         "mov %%r13, %%rsp\n\t"
                         "mov %%r12, %%rdi\n\t"
                         "mov %%rbx, %%rsi\n\t"
                         "call *%%r14\n\t"
                         "mov %%r15, %%rsp\n\t"
                         :
                         : "r"(rfn), "r"(rtop), "r"(ra0), "r"(ra1)
                         : "rax", "rcx", "rdx", "rdi", "rsi", "r8", "r9", "r10",
                           "r11", "r15", "cc", "memory");
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
/* Path A — true single-step (ground truth)                            */
/* ------------------------------------------------------------------ */
/* Returns 0 on a clean region capture, -1 on a ptrace/setup failure. *stops receives
 * the number of IN-REGION single-step stops (== captured steps). */
static int run_singlestep(const uint8_t *code, size_t len, const long *args,
                          int nargs, asmtest_valtrace_t *vt, long *result,
                          uint64_t *stops) {
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];
    void *ex = map_exec(code, len);
    if (ex == NULL)
        return -1;
    uint64_t base = (uint64_t)(uintptr_t)ex;
    pid_t pid = spawn_tracee(base, a);
    if (pid < 0) {
        munmap(ex, len);
        return -1;
    }
    vt->mem_space = AT_LOC_MEM_ABS;

    cap_ctx c;
    memset(&c, 0, sizeof c);
    c.vt = vt;
    c.code = code;
    c.code_len = len;
    c.base = base;
    c.mr = mr_tracee;
    c.mr_ctx = &pid;

    int rc = -1, entered = 0;
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
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP)
            break;
        if (++guard > (1u << 20))
            break;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
            break;
        uint64_t pc = regs.rip;
        if (pc >= base && pc < base + len) {
            capture_at(&c,
                       &regs); /* finalize prev (post) + open current (pre) */
            entered = 1;
            nstop++;
        } else if (entered) {
            if (c.have_cur)
                finalize_step(&c, &regs); /* last step's post-state */
            if (result != NULL)
                *result = (long)regs.rax;
            rc = 0;
            break;
        }
    }
    if (stops != NULL)
        *stops = nstop;
    free(c.cur.v);
    reap(pid);
    munmap(ex, len);
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

/* The coherence CANARY: does Unicorn's computed end-of-block state agree with the real
 * next boundary? Compares the GP regs, rip, rsp and the arithmetic flags (ignoring
 * IF/reserved/debug bits). A mismatch means the replay's inputs diverged from reality
 * (e.g. a sibling rewrote a loaded byte) and the block must drop to `truncated`. */
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

/* Replay one straight-line block through Unicorn, capturing each interior instruction,
 * until a TAKEN transfer whose target is `pc_next`. Returns 0 on the clean terminator,
 * 1 if Unicorn branched somewhere OTHER than the real boundary (divergence), -1 on a
 * Unicorn fault / undecodable step. The terminating branch is left as the open step
 * (finalized by the next block's seed, or at region exit). */
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

/* Returns 0 on a clean region capture, -1 on setup/ptrace failure, 1 when the coherence
 * canary fired (vt->truncated set). *stops = IN-REGION block-step boundary stops.
 * inject_block >= 0 corrupts Unicorn's seed rax at that 0-based block index to SIMULATE
 * a concurrent-divergence input, demonstrating the canary. */
static int run_blockstep_replay(const uint8_t *code, size_t len,
                                const long *args, int nargs,
                                asmtest_valtrace_t *vt, long *result,
                                uint64_t *stops, int inject_block) {
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    /* All resources declared up front so the single `out:` cleanup is always valid. */
    int ret = -1;
    void *ex = NULL;
    pid_t pid = -1;
    uc_engine *uc = NULL;
    uint8_t *stackbuf = NULL;
    uint64_t win_base = 0, win_size = 0;
    cap_ctx c;
    memset(&c, 0, sizeof c);
    struct user_regs_struct S_cur;
    int at_entry = 0;
    uint64_t nstop = 1; /* the entry boundary itself */
    int block_idx = 0;
    long real_result = 0;

    ex = map_exec(code, len);
    if (ex == NULL)
        return -1;
    uint64_t base = (uint64_t)(uintptr_t)ex;
    pid = spawn_tracee(base, a);
    if (pid < 0) {
        munmap(ex, len);
        return -1;
    }
    vt->mem_space = AT_LOC_MEM_ABS;

    c.vt = vt;
    c.code = code;
    c.code_len = len;
    c.base = base;
    c.mr = mr_uc;

    /* 1) Block-step through the entry glue until the first IN-REGION stop = boundary 0. */
    for (uint64_t g = 0; g < (1u << 16); g++) {
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

    /* 2) Stand up Unicorn: code mapped at the REAL base, a stack window mapped at the
     *    REAL rsp, both copied from the (stopped) tracee, and the GP file seeded from
     *    the entry boundary. Real addresses => effective addresses + values compare
     *    directly against the single-step ground truth. */
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
            memcpy(cb + (base - code_base), code, len);
        }
        uc_mem_write(uc, code_base, cb, code_size);
        free(cb);

        win_base = (S_cur.rsp - 0x1000) & ~0xFFFULL;
        win_size =
            0x3000; /* [rsp-0x1000, rsp+0x2000) covers rsp-8 and the ret slot */
        if (uc_mem_map(uc, win_base, win_size, UC_PROT_READ | UC_PROT_WRITE) !=
            UC_ERR_OK)
            goto out;
        stackbuf = (uint8_t *)malloc(win_size);
        if (stackbuf == NULL)
            goto out;
    }
    c.mr_ctx = uc;

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
            vt->truncated = true; /* ended before a clean region return */
            ret = 1;
            goto out;
        }
        if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTRAP)
            goto out;
        struct user_regs_struct S_next;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &S_next) != 0)
            goto out;
        int in_region_next = (S_next.rip >= base && S_next.rip < base + len);

        /* The region-EXIT terminator (a ret / tail jump) transfers to a real address
         * OUTSIDE the mapped region — Unicorn would UC_ERR_FETCH_UNMAPPED trying to
         * fetch there even under count=1. Map a one-page landing pad at that boundary
         * so the terminator's data effects (pop rsp, [rsp] read) execute and Unicorn
         * halts cleanly AT the boundary. Best-effort: an already-mapped page is fine. */
        if (!in_region_next) {
            uint64_t pad = S_next.rip & ~0xFFFULL;
            uc_mem_map(uc, pad, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
        }

        /* Seed this block from the real starting boundary + its memory snapshot, then
         * replay it in Unicorn. (The seed also finalizes the previous block's branch
         * with real ground truth on the next capture_at.) */
        uc_set_regs(uc, &S_cur);
        uc_mem_write(uc, win_base, stackbuf, win_size);
        if (block_idx == inject_block) {
            uint64_t bad =
                S_cur.rax + 1; /* simulate a diverging replay input */
            uc_reg_write(uc, UC_X86_REG_RAX, &bad);
        }

        int brc = step_block(&c, uc, S_next.rip);
        if (brc < 0)
            goto out;

        struct user_regs_struct ucR;
        uc_get_regs(uc, &ucR);
        if (brc == 1 || !regs_coherent(&ucR, &S_next)) {
            vt->truncated =
                true; /* divergence detected: never silently wrong */
            ret = 1;
            goto out;
        }

        if (!in_region_next) {
            /* Region return: finalize the terminating step with Unicorn's post-state. */
            if (c.have_cur)
                finalize_step(&c, &ucR);
            real_result = (long)S_next.rax;
            ret = 0;
            break;
        }

        /* Advance to the next block: resnapshot the tracee's stack (it is stopped at
         * S_next now) so the next block's loads see ground-truth memory. */
        nstop++;
        block_idx++;
        if (!mr_tracee(&pid, win_base, stackbuf, win_size))
            memset(stackbuf, 0, win_size);
        S_cur = S_next;
    }

    if (result != NULL)
        *result = real_result;
    if (stops != NULL)
        *stops = nstop;

out:
    free(c.cur.v);
    free(stackbuf);
    if (uc != NULL)
        uc_close(uc);
    if (pid > 0)
        reap(pid);
    if (ex != NULL)
        munmap(ex, len);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Byte-identical comparison of two value traces                        */
/* ------------------------------------------------------------------ */
static int rec_eq(const at_val_rec_t *x, const at_val_rec_t *y) {
    return x->kind == y->kind && x->reg == y->reg && x->base == y->base &&
           x->index == y->index && x->scale == y->scale && x->disp == y->disp &&
           x->addr == y->addr && x->size == y->size &&
           x->is_write == y->is_write && x->value_valid == y->value_valid &&
           x->wide == y->wide && x->wide_off == y->wide_off &&
           x->value == y->value && x->step == y->step;
}

/* Returns 1 if the two traces are identical; on a mismatch prints the first differing
 * element and returns 0. *rawmemcmp receives whether a literal memcmp of the record
 * arrays also matched (a stricter check that would also catch padding). */
static int traces_identical(const asmtest_valtrace_t *A,
                            const asmtest_valtrace_t *B, int *rawmemcmp) {
    if (rawmemcmp)
        *rawmemcmp = 0;
    if (A->steps_len != B->steps_len) {
        printf("#   steps_len differ: A=%zu B=%zu\n", A->steps_len,
               B->steps_len);
        return 0;
    }
    if (A->recs_len != B->recs_len) {
        printf("#   recs_len differ: A=%zu B=%zu\n", A->recs_len, B->recs_len);
        return 0;
    }
    for (size_t i = 0; i < A->steps_len; i++)
        if (A->insn_off[i] != B->insn_off[i]) {
            printf("#   insn_off[%zu] differ: A=0x%llx B=0x%llx\n", i,
                   (unsigned long long)A->insn_off[i],
                   (unsigned long long)B->insn_off[i]);
            return 0;
        }
    for (size_t i = 0; i < A->recs_len; i++)
        if (!rec_eq(&A->recs[i], &B->recs[i])) {
            const at_val_rec_t *x = &A->recs[i], *y = &B->recs[i];
            printf("#   rec[%zu] differ (step %u kind %d reg %u write %d): "
                   "A.value=0x%llx valid=%d  B.value=0x%llx valid=%d\n",
                   i, x->step, (int)x->kind, x->reg, (int)x->is_write,
                   (unsigned long long)x->value, (int)x->value_valid,
                   (unsigned long long)y->value, (int)y->value_valid);
            return 0;
        }
    if (A->wide_len != B->wide_len ||
        (A->wide_len && memcmp(A->wide, B->wide, A->wide_len) != 0)) {
        printf("#   wide[] differ\n");
        return 0;
    }
    if (rawmemcmp)
        *rawmemcmp =
            (A->recs_len == B->recs_len) &&
            memcmp(A->recs, B->recs, A->recs_len * sizeof(at_val_rec_t)) == 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Functional PTRACE_SINGLEBLOCK + ptrace probes (hang-proof)          */
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

/* ------------------------------------------------------------------ */
/* Drivers                                                              */
/* ------------------------------------------------------------------ */

/* One fixture, both ways: byte-identical assertion + stop-count reduction. */
static void run_identity_case(const char *name, const uint8_t *code, size_t len,
                              const long *args, int nargs, long want_result) {
    asmtest_valtrace_t *A = asmtest_valtrace_new(4096, 65536, 4096);
    asmtest_valtrace_t *B = asmtest_valtrace_new(4096, 65536, 4096);
    if (A == NULL || B == NULL) {
        CHECK(0, "%s: valtrace_new", name);
        goto done;
    }
    long ra = 0, rb = 0;
    uint64_t sa = 0, sb = 0;
    int rca = run_singlestep(code, len, args, nargs, A, &ra, &sa);
    int rcb = run_blockstep_replay(code, len, args, nargs, B, &rb, &sb, -1);
    if (rca != 0 || rcb != 0) {
        CHECK(0, "%s: capture failed (single-step rc=%d, block-replay rc=%d)",
              name, rca, rcb);
        goto done;
    }
    CHECK(ra == want_result && rb == want_result,
          "%s: both paths returned %ld (A=%ld B=%ld)", name, want_result, ra,
          rb);
    CHECK(!A->truncated && !B->truncated, "%s: neither trace truncated", name);

    int raw = 0;
    int same = traces_identical(A, B, &raw);
    CHECK(same,
          "%s: block-step+replay value trace is BYTE-IDENTICAL to single-step "
          "(%zu steps, %zu records)",
          name, A->steps_len, A->recs_len);
    if (same)
        printf("#   raw memcmp of record arrays also identical: %s\n",
               raw ? "yes"
                   : "no (semantic fields identical; struct padding differs)");

    double ratio = sb ? (double)sa / (double)sb : 0.0;
    CHECK(sb < sa,
          "%s: block-step CUT the stop count %llu -> %llu (%.2fx fewer "
          "in-region stops)",
          name, (unsigned long long)sa, (unsigned long long)sb, ratio);
    printf("#   single-step in-region stops = %llu; block-step in-region stops "
           "= %llu; "
           "captured steps = %zu\n",
           (unsigned long long)sa, (unsigned long long)sb, A->steps_len);
done:
    asmtest_valtrace_free(A);
    asmtest_valtrace_free(B);
}

/* Exit criterion 4: an injected divergence (simulating a sibling rewriting an input
 * between snapshot and execution) is DETECTED at the next real boundary and the capture
 * drops to `truncated`, never silently wrong. */
static void run_canary_case(void) {
    long args[1] = {50};
    asmtest_valtrace_t *B = asmtest_valtrace_new(4096, 65536, 4096);
    if (B == NULL) {
        CHECK(0, "canary: valtrace_new");
        return;
    }
    long rb = 0;
    uint64_t sb = 0;
    int rc =
        run_blockstep_replay(loop_poly, sizeof loop_poly, args, 1, B, &rb, &sb,
                             1 /* corrupt block index 1's seed rax */);
    CHECK(rc == 1 && B->truncated,
          "canary: injected replay-input divergence DETECTED, capture -> "
          "truncated "
          "(rc=%d truncated=%d)",
          rc, (int)B->truncated);
    asmtest_valtrace_free(B);
}

int main(void) {
    printf("# F1 spike: block-step + emulator-replay value optimization "
           "(increment 0)\n");

    /* Purity classifier needs no ptrace/Unicorn — run first so it reports even on a
     * locked-down host. */
    run_purity_case("loop_poly", loop_poly, sizeof loop_poly, 0, NULL);
    run_purity_case("mem_chain", mem_chain, sizeof mem_chain, 0, NULL);
    run_purity_case("imp_syscall", imp_syscall, sizeof imp_syscall, 1,
                    "syscall");
    run_purity_case("imp_rdtsc", imp_rdtsc, sizeof imp_rdtsc, 1, "rdtsc");
    run_purity_case("imp_cpuid", imp_cpuid, sizeof imp_cpuid, 1, "cpuid");
    run_purity_case("imp_rdrand", imp_rdrand, sizeof imp_rdrand, 1, "rdrand");
    run_purity_case("imp_int80", imp_int80, sizeof imp_int80, 1, "int 0x80");

    if (!probe_ptrace()) {
        printf("# SKIP live block-step/replay: ptrace unavailable here "
               "(seccomp/yama)\n");
        goto report;
    }
    if (!probe_singleblock()) {
        printf(
            "# SKIP live block-step/replay: PTRACE_SINGLEBLOCK non-functional "
            "(BTF masked by the hypervisor)\n");
        goto report;
    }

    {
        long a1[1] = {50};
        run_identity_case("loop_poly(n=50)", loop_poly, sizeof loop_poly, a1, 1,
                          3725);
        long a2[2] = {7, 5};
        run_identity_case("mem_chain(7,5)", mem_chain, sizeof mem_chain, a2, 2,
                          12);
    }
    run_canary_case();

report:
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    else
        printf("# all %d checks passed\n", checks);
    return failures ? 1 : 0;
}

#else /* not (Linux x86-64 + Capstone + Unicorn): purity-only / skip */

int main(void) {
    printf("# F1 spike: block-step + emulator-replay value optimization "
           "(increment 0)\n");
#ifdef ASMTEST_HAVE_CAPSTONE
    run_purity_case("loop_poly", loop_poly, sizeof loop_poly, 0, NULL);
    run_purity_case("mem_chain", mem_chain, sizeof mem_chain, 0, NULL);
    run_purity_case("imp_syscall", imp_syscall, sizeof imp_syscall, 1,
                    "syscall");
    run_purity_case("imp_rdtsc", imp_rdtsc, sizeof imp_rdtsc, 1, "rdtsc");
    run_purity_case("imp_cpuid", imp_cpuid, sizeof imp_cpuid, 1, "cpuid");
    run_purity_case("imp_rdrand", imp_rdrand, sizeof imp_rdrand, 1, "rdrand");
    run_purity_case("imp_int80", imp_int80, sizeof imp_int80, 1, "int 0x80");
    printf("# SKIP live block-step/replay: built without Unicorn "
           "(SPIKE_HAVE_UNICORN) "
           "or off Linux x86-64\n");
#else
    (void)loop_poly;
    (void)mem_chain;
    (void)imp_syscall;
    (void)imp_rdtsc;
    (void)imp_cpuid;
    (void)imp_rdrand;
    (void)imp_int80;
    printf("# SKIP: built without Capstone (ASMTEST_HAVE_CAPSTONE)\n");
#endif
    printf("1..%d\n", checks);
    return failures ? 1 : 0;
}

#endif
