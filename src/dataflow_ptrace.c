/*
 * dataflow_ptrace.c — the SCOPED PTRACE L0 producer (Phase 3, goal (a)): captures
 * a REAL per-step value trace off a LIVE, single-stepped process, OUT OF BAND, and
 * fills the same asmtest_valtrace_t the emulator producer (dataflow_emu.c) fills, so
 * the shared L1 def-use + L2 slicer (dataflow.c) work UNCHANGED on live captures.
 * See docs/internal/plans/data-flow-tracing-plan.md, Phase 3.
 *
 * Where the emulator REPLAYS the bytes under Unicorn, this OBSERVES the real CPU: a
 * tracer parent single-steps a tracee that calls the registered code, and at each stop
 * reads the tracee's architectural state directly —
 *   - GP registers / rflags / rip from one PTRACE_GETREGS per step (the pre-state of
 *     the about-to-execute instruction);
 *   - XMM via PTRACE_GETFPREGS, YMM high halves via PTRACE_GETREGSET(NT_X86_XSTATE);
 *   - memory operands by resolving the effective address base+index*scale+disp (with
 *     fs_base/gs_base segment resolution for gs:/fs:-relative .NET-style TLS, and the
 *     next-instruction fixup for RIP-relative EAs) from the just-read registers, then
 *     process_vm_readv on the TRACEE's address space.
 * The Phase 0 operand enumerator (asmtest_operands) says WHICH registers/memory each
 * instruction reads and writes; this file only reads their live values.
 *
 * Two entry points, one shared stepping+capture core:
 *   - asmtest_dataflow_ptrace_run    forks a self-owned tracee (PTRACE_TRACEME) and
 *                                    single-steps it — the deterministic CI driver.
 *   - asmtest_dataflow_ptrace_attach ATTACHES to a live, independently-running victim
 *                                    (PTRACE_SEIZE), single-steps the scoped region,
 *                                    then DETACHES so the target SURVIVES (the plan's
 *                                    goal-(a) "attach to a running process; survive
 *                                    the crash-safe detach").
 *
 * Value TIMING mirrors the emulator exactly so the two producers are cross-validatable
 * on the same fixture (the plan's oracle idea): a register/memory READ is captured at
 * the instruction's own stop (source state); a WRITE's value is deferred one stop and
 * filled from the post-instruction state (register file for a reg, a re-read of the
 * resolved address for a store). The def-use graph the analysis layer builds is keyed
 * on LOCATIONS (register ids / effective addresses), so a live slice matches the
 * emulator's on a deterministic region even though the concrete addresses differ.
 *
 * Cost model (per SCOPED region, NOT per whole run — plan Phase 3 exit criterion "cost
 * is documented per-region, not per-run"): single-stepping is the expensive tier by
 * design (~10^3-10^5x native for the stepped window, per the analysis note). Each
 * in-region instruction costs one PTRACE_SINGLESTEP round trip + one PTRACE_GETREGS;
 * add one PTRACE_GETFPREGS/GETREGSET only on a step touching a vector operand, plus one
 * process_vm_readv per memory operand. The cost therefore scales with
 * (in-region steps x operands-per-step), bounded by `max_insns` and DFP_STEP_BACKSTOP —
 * and it is INDEPENDENT of the untraced remainder of the process: the tracer steps only
 * [code, code+len), so the victim's work outside the region costs the tracer nothing
 * (that whole-run capture is a non-goal here; it is the DynamoRIO tier's job). This is
 * why the tier is scoped to a `using` region rather than a whole run.
 *
 * The per-step value capture needs a stop-by-stop register/memory hook that the
 * offset-only ptrace backend (src/ptrace_backend.c, asmtest_ptrace_*) does not expose,
 * so this is a SELF-CONTAINED single-step client with its own fork/attach +
 * PTRACE_SINGLESTEP loop — exactly as dataflow_emu.c is a self-contained Unicorn client
 * rather than an extension of emu.c. (A future refactor could factor the offset
 * backend's step loop into a shared per-step callback; that seam does not exist yet.)
 * Arch availability is the compile-time guard below; runtime ptrace permission (seccomp)
 * surfaces as a DF_PTRACE_ETRACE the caller self-skips on.
 *
 * Scope + supported target: a deterministic, single-threaded routine of up to six
 * integer arguments, executed from an inherited executable mapping; the recorded region
 * is [code, code+len) and value capture is bounded to it. Call-outs to helpers OUTSIDE
 * the region are STEPPED OVER at native speed (Increment 2): when the just-executed
 * in-region instruction was a `call` whose return address lands back in the region, the
 * producer runs the callee to that return address via an int3 breakpoint + PTRACE_CONT
 * (recording NOTHING over the helper) and then resumes in-region single-stepping — so a
 * non-leaf routine is traced across its helper calls. The step-over is NOT re-entrancy
 * aware (it resumes at the FIRST arrival at the return address); a W^X callee page that
 * refuses the int3, or a callee that never returns, truncates honestly rather than
 * hanging. Whole-run capture is a non-goal by design (see the plan).
 *
 * Requires Capstone (the operand enumerator) and Linux x86-64; off-platform / without
 * Capstone the entry points return DF_PTRACE_ENOSYS and callers self-skip.
 *
 * The return codes are declared here (NOT in a public asmtest_*.h header): like the
 * emulator producer, a value-trace PRODUCER is a tier, not part of the shared
 * asmtest_valtrace.h sink API. The producer's test re-declares the entry points and
 * these codes the same way it re-declares asmtest_dataflow_emu_run.
 */
#define _GNU_SOURCE

#include <sys/types.h> /* pid_t — used by the attach_pid signature on every platform */

#include "asmtest_codeimage.h" /* asmtest_codeimage_t / _bytes_at (Increment 3 versioned decode) */
#include "asmtest_valtrace.h"

/* Return codes from the scoped ptrace producers (kept in step with the test's copy). */
#define DF_PTRACE_OK     0 /* returned cleanly; a complete scoped trace     */
#define DF_PTRACE_FAULT  1 /* routine faulted; a partial trace is filled    */
#define DF_PTRACE_EINVAL (-1) /* bad arguments                               */
#define DF_PTRACE_ENOSYS (-3) /* off Linux x86-64 / no Capstone: self-skip   */
#define DF_PTRACE_ETRACE                                                       \
    (-4) /* fork/ptrace/wait failure (seccomp): self-skip */

#if defined(__linux__) && defined(__x86_64__) && defined(ASMTEST_HAVE_CAPSTONE)

#include <asm/prctl.h> /* ARCH_SET_GS */
#include <capstone/capstone.h>
#include <dirent.h> /* /proc/<pid>/task enumeration (Increment 4 worker targeting) */
#include <elf.h> /* NT_X86_XSTATE */
#include <errno.h>
#include <signal.h>
#include <stdio.h> /* snprintf for the /proc/<pid>/task path (Increment 4) */
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/* Hard backstop on TOTAL steps (prologue/libc to the region entry, plus the region):
 * bounds wall time if the tracee never enters or never leaves [code, code+len). */
#define DFP_STEP_BACKSTOP (1u << 20)

/* waitpid on a SEIZE'd foreign target's THREADS (clone children) needs __WALL to report
 * their ptrace-stops — Increment 4 enumerates every thread and steps whichever WORKER
 * enters the region, not just the leader. */
#ifndef __WALL
#define __WALL 0x40000000
#endif

/* ------------------------------------------------------------------ */
/* Register-file value reads                                           */
/* ------------------------------------------------------------------ */

/* Map a Capstone x86 register id to its 64-bit container value in the just-read GP
 * register file. A 32/16/8-bit sub-register folds to its 64-bit container (the record's
 * size carries the real operand width, exactly as the emulator producer does). Returns
 * false for a register not in the GP/flags/rip file (segment selectors, vector regs),
 * whose value is then simply left uncaptured. */
static bool gp_value(const struct user_regs_struct *r, uint32_t reg,
                     uint64_t *out) {
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        *out = r->rax;
        return true;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        *out = r->rbx;
        return true;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        *out = r->rcx;
        return true;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        *out = r->rdx;
        return true;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        *out = r->rsi;
        return true;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        *out = r->rdi;
        return true;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        *out = r->rbp;
        return true;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        *out = r->rsp;
        return true;
    case X86_REG_R8:
        *out = r->r8;
        return true;
    case X86_REG_R9:
        *out = r->r9;
        return true;
    case X86_REG_R10:
        *out = r->r10;
        return true;
    case X86_REG_R11:
        *out = r->r11;
        return true;
    case X86_REG_R12:
        *out = r->r12;
        return true;
    case X86_REG_R13:
        *out = r->r13;
        return true;
    case X86_REG_R14:
        *out = r->r14;
        return true;
    case X86_REG_R15:
        *out = r->r15;
        return true;
    case X86_REG_RIP:
        *out = r->rip;
        return true;
    case X86_REG_EFLAGS:
        *out = r->eflags;
        return true;
    default:
        return false;
    }
}

/* Vector operand width from its Capstone register class: XMM = 16, YMM = 32, else 0
 * (not a vector reg). cs_regs_access reports vector regs with no size, so the producer
 * assigns it from the register class. */
static uint16_t vec_width(uint32_t reg) {
    if (reg >= X86_REG_XMM0 && reg <= X86_REG_XMM0 + 31)
        return 16;
    if (reg >= X86_REG_YMM0 && reg <= X86_REG_YMM0 + 31)
        return 32;
    return 0;
}

/* Read the live 128-bit XMM[idx] (0..15; higher indices need AVX-512 xstate, skipped)
 * from PTRACE_GETFPREGS. Returns 1 on success. */
static int read_xmm(pid_t pid, int idx, uint8_t out[16]) {
    if (idx < 0 || idx > 15)
        return 0;
    struct user_fpregs_struct fp;
    if (ptrace(PTRACE_GETFPREGS, pid, NULL, &fp) != 0)
        return 0;
    memcpy(out, &fp.xmm_space[idx * 4], 16);
    return 1;
}

/* Read the live 256-bit YMM[idx]: the low 128 from the SSE area (GETFPREGS) and the
 * high 128 from the AVX component of the XSAVE image (PTRACE_GETREGSET/NT_X86_XSTATE).
 * The YMM_Hi128 component sits at the standard xsave offset 576 (16 bytes per reg).
 * Returns 1 on success (high half zeroed if the image is too short — a no-AVX host). */
static int read_ymm(pid_t pid, int idx, uint8_t out[32]) {
    if (idx < 0 || idx > 15)
        return 0;
    if (!read_xmm(pid, idx, out))
        return 0;
    uint8_t xs[8192];
    struct iovec iov = {xs, sizeof xs};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_X86_XSTATE, &iov) !=
        0) {
        memset(out + 16, 0, 16);
        return 1;
    }
    const size_t ymmh = 576 + (size_t)idx * 16;
    if (iov.iov_len >= ymmh + 16)
        memcpy(out + 16, xs + ymmh, 16);
    else
        memset(out + 16, 0, 16);
    return 1;
}

/* Read `n` bytes of the TRACEE's memory at `addr` into `buf` (process_vm_readv, with a
 * PTRACE_PEEKDATA fallback for a hardened /proc). Returns 1 iff all n bytes were read. */
static int child_read(pid_t pid, uint64_t addr, void *buf, size_t n) {
    struct iovec l = {buf, n};
    struct iovec r = {(void *)(uintptr_t)addr, n};
    if (process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)n)
        return 1;
    unsigned char *d = (unsigned char *)buf;
    size_t i = 0;
    while (i < n) {
        uintptr_t a = (uintptr_t)addr + i;
        uintptr_t al = a & ~(sizeof(long) - 1);
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid, (void *)al, NULL);
        if (w == -1 && errno != 0)
            return 0;
        size_t off = a - al;
        size_t chunk = sizeof(long) - off;
        if (chunk > n - i)
            chunk = n - i;
        memcpy(d + i, (unsigned char *)&w + off, chunk);
        i += chunk;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Per-step scratch + value fills                                      */
/* ------------------------------------------------------------------ */

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
    pid_t pid;
    asmtest_valtrace_t *vt;
    const uint8_t *code;
    size_t code_len;
    uint64_t base;
    int have_cur;
    uint64_t cur_off;
    recbuf cur;
    /* Foreign-attach disposition (attach_pid path). `foreign` = the tracee is a process we
     * did NOT create, so dfp_step_loop must NEVER kill it on an error/truncation exit —
     * it leaves it trap-stopped (*left_stopped=1) and the caller PTRACE_DETACHes it (with
     * `detach_sig` forwarded on a fault) so it SURVIVES. `pre_positioned` = the tracee is
     * already trap-stopped AT the region entry (run_to), so the loop must examine THAT stop
     * as the first instruction's pre-state instead of single-stepping past it. Both default
     * 0 (the fork `_run` + the forked-victim `attach` paths are byte-unchanged). */
    int foreign;
    int pre_positioned;
    int detach_sig;
    /* Optional versioned decode (Increment 3): when `img` != NULL, each step's operand
     * read/write set is enumerated against the code-image bytes live at (base+off) as of
     * sequence `when` (asmtest_codeimage_bytes_at) instead of the c->code snapshot — so a
     * region the JIT patched / freed / had its address reused mid-capture still decodes the
     * instruction that was live at that step, not the final bytes. `img` == NULL / `when` ==
     * 0 (the fork `_run`, forked-victim `attach`, and native `attach_pid` paths all default
     * it there) keeps the c->code snapshot path byte-for-byte. */
    asmtest_codeimage_t *img;
    uint64_t when;
} dfp_ctx;

/* Fill a record's value from a register (GP inline, XMM/YMM spilled to wide[]) at the
 * CURRENT register/vector state. Used for reads (source state, at the instruction's own
 * stop) and to finalize deferred writes (destination state, at the next stop). */
static void fill_reg_value(dfp_ctx *c, const struct user_regs_struct *regs,
                           at_val_rec_t *r) {
    uint16_t w = vec_width(r->reg);
    if (w == 0) {
        uint64_t val;
        if (gp_value(regs, r->reg, &val)) {
            r->value = val;
            r->value_valid = true;
        }
        return;
    }
    r->size = w;
    uint8_t buf[32];
    int ok = (w == 16) ? read_xmm(c->pid, (int)(r->reg - X86_REG_XMM0), buf)
                       : read_ymm(c->pid, (int)(r->reg - X86_REG_YMM0), buf);
    if (!ok)
        return;
    if (w <= 8) {
        memcpy(&r->value, buf, w);
        r->value_valid = true;
        return;
    }
    size_t woff = asmtest_valtrace_stash_wide(c->vt, buf, w);
    if (woff != (size_t)-1) {
        r->wide = true;
        r->wide_off = (uint32_t)woff;
        r->value_valid = true;
    }
}

/* Fill a memory record's value by re-reading the resolved address from the tracee (a
 * store's value is only in memory AFTER the instruction; a load's before). */
static void fill_mem_value(dfp_ctx *c, at_val_rec_t *r) {
    uint16_t sz = r->size;
    if (sz == 0 || sz > 32)
        return;
    uint8_t buf[32];
    if (!child_read(c->pid, r->addr, buf, sz))
        return;
    if (sz <= 8) {
        r->value = 0;
        memcpy(&r->value, buf, sz);
        r->value_valid = true;
        return;
    }
    size_t woff = asmtest_valtrace_stash_wide(c->vt, buf, sz);
    if (woff != (size_t)-1) {
        r->wide = true;
        r->wide_off = (uint32_t)woff;
        r->value_valid = true;
    }
}

/* Resolve a memory operand's effective address from the just-read registers:
 * seg_base + base + index*scale + disp, with fs_base/gs_base for fs:/gs:, and the
 * x86 RIP-relative fixup (the EA is relative to the NEXT instruction, so add the
 * instruction's byte length when the base register is RIP). */
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
            /* RIP-relative EAs are computed from the address of the FOLLOWING
             * instruction, not this one — gp_value returned the current rip. */
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

/* Finalize the current step: fill every deferred WRITE value from the post-instruction
 * state (`regs` is the next stop's register file, i.e. this step's destination state),
 * then append the step to the value trace. */
static void finalize_step(dfp_ctx *c, const struct user_regs_struct *regs) {
    for (size_t i = 0; i < c->cur.n; i++) {
        at_val_rec_t *r = &c->cur.v[i];
        if (!r->is_write || r->value_valid)
            continue;
        if (r->kind == AT_LOC_REG)
            fill_reg_value(c, regs, r);
        else
            fill_mem_value(c, r); /* addr resolved when the step opened */
    }
    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v, c->cur.n);
    c->have_cur = 0;
    c->cur.n = 0;
}

/* Open the step at region offset `off`: enumerate its read/write set (Phase 0), capture
 * READ values now (source state), resolve store addresses now, and defer WRITE values to
 * the next stop. `regs` is this instruction's pre-state. */
static void open_step(dfp_ctx *c, const struct user_regs_struct *regs,
                      uint64_t off) {
    c->cur.n = 0;
    c->cur_off = off;
    c->have_cur = 1;

    at_val_rec_t rd[64], wr[64];
    size_t nr = 64, nw = 64;

    /* Decode byte source. Default: the live snapshot c->code (offset `off`). Versioned
     * (Increment 3): when a code-image is provided, decode the TIME-CORRECT bytes the
     * recorder had live at this PC (c->base + off) as of c->when — the "swap the byte
     * source, keep the step loop" decoupling asmtest_ptrace_trace_attached_versioned uses,
     * so a region patched/relocated mid-capture still enumerates the RIGHT instruction. If
     * the query fails (PC not tracked at `when`) fall back to the live snapshot. */
    const uint8_t *dec = c->code;
    size_t dec_len = c->code_len;
    size_t dec_off = off;
    if (c->img != NULL) {
        const uint8_t *vb = NULL;
        size_t avail = 0;
        if (asmtest_codeimage_bytes_at(c->img,
                                       (const void *)(uintptr_t)(c->base + off),
                                       c->when, &vb, &avail) == ASMTEST_CI_OK &&
            vb != NULL && avail > 0) {
            dec = vb;
            dec_len = avail;
            dec_off = 0;
        }
    }

    /* The enumerator returns the instruction byte length — needed for the RIP-relative
     * EA fixup (the EA is relative to the next instruction). */
    size_t insn_len = asmtest_operands(ASMTEST_ARCH_X86_64, dec, dec_len,
                                       dec_off, rd, &nr, wr, &nw);

    for (size_t i = 0; i < nr; i++) {
        at_val_rec_t r = rd[i];
        if (r.kind == AT_LOC_REG) {
            fill_reg_value(c, regs, &r);
        } else {
            r.addr = resolve_ea(regs, &r, insn_len);
            fill_mem_value(c, &r); /* load value is in memory pre-instruction */
        }
        recbuf_push(&c->cur, &r);
    }
    for (size_t i = 0; i < nw; i++) {
        at_val_rec_t r = wr[i];
        r.value_valid = false; /* filled at the next stop (destination state) */
        if (r.kind != AT_LOC_REG)
            r.addr = resolve_ea(regs, &r, insn_len); /* EA from pre-insn regs */
        recbuf_push(&c->cur, &r);
    }
}

/* ------------------------------------------------------------------ */
/* The shared single-step driver                                       */
/* ------------------------------------------------------------------ */

/* End a step loop on a DIRTY outcome (fault / backstop / bound / ptrace-wait failure). The
 * kill-vs-detach decision lives HERE so both callers share it: a FOREIGN tracee (attach_pid)
 * is never killed — it is left trap-stopped (*left_stopped=1) for the caller to PTRACE_DETACH
 * (with `fatal_sig` forwarded, 0 = none) so it SURVIVES; a self-owned tracee (the fork `_run`
 * / forked-victim `attach` paths) is killed+reaped here, exactly as before. Returns `code`. */
static int dfp_dirty_exit(dfp_ctx *c, int code, int fatal_sig,
                          int *left_stopped) {
    if (c->foreign) {
        c->detach_sig = fatal_sig;
        *left_stopped = 1;
        return code;
    }
    int status;
    kill(c->pid, SIGKILL);
    waitpid(c->pid, &status, 0);
    return code;
}

/* Increment 5 — the signal split. A SIGTRAP stop this file's OWN PTRACE_SINGLESTEP produced
 * ordinarily reports si_code TRAP_TRACE (an x86 #DB single-step) or TRAP_BRKPT (a step
 * completing across a syscall — MEASURED in cli/asmspy_engine.c, not an app breakpoint). If
 * the single-stepped instruction was instead a byte the TARGET planted itself — a JIT/
 * debugger int3 self-check (V8's IMMEDIATE_CRASH, a CLR breakpoint) or its own hardware
 * breakpoint — the kernel reports si_code SI_KERNEL (the force_sig path a real #BP exception
 * takes) or TRAP_HWBKPT instead. This function is called ONLY from the PTRACE_SINGLESTEP
 * branch of dfp_step_loop below, where this file never plants an int3 of its own (the
 * region-entry / call-out breakpoints are planted-and-restored entirely inside dfp_run_to /
 * dfp_run_to_multi before the tracee is ever handed to the single-step loop), so a positive
 * here is unambiguously the TARGET's own trap, never ours. A GETSIGINFO failure or any other
 * si_code defaults to 0 (treat as our step) — the same discard-not-deliver behaviour every
 * SIGTRAP got before this increment, so a false negative is a regression to the prior
 * behaviour, not a new hazard. Mirrors cli/asmspy_engine.c's sigtrap_is_app. */
static int dfp_sigtrap_is_app(pid_t tid) {
    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, tid, NULL, &si) != 0)
        return 0;
    return si.si_code == SI_KERNEL || si.si_code == TRAP_HWBKPT;
}

/* dfp_run_to (defined below) runs the tracee at NATIVE SPEED to an address via an int3
 * breakpoint + PTRACE_CONT, rewinding rip to that address. The call-out step-over reuses
 * it to run OVER a helper to its return; forward-declared here so dfp_step_loop can call
 * it before its definition. */
static int dfp_run_to(pid_t pid, uint64_t base);

/* Decide whether the region exit just observed is a CALL-OUT to a helper OUTSIDE the
 * region rather than the region's own return. `last_off` is the last in-region
 * instruction — the one whose single-step carried PC out of [base_ip, base_ip+code_len);
 * `regs` is the post-step register file, i.e. the callee's ENTRY state on a real call-out.
 * Mirrors ptrace_backend.c's classify_region_exit: the last instruction must be a `call`
 * (direct OR indirect) whose return address (its fall-through) lands back inside the
 * region, AND the return address the call pushed on the stack must confirm it. On a
 * call-out, *resume_off gets the in-region resume offset (the call's fall-through).
 *
 * The call is decoded with Capstone directly (a self-contained cs_open/cs_close, detail
 * on for the CS_GRP_CALL query) rather than via the shared operand enumerator, keeping the
 * step-over local to this producer; region exits are rare (once per call-out / once at the
 * return), so this is off the per-step hot path. On x86-64 CS_GRP_CALL tags both `call
 * rel32` and `call r/m64`, so this is exact for the file's compile-time arch. */
static int dfp_is_callout(dfp_ctx *c, uint64_t base_ip, size_t code_len,
                          uint64_t last_off,
                          const struct user_regs_struct *regs,
                          uint64_t *resume_off) {
    if (last_off >= c->code_len)
        return 0;
    csh h;
    if (cs_open(CS_ARCH_X86, CS_MODE_64, &h) != CS_ERR_OK)
        return 0;
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON); /* groups[] needs detail mode */
    cs_insn *insn = NULL;
    size_t n = cs_disasm(h, c->code + last_off, c->code_len - (size_t)last_off,
                         base_ip + last_off, 1, &insn);
    int is_call = 0;
    size_t call_len = 0;
    if (n > 0) {
        is_call = cs_insn_group(h, &insn[0], CS_GRP_CALL);
        call_len = insn[0].size;
        cs_free(insn, n);
    }
    cs_close(&h);
    if (!is_call || call_len == 0)
        return 0;
    uint64_t ret_off = last_off + call_len;
    if (ret_off >= code_len)
        return 0; /* the call's fall-through is outside the region: a tail exit */
    /* Confirm the exit is really this call's call-out: a real call leaves its
     * fall-through (return) address on top of the stack at the callee's entry. */
    uint64_t pushed = 0;
    if (!child_read(c->pid, regs->rsp, &pushed, sizeof pushed) ||
        pushed != base_ip + ret_off)
        return 0;
    *resume_off = ret_off;
    return 1;
}

/* Drive PTRACE_SINGLESTEP over [base_ip, base_ip+code_len) of an already trace-stopped
 * tracee (c->pid), capturing each in-region step's values. On a CLEAN region exit the
 * tracee is left ptrace-stopped just past the region, `result` (if non-NULL) receives
 * rax, and *left_stopped is set to 1 — the caller then resumes it (the fork owner runs
 * it to _exit; an attached victim is DETACHed so it SURVIVES). On any other outcome the
 * disposition is dfp_dirty_exit's (self-owned → kill+reap; foreign → left stopped for
 * detach). When c->pre_positioned the tracee is already trap-stopped AT base_ip (run_to),
 * so the first iteration examines THAT stop as the entry instruction's pre-state instead of
 * single-stepping past it. Returns a DF_PTRACE_* code. */
static int dfp_step_loop(dfp_ctx *c, uint64_t base_ip, size_t code_len,
                         uint64_t max_insns, long *result, int *left_stopped) {
    pid_t pid = c->pid;
    int status = 0, entered = 0, pending_sig = 0;
    uint64_t recorded = 0, total = 0;
    int skip_step =
        c->pre_positioned; /* examine the run_to entry stop before stepping */
    *left_stopped = 0;

    for (;;) {
        if (skip_step) {
            skip_step =
                0; /* first iteration only: the tracee is already at the entry */
        } else {
            if (ptrace(PTRACE_SINGLESTEP, pid, NULL,
                       (void *)(uintptr_t)pending_sig) != 0)
                return dfp_dirty_exit(c, DF_PTRACE_ETRACE, 0, left_stopped);
            pending_sig = 0;
            if (waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR)
                    continue;
                return dfp_dirty_exit(c, DF_PTRACE_ETRACE, 0, left_stopped);
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                /* Tracee gone (already reaped by this waitpid) — nothing to detach. If we
                 * entered but never observed the return, the capture is incomplete; if we
                 * never entered, this is a setup failure. */
                if (c->have_cur)
                    c->vt->truncated = true;
                return entered ? DF_PTRACE_OK : DF_PTRACE_ETRACE;
            }
            if (!WIFSTOPPED(status))
                continue;
            if (WSTOPSIG(status) != SIGTRAP) {
                int sig = WSTOPSIG(status);
                if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                    sig == SIGFPE) {
                    c->vt->truncated = true;
                    return dfp_dirty_exit(c, DF_PTRACE_FAULT, sig,
                                          left_stopped);
                }
                pending_sig =
                    sig; /* unrelated signal: forward and keep stepping */
                continue;
            }
            /* The signal split (Increment 5): the instruction we just single-stepped was
             * the TARGET's own trap, not an ordinary step completion. Precise per-
             * instruction capture cannot safely continue across it — re-arming the trap
             * flag immediately after delivering a signal fires a #DB on the handler's
             * first instruction, where SIGTRAP is masked by default, which the kernel
             * escalates to SIG_DFL and KILLS the target (the asmspy engines' MEASURED
             * finding, cli/asmspy_engine.c). So the scoped capture ends here, honestly
             * truncated, and the trap is DELIVERED via the crash-safe detach path
             * (fatal_sig = SIGTRAP) so the runtime's own signal machinery — a debugger
             * protocol handshake, an abort/crash handler, a safepoint redirect — runs
             * exactly as it would untraced, the out-of-process analog of the DynamoRIO
             * tier's DR_SIGNAL_DELIVER. Finalize whatever step was already open (its
             * post-state is these regs) before handing off. */
            if (dfp_sigtrap_is_app(pid)) {
                struct user_regs_struct aregs;
                if (c->have_cur) {
                    if (ptrace(PTRACE_GETREGS, pid, NULL, &aregs) == 0)
                        finalize_step(c, &aregs);
                    else
                        c->cur.n =
                            0; /* can't read post-state: drop, don't guess */
                    c->have_cur = 0;
                }
                c->vt->truncated = true;
                return dfp_dirty_exit(c, DF_PTRACE_FAULT, SIGTRAP,
                                      left_stopped);
            }
            if (++total > DFP_STEP_BACKSTOP) {
                c->vt->truncated = true;
                return dfp_dirty_exit(c, DF_PTRACE_ETRACE, 0, left_stopped);
            }
        }

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
            return dfp_dirty_exit(c, DF_PTRACE_ETRACE, 0, left_stopped);
        uint64_t pc = regs.rip;

        if (pc >= base_ip && pc < base_ip + code_len) {
            /* In region: this stop is the post-state of the previous step (finalize it)
             * and the pre-state of this one (open it). */
            if (c->have_cur)
                finalize_step(c, &regs);
            entered = 1;
            open_step(c, &regs, pc - base_ip);
            if (max_insns != 0 && ++recorded >= max_insns) {
                /* Bounded scope reached: append what we have (its writes stay
                 * unfilled), flag truncated, and leave the tracee. */
                asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v, c->cur.n);
                c->have_cur = 0;
                c->vt->truncated = true;
                return dfp_dirty_exit(c, DF_PTRACE_ETRACE, 0, left_stopped);
            }
        } else if (entered) {
            /* Left the region. This stop's registers are the previous in-region step's
             * DESTINATION state, so finalize it: for a CALL-OUT this is the immediate
             * post-call, callee-entry state (RSP decremented, return address pushed — so
             * the call's own writes are captured correctly); for a RETURN it is the
             * post-return state. */
            uint64_t last_off = c->cur_off;
            if (c->have_cur)
                finalize_step(c, &regs);

            /* CALL-OUT step-over: if the last in-region instruction was a `call` whose
             * return address (its fall-through) lands back inside the region, this is a
             * call to a helper OUTSIDE the region, not the routine's own return. Run the
             * callee at NATIVE SPEED to that return address (int3 breakpoint + PTRACE_CONT
             * via dfp_run_to, recording NOTHING over the helper) and then resume in-region
             * single-stepping — the out-of-process analog of ptrace_backend.c's
             * classify_region_exit (EXIT_CALLOUT_RESUMED). RE-ENTRANCY CAVEAT: the
             * step-over resumes at the FIRST arrival at the return address and is NOT
             * re-entrancy aware — a helper that re-enters the region (a callback /
             * tiering-OSR stub) and passes the return address before the outer call
             * returns resumes in the NESTED invocation. */
            uint64_t resume_off = 0;
            if (dfp_is_callout(c, base_ip, code_len, last_off, &regs,
                               &resume_off)) {
                /* Bound the step-over with the existing whole-run backstop so a runaway
                 * sequence of call-outs self-truncates rather than looping unbounded. */
                if (++total > DFP_STEP_BACKSTOP ||
                    dfp_run_to(pid, base_ip + resume_off) != 0) {
                    /* Over budget, or the callee never reached its return address (it
                     * exited, faulted, or its return byte could not be trapped — e.g. a
                     * W^X helper page): truncate HONESTLY rather than hang. */
                    c->vt->truncated = true;
                    return dfp_dirty_exit(c, DF_PTRACE_ETRACE, 0, left_stopped);
                }
                /* Stopped AT the return address (in region); examine THAT stop as the
                 * resume instruction's pre-state without single-stepping past it. */
                skip_step = 1;
                continue;
            }

            /* A genuine return (or a tail-jump out of the region): record the return
             * value and hand the still-stopped tracee back to the caller for its
             * resume/detach policy. */
            if (result != NULL)
                *result = (long)regs.rax;
            *left_stopped = 1;
            return DF_PTRACE_OK;
        }
        /* else: prologue / libc before the region — keep stepping, capture nothing. */
    }
}

/* ------------------------------------------------------------------ */
/* Entry points                                                        */
/* ------------------------------------------------------------------ */

typedef long (*fn6_t)(long, long, long, long, long, long);

/* A MAP_SHARED rendezvous page for the attach path: the tracer releases the victim into
 * the region (go) and the victim signals it ran PAST the region after detach (survived). */
typedef struct {
    volatile int go;
    volatile int survived;
} dfp_ctl;

/* Map the routine's bytes into an inherited executable page (RW then R+X, so it works on
 * a W^X kernel). Returns the mapping or NULL. */
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

/* Breakpoint-and-continue an already-SEIZEd, INTERRUPT-stopped tracee to `base` and leave it
 * trap-stopped THERE (original byte restored, rip rewound past the int3) — the region-entry
 * precondition dfp_step_loop's pre_positioned path expects. int3 via PTRACE_POKETEXT only (a
 * native r-x or r-w-x region: ptrace writes bypass the page write bit); the DR0 hardware-
 * breakpoint fallback for a W^X JIT heap that cannot be poked is a later increment. Non-SIGTRAP
 * signals the target takes on the way to the region are forwarded. Returns 0 on success, -1 if
 * the target exited or a ptrace call failed. */
static int dfp_run_to(pid_t pid, uint64_t base) {
    errno = 0;
    long orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)(uintptr_t)base, NULL);
    if (orig == -1 && errno != 0)
        return -1;
    long trap = (orig & ~0xffL) | 0xccL;
    if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)base,
               (void *)(uintptr_t)trap) != 0)
        return -1;

    int status = 0, pending = 0;
    for (;;) {
        if (ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)pending) != 0)
            return -1;
        pending = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            return -1; /* target gone before reaching the region */
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) == SIGTRAP)
            break;
        pending =
            WSTOPSIG(status); /* forward an unrelated signal, keep running */
    }

    /* Restore the original byte and rewind rip from base+1 (past the int3) back to base. */
    if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)base,
               (void *)(uintptr_t)orig) != 0)
        return -1;
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        return -1;
    if (regs.rip != base + 1)
        return -1; /* stopped somewhere other than our breakpoint */
    regs.rip = base;
    if (ptrace(PTRACE_SETREGS, pid, NULL, &regs) != 0)
        return -1;
    return 0;
}

int asmtest_dataflow_ptrace_run(const uint8_t *code, size_t code_len,
                                const long *args, int nargs, uint64_t max_insns,
                                uint64_t gs_base, long *result,
                                asmtest_valtrace_t *vt) {
    if (vt == NULL || code == NULL || code_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_PTRACE_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    void *ex = map_exec(code, code_len);
    if (ex == NULL)
        return DF_PTRACE_ETRACE;
    const uint64_t base_ip = (uint64_t)(uintptr_t)ex;

    pid_t pid = fork();
    if (pid < 0) {
        munmap(ex, code_len);
        return DF_PTRACE_ETRACE;
    }
    if (pid == 0) {
        /* Tracee: enable tracing, optionally set the GS base (gs:-relative fixtures),
         * stop for the parent, then call the routine. _exit avoids atexit/stdio. */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        if (gs_base != 0)
            syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)gs_base);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)base_ip)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }

    dfp_ctx c;
    memset(&c, 0, sizeof c);
    c.pid = pid;
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base_ip;

    int status = 0;
    /* Initial post-fork SIGSTOP handshake (retry across an unrelated EINTR). */
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w >= 0 || errno != EINTR)
            break;
    }
    if (!WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        munmap(ex, code_len);
        return DF_PTRACE_ETRACE; /* seccomp / ptrace blocked → caller self-skips */
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    int left_stopped = 0;
    int rc =
        dfp_step_loop(&c, base_ip, code_len, max_insns, result, &left_stopped);
    if (left_stopped) {
        /* Self-owned tracee: let it run to _exit so it SURVIVES the capture, then reap. */
        ptrace(PTRACE_CONT, pid, NULL, NULL);
        if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
    }
    free(c.cur.v);
    munmap(ex, code_len);
    return rc;
}

int asmtest_dataflow_ptrace_attach(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   uint64_t max_insns, uint64_t gs_base,
                                   long *result, int *survived,
                                   asmtest_valtrace_t *vt) {
    if (survived != NULL)
        *survived = 0;
    if (vt == NULL || code == NULL || code_len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return DF_PTRACE_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    void *ex = map_exec(code, code_len);
    if (ex == NULL)
        return DF_PTRACE_ETRACE;
    const uint64_t base_ip = (uint64_t)(uintptr_t)ex;

    dfp_ctl *ctl = (dfp_ctl *)mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) {
        munmap(ex, code_len);
        return DF_PTRACE_ETRACE;
    }
    ctl->go = 0;
    ctl->survived = 0;

    pid_t pid = fork();
    if (pid < 0) {
        munmap(ex, code_len);
        munmap(ctl, sizeof *ctl);
        return DF_PTRACE_ETRACE;
    }
    if (pid == 0) {
        /* Live victim: runs INDEPENDENTLY (no PTRACE_TRACEME) — the tracer SEIZEs it.
         * It spins until the attached tracer releases it into the region, runs the
         * routine, then records that it ran PAST the region (post-detach) and exits. */
        if (gs_base != 0)
            syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)gs_base);
        while (ctl->go == 0) {
            /* busy-wait; ctl->go is volatile so the load is re-issued each turn */
        }
        volatile long r = ((fn6_t)base_ip)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        ctl->survived =
            1; /* proves the target SURVIVED the crash-safe detach */
        _exit(0);
    }

    dfp_ctx c;
    memset(&c, 0, sizeof c);
    c.pid = pid;
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base_ip;

    int status = 0, rc = DF_PTRACE_ETRACE, left_stopped = 0;

    /* ATTACH to the live victim (SEIZE, not fork+TRACEME). EXITKILL guards against a
     * tracer crash mid-capture leaving an orphaned spinning victim; it is cleared by the
     * clean DETACH below on the success path. */
    if (ptrace(PTRACE_SEIZE, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL) !=
        0)
        goto kill_out;
    /* Stop the running victim so we can single-step it (PTRACE_INTERRUPT → event-stop). */
    if (ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0)
        goto kill_out;
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w >= 0 || errno != EINTR)
            break;
    }
    if (!WIFSTOPPED(status))
        goto kill_out;

    /* Attached and stopped in the spin loop (go is still 0, so the victim has NOT yet
     * entered the region). Release it; the next single-steps carry it into the region. */
    ctl->go = 1;

    rc = dfp_step_loop(&c, base_ip, code_len, max_insns, result, &left_stopped);
    if (left_stopped) {
        /* Crash-safe two-phase detach: the victim is already trap-stopped just past the
         * region, so PTRACE_DETACH (no pending signal) resumes it FREE (untraced) and it
         * SURVIVES — it then runs its post-region code and exits on its own. */
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        waitpid(pid, &status, 0); /* reap the survivor once it _exits */
        if (survived != NULL)
            *survived = ctl->survived;
    }
    free(c.cur.v);
    munmap(ex, code_len);
    munmap(ctl, sizeof *ctl);
    return rc;

kill_out:
    /* Attach failed before the victim could be released: it would spin forever on go=0,
     * so kill+reap it rather than leak it. */
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    free(c.cur.v);
    munmap(ex, code_len);
    munmap(ctl, sizeof *ctl);
    return DF_PTRACE_ETRACE;
}

/* Increment 3: attach_pid with an OPTIONAL versioned decode. Identical to the native
 * attach_pid below, except each step's operands are enumerated against `img`'s time-correct
 * bytes at sequence `when` instead of the live process_vm_readv snapshot — so a JIT that
 * patched / freed / reused the code at `base` still decodes the instruction live at that
 * step (the addr-channel `(base,len,version)` records feed the code-image; each step is
 * attributed to its method+version by the pure asmtest_method_attribute post-pass over the
 * captured trace). `img` == NULL degrades to the exact native attach_pid behaviour. */
int asmtest_dataflow_ptrace_attach_pid_versioned(pid_t pid, uint64_t base,
                                                 size_t code_len,
                                                 uint64_t max_insns,
                                                 asmtest_codeimage_t *img,
                                                 uint64_t when, long *result,
                                                 asmtest_valtrace_t *vt) {
    if (vt == NULL || pid <= 0 || base == 0 || code_len == 0)
        return DF_PTRACE_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

    int status = 0, left_stopped = 0, rc = DF_PTRACE_ETRACE;

    /* SEIZE the already-running FOREIGN target. NO PTRACE_O_EXITKILL — a tracer crash must not
     * take a process we do not own down with it. INTERRUPT it into an event-stop so we can
     * drive it; a ptrace-permission failure (seccomp / yama) surfaces as ETRACE and the caller
     * self-skips. */
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0)
        return DF_PTRACE_ETRACE;
    if (ptrace(PTRACE_INTERRUPT, pid, NULL, NULL) != 0)
        goto detach;
    for (;;) {
        pid_t w = waitpid(pid, &status, 0);
        if (w >= 0 || errno != EINTR)
            break;
    }
    if (!WIFSTOPPED(status))
        goto detach;

    /* Run the target to the region entry (breakpoint-cont-rewind), then read the region's
     * bytes FROM the target for the operand enumerator (process_vm_readv, not fork inheritance
     * — we did not create this process). */
    if (dfp_run_to(pid, base) != 0)
        goto detach;
    uint8_t *code = (uint8_t *)malloc(code_len);
    if (code == NULL)
        goto detach;
    if (!child_read(pid, base, code, code_len)) {
        free(code);
        goto detach;
    }

    dfp_ctx c;
    memset(&c, 0, sizeof c);
    c.pid = pid;
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base;
    c.foreign = 1;        /* NEVER kill the target on any exit */
    c.pre_positioned = 1; /* already trap-stopped AT base (dfp_run_to) */
    c.img = img; /* optional versioned decode source (NULL = live snapshot) */
    c.when = when;

    rc = dfp_step_loop(&c, base, code_len, max_insns, result, &left_stopped);
    free(c.cur.v);
    free(code);

    /* Crash-safe detach: the target is trap-stopped (just past the region on a clean exit, or
     * left in place on a dirty one) — PTRACE_DETACH resumes it FREE (untraced) so it SURVIVES,
     * forwarding a fault signal (c.detach_sig, 0 otherwise) so the target handles its own
     * fault. It is NOT our child, so we do not waitpid it. If it already exited mid-capture
     * (left_stopped == 0) the detach is a harmless no-op. */
    if (left_stopped)
        ptrace(PTRACE_DETACH, pid, NULL, (void *)(uintptr_t)c.detach_sig);
    return rc;

detach:
    /* Setup failed after SEIZE: detach cleanly, leaving the target running + alive. */
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return DF_PTRACE_ETRACE;
}

/* The native live-attach path (Increment 1) is the versioned path with NO code-image: the
 * live process_vm_readv snapshot is the decode source. Kept byte-compatible so its callers
 * (its test suite; the asmspy --dataflow engine) are unaffected by Increment 3. */
int asmtest_dataflow_ptrace_attach_pid(pid_t pid, uint64_t base,
                                       size_t code_len, uint64_t max_insns,
                                       long *result, asmtest_valtrace_t *vt) {
    return asmtest_dataflow_ptrace_attach_pid_versioned(
        pid, base, code_len, max_insns, NULL, 0, result, vt);
}

/* ================================================================== */
/* Increment 4 — worker-thread targeting (managed methods run off the  */
/* leader).                                                            */
/*                                                                     */
/* The attach paths above single-step the LEADER: dfp_run_to plants an  */
/* int3 and PTRACE_CONTs `pid` (the thread-group leader) alone, so a    */
/* routine that only ever runs on a WORKER thread is never entered —    */
/* the leader hits the region-entry breakpoint never, and the capture   */
/* hangs / comes back empty. Managed runtimes run almost everything on  */
/* worker threads, so this path must not depend on the leader.          */
/*                                                                     */
/* It SEIZEs EVERY thread of the target (enumerating /proc/<pid>/task), */
/* plants ONE int3 at the region entry — a software breakpoint is a     */
/* memory patch, so it is shared across all threads of the (single)     */
/* address space — releases them all, and single-steps WHICHEVER thread */
/* first traps at the entry. That thread is targeted by the tid         */
/* `waitpid` reports (its OWN kernel tid), NEVER the leader `pid`:       */
/* stepping the leader while a worker is the one at the breakpoint is    */
/* the getpid-vs-gettid mistargeting that was a real fatal-SIGTRAP bug   */
/* on HotSpot (src/hwtrace.c targets SYS_gettid, not getpid, for exactly */
/* this reason). The other threads are DETACHed so they run FREE at full */
/* speed while the one entering thread is stepped. An optional           */
/* `only_tid` (0 = any) pins exactly one thread: a non-target thread     */
/* that reaches the entry first is single-stepped OVER the shared        */
/* breakpoint and released, so the bp stays armed for the pinned tid.    */
/*                                                                     */
/* Caveat carried in-code: PTRACE_O_TRACECLONE is deliberately NOT set. */
/* The siblings are detached to run free, so we must not inherit their   */
/* future clone-stops (which would strand a never-waited thread). The    */
/* re-scan in dfp_seize_all catches every thread that exists when we     */
/* attach — the worker-targeting scope here; a thread SPAWNED after the  */
/* scan that enters the region during the brief entry-catch window is    */
/* untracked (Increment 5's JIT entry pairs this with a re-arm loop).    */
/* ================================================================== */

typedef struct {
    pid_t *v;
    size_t n, cap;
} dfp_thrset;

static int dfp_thrset_has(const dfp_thrset *s, pid_t tid) {
    for (size_t i = 0; i < s->n; i++)
        if (s->v[i] == tid)
            return 1;
    return 0;
}

/* Append `tid` (idempotent). Returns 1 on success, 0 on allocation failure. */
static int dfp_thrset_add(dfp_thrset *s, pid_t tid) {
    if (dfp_thrset_has(s, tid))
        return 1;
    if (s->n == s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 16;
        pid_t *nv = (pid_t *)realloc(s->v, nc * sizeof *nv);
        if (nv == NULL)
            return 0;
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n++] = tid;
    return 1;
}

/* Plant a shared int3 at `base` via `tid` (any stopped thread — one address space); the
 * original word is returned in *orig for the later restore. 0 on success, -1 on failure. */
static int dfp_plant_bp(pid_t tid, uint64_t base, long *orig) {
    errno = 0;
    long o = ptrace(PTRACE_PEEKTEXT, tid, (void *)(uintptr_t)base, NULL);
    if (o == -1 && errno != 0)
        return -1;
    long trap = (o & ~0xffL) | 0xccL;
    if (ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
               (void *)(uintptr_t)trap) != 0)
        return -1;
    *orig = o;
    return 0;
}

/* Restore the original byte at `base` (remove the shared int3) via a stopped `tid`. */
static void dfp_remove_bp(pid_t tid, uint64_t base, long orig) {
    ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
           (void *)(uintptr_t)orig);
}

/* Remove the shared int3 via ANY thread we can stop (POKETEXT needs a stopped tracee) —
 * used on failure teardown when the thread that would naturally restore it is gone. */
static void dfp_remove_bp_any(dfp_thrset *set, uint64_t base, long orig) {
    for (size_t i = 0; i < set->n; i++) {
        pid_t tid = set->v[i];
        ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
        int st = 0;
        pid_t w;
        do {
            w = waitpid(tid, &st, __WALL);
        } while (w < 0 && errno == EINTR);
        if (w == tid && WIFSTOPPED(st)) {
            dfp_remove_bp(tid, base, orig);
            return;
        }
    }
}

/* A non-target thread `tid` is trap-stopped at base+1 (it reached the shared entry int3
 * before the pinned thread). Step it PAST the entry WITHOUT losing the breakpoint for the
 * pinned thread: rewind rip to base, restore the original byte, single-step one real
 * instruction, then re-plant the int3 (the caller then CONTs `tid` so it runs on free).
 * Returns 0 on success, -1 if the thread vanished / a ptrace call failed. */
static int dfp_step_over_bp(pid_t tid, uint64_t base, long orig) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0)
        return -1;
    if (regs.rip == base + 1) {
        regs.rip = base;
        if (ptrace(PTRACE_SETREGS, tid, NULL, &regs) != 0)
            return -1;
    }
    dfp_remove_bp(tid, base, orig);
    if (ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL) != 0)
        return -1;
    int st = 0;
    pid_t w;
    do {
        w = waitpid(tid, &st, __WALL);
    } while (w < 0 && errno == EINTR);
    if (w != tid || WIFEXITED(st) || WIFSIGNALED(st))
        return -1; /* it left mid-step: nothing to re-arm, no thread to resume */
    long trap = (orig & ~0xffL) | 0xccL;
    ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
           (void *)(uintptr_t)trap); /* re-arm for the pinned thread */
    return 0;
}

/* SEIZE every thread of `pid` (enumerate /proc/<pid>/task, re-scanning until a full pass
 * adds nothing — closing the race where a not-yet-seized thread spawns another) and
 * INTERRUPT each into a stop. NO PTRACE_O_EXITKILL (a foreign target must outlive us) and
 * NO O_TRACECLONE (the siblings are detached to run free, so their future clone-stops must
 * not become ours). Records every seized tid in `set`. Returns 0 if the leader was seized
 * (the shared int3 can then be planted via the common address space), -1 otherwise. */
static int dfp_seize_all(pid_t pid, dfp_thrset *set) {
    if (ptrace(PTRACE_SEIZE, pid, NULL, NULL) != 0)
        return -1;
    ptrace(PTRACE_INTERRUPT, pid, NULL, NULL);
    if (!dfp_thrset_add(set, pid)) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)pid);
    for (int pass = 0; pass < 16; pass++) {
        DIR *d = opendir(path);
        if (d == NULL)
            break;
        int added = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            pid_t tid = (pid_t)strtol(de->d_name, NULL, 10);
            if (tid <= 0 || dfp_thrset_has(set, tid))
                continue;
            if (ptrace(PTRACE_SEIZE, tid, NULL, NULL) == 0) {
                ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
                if (dfp_thrset_add(set, tid))
                    added = 1;
                else
                    ptrace(PTRACE_DETACH, tid, NULL, NULL);
            }
        }
        closedir(d);
        if (!added)
            break; /* stable: a full pass found no new thread */
    }
    return 0;
}

/* With every thread of the target already SEIZE+INTERRUPT'd (recorded in `set`), plant the
 * shared entry int3, release ALL threads, and run until the FIRST thread to execute `base`
 * traps there — honoring `only_tid` (0 = any). A non-target thread that reaches base first
 * is stepped OVER the breakpoint and released (the bp stays armed for the pinned thread).
 * On success *entering gets that tid, left trap-stopped AT base (byte restored, rip rewound
 * — the pre_positioned precondition dfp_step_loop expects) with the OTHER threads running
 * free; returns 0. On failure the shared bp is removed and -1 returned (caller tears down). */
static int dfp_run_to_multi(dfp_thrset *set, uint64_t base, pid_t only_tid,
                            pid_t *entering) {
    /* 1. Drain each thread's SEIZE/INTERRUPT stop so all are ptrace-stopped (the
     *    precondition for planting the shared int3 and for the release CONT). Drop any
     *    thread that vanished mid-seize. */
    for (size_t i = 0; i < set->n;) {
        pid_t tid = set->v[i];
        int st = 0;
        pid_t w;
        do {
            w = waitpid(tid, &st, __WALL);
        } while (w < 0 && errno == EINTR);
        if (w != tid || WIFEXITED(st) || WIFSIGNALED(st)) {
            set->v[i] = set->v[--set->n]; /* vanished: drop, order irrelevant */
            continue;
        }
        i++;
    }
    if (set->n == 0)
        return -1; /* whole target gone */

    /* 2. Plant the shared int3 (via the first live thread — one address space). */
    long orig = 0;
    if (dfp_plant_bp(set->v[0], base, &orig) != 0)
        return -1;

    /* 3. Release every thread into the region race. */
    for (size_t i = 0; i < set->n; i++)
        ptrace(PTRACE_CONT, set->v[i], NULL, NULL);

    /* 4. Catch the first thread to reach base (respecting only_tid). */
    uint64_t budget = 0;
    for (;;) {
        int st = 0;
        pid_t w = waitpid(-1, &st, __WALL);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* ECHILD: target gone, the bp died with it */
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            for (size_t i = 0; i < set->n; i++)
                if (set->v[i] == w) {
                    set->v[i] = set->v[--set->n];
                    break;
                }
            if (set->n == 0)
                return -1; /* target gone */
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;
        int sig = WSTOPSIG(st);
        if (sig == SIGTRAP) {
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, w, NULL, &regs) != 0) {
                ptrace(PTRACE_CONT, w, NULL, NULL);
                continue;
            }
            if (regs.rip == base + 1) {
                if (only_tid == 0 || w == only_tid) {
                    /* The pinned/first thread: restore the byte, rewind rip to base, and
                     * leave it trap-stopped THERE for dfp_step_loop (pre_positioned). */
                    dfp_remove_bp(w, base, orig);
                    regs.rip = base;
                    if (ptrace(PTRACE_SETREGS, w, NULL, &regs) != 0)
                        return -1;
                    *entering = w;
                    return 0;
                }
                /* A non-target reached base first: step it OVER the bp and release it,
                 * keeping the bp armed for only_tid. */
                if (++budget > DFP_STEP_BACKSTOP ||
                    dfp_step_over_bp(w, base, orig) != 0) {
                    dfp_remove_bp_any(set, base, orig);
                    return -1;
                }
                ptrace(PTRACE_CONT, w, NULL, NULL);
                continue;
            }
            /* A SIGTRAP not at our bp (a stray group/event stop): just resume. */
            ptrace(PTRACE_CONT, w, NULL, NULL);
            continue;
        }
        /* A group-stop (SIGSTOP family under SEIZE) resumes with 0; any other signal is
         * forwarded so the target handles its own. */
        int fwd = (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
                   sig == SIGTTOU)
                      ? 0
                      : sig;
        ptrace(PTRACE_CONT, w, NULL, (void *)(uintptr_t)fwd);
    }
}

/* Detach every thread in `set` except `keep` (the one about to be single-stepped) so the
 * siblings resume and run FREE (untraced, full speed). Each is INTERRUPT'd, waited to a
 * stop, then DETACHed; a sibling caught at base+1 (a concurrent int3 hit not yet drained)
 * is rewound to base first so it re-executes the now-restored instruction correctly. */
static void dfp_detach_others(dfp_thrset *set, pid_t keep, uint64_t base) {
    for (size_t i = 0; i < set->n; i++) {
        pid_t tid = set->v[i];
        if (tid == keep)
            continue;
        ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
        int st = 0;
        pid_t w;
        for (;;) {
            w = waitpid(tid, &st, __WALL);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                break; /* gone */
            }
            if (WIFEXITED(st) || WIFSIGNALED(st)) {
                w = -1;
                break;
            }
            if (WIFSTOPPED(st))
                break;
        }
        if (w != tid)
            continue; /* exited: nothing to detach */
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0 &&
            regs.rip == base + 1) {
            regs.rip = base;
            ptrace(PTRACE_SETREGS, tid, NULL, &regs);
        }
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
    }
}

/* The multi-thread worker-targeting core. `img`/`when` carry the optional Increment-3
 * versioned decode (NULL/0 = the live process_vm_readv snapshot) so the Increment-5 JIT
 * entry can compose worker-targeting with time-correct bytes; the public attach_pid_tid
 * below passes NULL. `survived` (optional, NULL = don't care) reports whether the crash-
 * safe detach actually engaged on a live, stopped tracee — see the note at its write site
 * below for exactly what it does and does not prove. */
static int dfp_attach_worker(pid_t pid, pid_t only_tid, uint64_t base,
                             size_t code_len, uint64_t max_insns,
                             asmtest_codeimage_t *img, uint64_t when,
                             long *result, int *survived,
                             asmtest_valtrace_t *vt) {
    if (survived != NULL)
        *survived = 0;
    if (vt == NULL || pid <= 0 || only_tid < 0 || base == 0 || code_len == 0)
        return DF_PTRACE_EINVAL;
    vt->mem_space = AT_LOC_MEM_ABS;

    dfp_thrset set;
    memset(&set, 0, sizeof set);

    /* SEIZE every thread of the (live, foreign) target. */
    if (dfp_seize_all(pid, &set) != 0) {
        free(set.v);
        return DF_PTRACE_ETRACE;
    }
    /* A requested tid must actually be a thread we seized — else it could hit the shared
     * int3 UNTRACED and take a real (target-fatal) SIGTRAP. */
    if (only_tid != 0 && !dfp_thrset_has(&set, only_tid)) {
        dfp_detach_others(&set, 0, base); /* nothing planted yet */
        free(set.v);
        return DF_PTRACE_ETRACE;
    }

    /* Plant the shared entry breakpoint, release all threads, catch whichever enters. */
    pid_t entering = 0;
    if (dfp_run_to_multi(&set, base, only_tid, &entering) != 0) {
        dfp_detach_others(&set, 0, base);
        free(set.v);
        return DF_PTRACE_ETRACE;
    }

    /* The entering thread is trap-stopped at base. Detach the SIBLINGS so they run free. */
    dfp_detach_others(&set, entering, base);

    /* Read the region bytes FROM the target (shared AS; read via the entering tid). */
    uint8_t *code = (uint8_t *)malloc(code_len);
    if (code == NULL) {
        ptrace(PTRACE_DETACH, entering, NULL, NULL);
        free(set.v);
        return DF_PTRACE_ETRACE;
    }
    if (!child_read(entering, base, code, code_len)) {
        free(code);
        ptrace(PTRACE_DETACH, entering, NULL, NULL);
        free(set.v);
        return DF_PTRACE_ETRACE;
    }

    dfp_ctx c;
    memset(&c, 0, sizeof c);
    c.pid =
        entering; /* single-step the ENTERING thread by its own tid, never the leader */
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base;
    c.foreign = 1;        /* NEVER kill the target on any exit */
    c.pre_positioned = 1; /* already trap-stopped AT base (dfp_run_to_multi) */
    c.img = img;          /* optional versioned decode (NULL = live snapshot) */
    c.when = when;

    int left_stopped = 0;
    int rc =
        dfp_step_loop(&c, base, code_len, max_insns, result, &left_stopped);
    free(c.cur.v);
    free(code);

    /* Crash-safe detach: the entering thread is trap-stopped past the region; DETACH (with
     * any fault signal forwarded, incl. a delivered app SIGTRAP from the signal split above)
     * resumes it free so the target SURVIVES. `*survived` reflects whether the kernel
     * actually CONFIRMED the detach (rc==0) rather than merely that we believed the tracee
     * was still stopped — a real proof for THIS family, unlike the self-forked
     * asmtest_dataflow_ptrace_attach sibling, which has a shared control page the victim
     * itself writes to after running past the region; a foreign attach has no such page, so
     * a successful detach-while-stopped is the strongest signal available. */
    int detached_ok = 0;
    if (left_stopped)
        detached_ok = ptrace(PTRACE_DETACH, entering, NULL,
                             (void *)(uintptr_t)c.detach_sig) == 0;
    if (survived != NULL)
        *survived = detached_ok;
    free(set.v);
    return rc;
}

/* Increment 4 — worker-thread targeting. SEIZE EVERY thread of the (live, foreign) target,
 * plant one shared region-entry int3, and single-step WHICHEVER thread first enters the
 * region — identified by its OWN tid (the waitpid-reported stopping thread), never the
 * leader — while the siblings are DETACHed to run free at full speed. `only_tid` (0 = any)
 * pins exactly one thread. This is the entry managed methods need: they run on worker
 * threads, so the leader-only attach_pid above returns nothing for them. Native-decode
 * (live snapshot); versioned + worker-targeting is composed by the Increment-5 JIT entry. */
int asmtest_dataflow_ptrace_attach_pid_tid(pid_t pid, pid_t only_tid,
                                           uint64_t base, size_t code_len,
                                           uint64_t max_insns, long *result,
                                           asmtest_valtrace_t *vt) {
    return dfp_attach_worker(pid, only_tid, base, code_len, max_insns, NULL, 0,
                             result, NULL, vt);
}

/* Increment 5 — the JIT-aware entry point: worker-thread targeting (Inc 4) + hardware/int3
 * entry breakpoint (automatic via dfp_run_to_multi) + versioned decode & method-version
 * attribution feed (Inc 3, `img`/`when` — attribution itself stays the caller-side
 * asmtest_method_attribute post-pass, exactly as Inc 3 established) + call-out step-over
 * (Inc 2, always on in dfp_step_loop) + the signal split (this increment, always on in
 * dfp_step_loop) + the crash-safe two-phase detach with an explicit `*survived` proof. This
 * is everything a live managed runtime needs that a native attach does not: real methods run
 * on worker threads, get re-JIT'd/moved mid-capture, call runtime helpers, and may trap into
 * their own signal machinery mid-region. `img`/`when` may be NULL/0 to degrade to the native
 * live-snapshot decode `attach_pid_tid` uses. Composes entirely on `dfp_attach_worker`; the
 * only code this function adds is the ENOSYS/EINVAL contract and the `*survived` default. */
int asmtest_dataflow_ptrace_attach_jit(pid_t pid, pid_t only_tid, uint64_t base,
                                       size_t code_len,
                                       asmtest_codeimage_t *img, uint64_t when,
                                       uint64_t max_insns, long *result,
                                       int *survived, asmtest_valtrace_t *vt) {
    if (survived != NULL)
        *survived = 0;
    return dfp_attach_worker(pid, only_tid, base, code_len, max_insns, img,
                             when, result, survived, vt);
}

#else /* not (Linux x86-64 + Capstone) */

int asmtest_dataflow_ptrace_run(const uint8_t *code, size_t code_len,
                                const long *args, int nargs, uint64_t max_insns,
                                uint64_t gs_base, long *result,
                                asmtest_valtrace_t *vt) {
    (void)code;
    (void)code_len;
    (void)args;
    (void)nargs;
    (void)max_insns;
    (void)gs_base;
    (void)result;
    (void)vt;
    return DF_PTRACE_ENOSYS;
}

int asmtest_dataflow_ptrace_attach(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   uint64_t max_insns, uint64_t gs_base,
                                   long *result, int *survived,
                                   asmtest_valtrace_t *vt) {
    (void)code;
    (void)code_len;
    (void)args;
    (void)nargs;
    (void)max_insns;
    (void)gs_base;
    (void)result;
    (void)vt;
    if (survived != NULL)
        *survived = 0;
    return DF_PTRACE_ENOSYS;
}

int asmtest_dataflow_ptrace_attach_pid(pid_t pid, uint64_t base,
                                       size_t code_len, uint64_t max_insns,
                                       long *result, asmtest_valtrace_t *vt) {
    (void)pid;
    (void)base;
    (void)code_len;
    (void)max_insns;
    (void)result;
    (void)vt;
    return DF_PTRACE_ENOSYS;
}

int asmtest_dataflow_ptrace_attach_pid_versioned(pid_t pid, uint64_t base,
                                                 size_t code_len,
                                                 uint64_t max_insns,
                                                 asmtest_codeimage_t *img,
                                                 uint64_t when, long *result,
                                                 asmtest_valtrace_t *vt) {
    (void)pid;
    (void)base;
    (void)code_len;
    (void)max_insns;
    (void)img;
    (void)when;
    (void)result;
    (void)vt;
    return DF_PTRACE_ENOSYS;
}

int asmtest_dataflow_ptrace_attach_pid_tid(pid_t pid, pid_t only_tid,
                                           uint64_t base, size_t code_len,
                                           uint64_t max_insns, long *result,
                                           asmtest_valtrace_t *vt) {
    (void)pid;
    (void)only_tid;
    (void)base;
    (void)code_len;
    (void)max_insns;
    (void)result;
    (void)vt;
    return DF_PTRACE_ENOSYS;
}

int asmtest_dataflow_ptrace_attach_jit(pid_t pid, pid_t only_tid, uint64_t base,
                                       size_t code_len,
                                       asmtest_codeimage_t *img, uint64_t when,
                                       uint64_t max_insns, long *result,
                                       int *survived, asmtest_valtrace_t *vt) {
    (void)pid;
    (void)only_tid;
    (void)base;
    (void)code_len;
    (void)img;
    (void)when;
    (void)max_insns;
    (void)result;
    (void)vt;
    if (survived != NULL)
        *survived = 0;
    return DF_PTRACE_ENOSYS;
}

#endif
