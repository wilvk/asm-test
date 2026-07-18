/*
 * ss_btf.c — in-process, no-fork, branch-granular single-step via the x86 debug-control
 * BTF bit (DEBUGCTL.BTF, MSR 0x1d9 bit 1). See docs/internal/implementations/
 * inproc-btf-block-step.md (W3) for the full design + Research notes this file cites.
 *
 * asm-test already has branch-granular stepping OUT of process (PTRACE_SINGLEBLOCK, in
 * src/ptrace_backend.c) and per-INSTRUCTION stepping in-process (EFLAGS.TF, ss_backend.c).
 * This is the missing third form: in-process AND branch-granular, by arming BOTH
 * EFLAGS.TF and DEBUGCTL.BTF through the same thread-pinned /dev/cpu/N/msr route
 * src/msr_lbr.c already uses, then feeding the resulting waypoints into the AMD replay
 * loop (asmtest_amd_decode_stitched) — no 16-entry ceiling, no ptrace child.
 *
 * ENVELOPE. Linux does NOT preserve a user-written BTF across a context switch:
 * __switch_to_xtra() only touches DEBUGCTL for TIF_BLOCKSTEP tasks (arch/x86/kernel/
 * process.c), and the only in-tree TIF_BLOCKSTEP writer is ptrace's PTRACE_SINGLEBLOCK
 * (arch/x86/kernel/step.c) — so a raw-MSR BTF capture that outlives a context switch is
 * silently unprotected. BTF is also a hardware ONE-SHOT: the CPU clears it the moment it
 * raises the branch-trap #DB (Intel SDM Vol 3B), so every trap must re-arm it before
 * resuming. This tier is therefore deliberately scoped to a PINNED, SMALL, LEAF-ROUTINE
 * envelope with per-trap re-arm and honest truncation on any observed context switch —
 * never a general, context-switch-proof replacement for the shipped ptrace block-step
 * (which owns the kernel-coupled TIF_BLOCKSTEP form and stays the robust default).
 *
 * A kernel helper for a context-switch-proof in-process form was deliberately NOT built:
 * that robust general form already exists as PTRACE_SINGLEBLOCK, and an out-of-tree
 * module could not be validated in any Docker lane anyway (loading a module is a
 * host-kernel change, the same gate class as hardware).
 */
#define _GNU_SOURCE

#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"

#include <stddef.h>

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define SS_BTF_MSR_DEBUGCTL 0x1d9u
#define SS_BTF_DEBUGCTL_BTF (1ull << 1)
#define SS_BTF_TF                                                              \
    0x100ull /* EFLAGS.TF, bit 8 — same constant as ss_backend.c */

/* Deliberate 20-line duplication of msr_lbr.c's static MSR helpers (that file's own
 * comment explains why: two independent single-TU privileged-MSR backends, no shared
 * internal header worth the coupling for three one-line wrappers). */
static int msr_rd(int fd, uint32_t reg, uint64_t *out) {
    return pread(fd, out, 8, (off_t)reg) == 8 ? 0 : -1;
}
static int msr_wr(int fd, uint32_t reg, uint64_t v) {
    return pwrite(fd, &v, 8, (off_t)reg) == 8 ? 0 : -1;
}
static int open_msr(int cpu) {
    char path[64];
    snprintf(path, sizeof path, "/dev/cpu/%d/msr", cpu);
    return open(path, O_RDWR);
}

/* ------------------------------------------------------------------------- *
 * asmtest_ss_btf_available(): hang-proof functional probe.
 *
 * Structurally hang-proof: no fork, no waitpid — this thread arms itself, runs a
 * bounded 9-byte blob, and disarms. Worst case (BTF silently degrades to
 * per-instruction TF stepping) is 9 traps, not a hang.
 * ------------------------------------------------------------------------- */

/* Handler state for the probe only (distinct statics from the T3 capture handler —
 * the probe and a real capture are never concurrent, but keeping them separate keeps
 * each handler's async-signal-safe footprint minimal and obviously correct). */
static volatile sig_atomic_t g_probe_total_stops;
static volatile sig_atomic_t g_probe_nop_stops;
static volatile sig_atomic_t g_probe_rearm_failed;
static uint64_t g_probe_nop_lo,
    g_probe_nop_hi; /* [lo,hi): the 8-byte nop run */
static int g_probe_msr_fd;
static uint64_t
    g_probe_rearm_value; /* saved DEBUGCTL | BTF, re-applied every trap */

/* Async-signal-safe only: increment counters, re-arm BTF (the #DB just cleared it),
 * re-assert TF (POPF/IRET's one-instruction delay does not apply mid-handler — the
 * handler's own body never traps because TF reads 0 inside the signal frame). */
static void probe_on_sigtrap(int sig, siginfo_t *si, void *uctx) {
    (void)sig;
    (void)si;
    g_probe_total_stops++;
    ucontext_t *uc = (ucontext_t *)uctx;
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    /* Strictly-greater-than lo: the CALL into the blob is itself a taken branch, so a
     * functional BTF trap legitimately lands AT the entry byte (rip == g_probe_nop_lo)
     * — empirically confirmed on Zen 2 (both -O0 and -O2): a working probe sees exactly
     * two stops, one at the entry and one back in the caller (the ret), zero in between.
     * Only a stop STRICTLY INSIDE the run (entry+1 .. hi-1) is the degradation signature
     * — BTF silently dropped to per-instruction TF stepping, tripping after every nop. */
    if (rip > g_probe_nop_lo && rip < g_probe_nop_hi)
        g_probe_nop_stops++;
    if (pwrite(g_probe_msr_fd, &g_probe_rearm_value, 8,
               (off_t)SS_BTF_MSR_DEBUGCTL) != 8)
        g_probe_rearm_failed =
            1; /* some hypervisors reject the write outright */
    uc->uc_mcontext.gregs[REG_EFL] |= (greg_t)SS_BTF_TF;
}

/* always_inline, not just the "inline" hint: at -O0 (this project's default CFLAGS) a
 * plain "inline" function is routinely emitted out-of-line, which inserts the helper's
 * own CALL/RET around the arm point — extra taken branches that the functional probe
 * below would misread as capture activity. Must be truly inline so the only branch
 * between arming and the blob call is the blob call itself. */
__attribute__((always_inline)) static inline void probe_arm_tf(void) {
    __asm__ __volatile__("pushfq\n\torq $0x100,(%%rsp)\n\tpopfq"
                         :
                         :
                         : "cc", "memory");
}
__attribute__((always_inline)) static inline void probe_disarm_tf(void) {
    __asm__ __volatile__("pushfq\n\tandq $-257,(%%rsp)\n\tpopfq"
                         :
                         :
                         : "cc", "memory");
}

int asmtest_ss_btf_available(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    cached = 0;

    if (!asmtest_disas_available())
        return cached; /* Capstone drives the reconstruction — same gate as ptrace's */

    int cpu = sched_getcpu();
    if (cpu < 0)
        cpu = 0;
    cpu_set_t set, prev;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    CPU_ZERO(&prev);
    int had_prev = sched_getaffinity(0, sizeof prev, &prev) == 0;
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        return cached;

    int fd = open_msr(cpu);
    if (fd < 0) {
        /* needs CAP_SYS_ADMIN + the msr module, i.e. the docker-hwtrace-msr lane */
        if (had_prev)
            sched_setaffinity(0, sizeof prev, &prev);
        return cached;
    }

    static const uint8_t blob[] = {0x90, 0x90, 0x90, 0x90, 0x90,
                                   0x90, 0x90, 0x90, 0xC3}; /* 8x nop; ret */
    void *p = mmap(NULL, sizeof blob, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        close(fd);
        if (had_prev)
            sched_setaffinity(0, sizeof prev, &prev);
        return cached;
    }
    memcpy(p, blob, sizeof blob);

    uint64_t saved_debugctl = 0;
    if (msr_rd(fd, SS_BTF_MSR_DEBUGCTL, &saved_debugctl) != 0) {
        munmap(p, sizeof blob);
        close(fd);
        if (had_prev)
            sched_setaffinity(0, sizeof prev, &prev);
        return cached;
    }

    g_probe_total_stops = 0;
    g_probe_nop_stops = 0;
    g_probe_rearm_failed = 0;
    g_probe_nop_lo = (uint64_t)(uintptr_t)p;
    g_probe_nop_hi = g_probe_nop_lo + 8; /* the 8 nop bytes; byte 8 is ret */
    g_probe_msr_fd = fd;
    g_probe_rearm_value = saved_debugctl | SS_BTF_DEBUGCTL_BTF;

    struct sigaction sa, old_sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = probe_on_sigtrap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    int functional = 0;
    if (sigaction(SIGTRAP, &sa, &old_sa) == 0) {
        if (msr_wr(fd, SS_BTF_MSR_DEBUGCTL, g_probe_rearm_value) == 0) {
            probe_arm_tf();
            ((void (*)(void))p)();
            probe_disarm_tf();
            functional = !g_probe_rearm_failed && g_probe_total_stops >= 1 &&
                         g_probe_nop_stops == 0;
        }
        /* Clear BTF and restore the pre-probe DEBUGCTL unconditionally. */
        msr_wr(fd, SS_BTF_MSR_DEBUGCTL, saved_debugctl);
        sigaction(SIGTRAP, &old_sa, NULL);
    }

    munmap(p, sizeof blob);
    close(fd);
    if (had_prev)
        sched_setaffinity(0, sizeof prev, &prev);

    cached = functional;
    return cached;
}

#else /* not Linux x86-64 */

int asmtest_ss_btf_available(void) { return 0; }

#endif
