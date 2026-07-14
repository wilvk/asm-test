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
 * Scope + supported target (Phase 3, first landing): a deterministic, single-threaded
 * LEAF routine of up to six integer arguments, executed from an inherited executable
 * mapping; the recorded region is [code, code+len) and value capture is bounded to it.
 * Call-outs to helpers OUTSIDE the region are not yet stepped over here (the offset-only
 * backend's step-over seam is not exported); a routine that leaves the region is treated
 * as having returned. Whole-run capture is a non-goal by design (see the plan).
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
#include <elf.h> /* NT_X86_XSTATE */
#include <errno.h>
#include <signal.h>
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
    /* The enumerator returns the instruction byte length — needed for the RIP-relative
     * EA fixup (the EA is relative to the next instruction). */
    size_t insn_len = asmtest_operands(ASMTEST_ARCH_X86_64, c->code,
                                       c->code_len, off, rd, &nr, wr, &nw);

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
            /* Left the region (a leaf routine's ret): finalize the last in-region step
             * from this post-state, record the return value, and hand the still-stopped
             * tracee back to the caller for its resume/detach policy. */
            if (c->have_cur)
                finalize_step(c, &regs);
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

int asmtest_dataflow_ptrace_attach_pid(pid_t pid, uint64_t base,
                                       size_t code_len, uint64_t max_insns,
                                       long *result, asmtest_valtrace_t *vt) {
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

#endif
