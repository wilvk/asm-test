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

#include "amd_backend.h" /* asmtest_amd_decode_stitched + ASMTEST_HW_* */
#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"
#include "bs_recon.h" /* asmtest_bs_scan_terminator, asmtest_ss_btf_pair_stops */

#include <stddef.h>

#if defined(__linux__) && defined(__x86_64__)
#include <fcntl.h>
#include <linux/perf_event.h> /* struct perf_branch_entry */
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h> /* getrusage/RUSAGE_THREAD: the context-switch honesty belt */
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

/* ------------------------------------------------------------------------- *
 * asmtest_ss_btf_trace(): the real capture + (from,to) waypoint synthesis.
 * ------------------------------------------------------------------------- */

#ifndef SS_BTF_STREAM_CAP
#define SS_BTF_STREAM_CAP                                                      \
    (1u << 16) /* same envelope as ss_backend.c's SS_STREAM_CAP */
#endif

/* Capture-handler state, distinct from the probe's above (never concurrent with it —
 * asmtest_ss_btf_trace and asmtest_ss_btf_available never run on the same thread at the
 * same time — but kept separate anyway so each handler's async-signal-safe footprint
 * stays obviously self-contained). g_cap_armed is the single non-nested-capture guard:
 * DEBUGCTL is physical per-CPU state, so nesting would mean two callers fighting over
 * the same MSR — meaningless, not just unsupported. */
static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_cap_armed;
static uint64_t *g_cap_stream;
static volatile uint32_t g_cap_stream_len;
static volatile sig_atomic_t g_cap_overflow;
static volatile sig_atomic_t g_cap_rearm_failed;
static int g_cap_msr_fd;
static uint64_t g_cap_rearm_value;

/* Async-signal-safe only, same shape as probe_on_sigtrap: bounded-append the absolute
 * RIP, re-arm BTF (the #DB just cleared it), re-assert TF. Unconditionally re-arms even
 * past overflow (matching ss_backend.c's ss_on_sigtrap, which always re-asserts TF too)
 * — simpler than branching the arm state, and the capture is already truncated once
 * overflow is set, so continuing to step the (untracked) remainder costs nothing. */
static void cap_on_sigtrap(int sig, siginfo_t *si, void *uctx) {
    (void)sig;
    (void)si;
    ucontext_t *uc = (ucontext_t *)uctx;
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    if (g_cap_stream_len < SS_BTF_STREAM_CAP)
        g_cap_stream[g_cap_stream_len++] = rip;
    else
        g_cap_overflow = 1;
    if (pwrite(g_cap_msr_fd, &g_cap_rearm_value, 8,
               (off_t)SS_BTF_MSR_DEBUGCTL) != 8)
        g_cap_rearm_failed = 1; /* some hypervisors reject the write outright */
    uc->uc_mcontext.gregs[REG_EFL] |= (greg_t)SS_BTF_TF;
}

__attribute__((always_inline)) static inline void cap_arm_tf(void) {
    __asm__ __volatile__("pushfq\n\torq $0x100,(%%rsp)\n\tpopfq"
                         :
                         :
                         : "cc", "memory");
}
__attribute__((always_inline)) static inline void cap_disarm_tf(void) {
    __asm__ __volatile__("pushfq\n\tandq $-257,(%%rsp)\n\tpopfq"
                         :
                         :
                         : "cc", "memory");
}

size_t asmtest_ss_btf_pair_stops(const uint8_t *code, size_t len,
                                 uint64_t base_ip, const uint64_t *stops,
                                 size_t nstops, struct perf_branch_entry *out,
                                 size_t out_cap, int *gap) {
    if (gap != NULL)
        *gap = 0;
    if (code == NULL || len == 0 || stops == NULL || out == NULL ||
        out_cap == 0)
        return 0;

    uint64_t end_ip = base_ip + len;
    size_t i = 0;
    while (i < nstops && !(stops[i] >= base_ip && stops[i] < end_ip))
        i++; /* skip leading glue: arming/call-into-region branches outside [base,len) */
    if (i >= nstops)
        return 0; /* no in-region stop at all: nothing provably ran */

    size_t n = 0;
    for (; i + 1 < nstops && n < out_cap; i++) {
        uint64_t prev = stops[i];
        uint64_t s = stops[i + 1];
        uint64_t term = 0;
        int r = asmtest_bs_scan_terminator(ASMTEST_ARCH_X86_64, code, len,
                                           base_ip, prev - base_ip, s, &term);
        if (r != ASMTEST_BS_OK) {
            /* AMBIGUOUS or FAIL: a lost trap leaves the next stop unreachable — the
             * context-switch signature. Record the honest prefix, stop pairing. */
            if (gap != NULL)
                *gap = 1;
            break;
        }
        memset(&out[n], 0, sizeof out[n]);
        out[n].from = base_ip + term;
        out[n].to = s;
        n++;
        if (!(s >= base_ip && s < end_ip))
            break; /* the first out-of-region stop is the exit edge: done */
    }

    /* Reverse to newest-first — the order asmtest_amd_stitch's own reversal produces
     * and amd_replay consumes (src/amd_backend.c). */
    if (n > 0) {
        size_t a = 0, b = n - 1;
        while (a < b) {
            struct perf_branch_entry t = out[a];
            out[a] = out[b];
            out[b] = t;
            a++;
            b--;
        }
    }
    return n;
}

int asmtest_ss_btf_trace(const void *base, size_t len, void (*run_fn)(void *),
                         void *arg, asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || run_fn == NULL || trace == NULL)
        return ASMTEST_HW_EINVAL;
    if (!asmtest_ss_btf_available())
        return ASMTEST_HW_EUNAVAIL;

    pthread_mutex_lock(&g_cap_lock);
    if (g_cap_armed) {
        pthread_mutex_unlock(&g_cap_lock);
        return ASMTEST_HW_EFULL; /* one non-nested capture per process */
    }
    g_cap_armed = 1;
    pthread_mutex_unlock(&g_cap_lock);

    int rc = ASMTEST_HW_EUNAVAIL;
    int cpu = sched_getcpu();
    if (cpu < 0)
        cpu = 0;
    cpu_set_t set, prev;
    CPU_ZERO(&set);
    CPU_SET((unsigned)cpu, &set);
    CPU_ZERO(&prev);
    int had_prev = sched_getaffinity(0, sizeof prev, &prev) == 0;
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        goto unarm;

    {
        int fd = open_msr(cpu);
        if (fd < 0)
            goto restore_affinity;

        uint64_t *stream =
            (uint64_t *)malloc(SS_BTF_STREAM_CAP * sizeof(uint64_t));
        if (stream == NULL) {
            close(fd);
            goto restore_affinity;
        }

        uint64_t saved_debugctl = 0;
        if (msr_rd(fd, SS_BTF_MSR_DEBUGCTL, &saved_debugctl) != 0) {
            free(stream);
            close(fd);
            goto restore_affinity;
        }

        g_cap_overflow = 0;
        g_cap_rearm_failed = 0;
        g_cap_stream = stream;
        g_cap_stream_len = 0;
        g_cap_msr_fd = fd;
        g_cap_rearm_value = saved_debugctl | SS_BTF_DEBUGCTL_BTF;

        struct rusage ru_before, ru_after;
        memset(&ru_before, 0, sizeof ru_before);
        memset(&ru_after, 0, sizeof ru_after);
        getrusage(RUSAGE_THREAD, &ru_before);

        struct sigaction sa, old_sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = cap_on_sigtrap;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        int captured = 0;
        if (sigaction(SIGTRAP, &sa, &old_sa) == 0) {
            if (msr_wr(fd, SS_BTF_MSR_DEBUGCTL, g_cap_rearm_value) == 0) {
                cap_arm_tf();
                run_fn(arg);
                cap_disarm_tf();
                captured = 1;
            }
            /* Clear BTF and restore the pre-capture DEBUGCTL unconditionally. */
            msr_wr(fd, SS_BTF_MSR_DEBUGCTL, saved_debugctl);
            sigaction(SIGTRAP, &old_sa, NULL);
        }
        getrusage(RUSAGE_THREAD, &ru_after);

        if (captured) {
            /* Belt 3: ANY context switch during the armed window makes completeness
             * unprovable — a switched-out thread without TIF_BLOCKSTEP gets no BTF
             * restore (Research notes), so the kernel may have silently dropped it. */
            int switched = (ru_after.ru_nvcsw + ru_after.ru_nivcsw) >
                           (ru_before.ru_nvcsw + ru_before.ru_nivcsw);
            int gap = 0;
            size_t n = 0;
            uint32_t nstops = g_cap_stream_len;
            struct perf_branch_entry *pairs = NULL;
            if (nstops > 0)
                pairs = (struct perf_branch_entry *)malloc(
                    nstops * sizeof(struct perf_branch_entry));
            if (pairs != NULL)
                n = asmtest_ss_btf_pair_stops((const uint8_t *)base, len,
                                              (uint64_t)(uintptr_t)base, stream,
                                              nstops, pairs, nstops, &gap);
            if (n == 0) {
                /* Belt 4: zero provable waypoints is honest, never empty-complete
                 * (the msr_lbr.c shape) — asmtest_amd_decode_stitched itself refuses
                 * nbr==0 with EDECODE, which would misreport this as a hard failure. */
                trace->truncated = true;
                rc = ASMTEST_HW_OK;
            } else {
                rc = asmtest_amd_decode_stitched(pairs, n, base, len, trace,
                                                 gap);
            }
            if (pairs != NULL)
                free(pairs);
            /* Belts 1 + 3: buffer overflow, a failed re-arm write, or an observed
             * context switch all make completeness unprovable. */
            if (g_cap_overflow || g_cap_rearm_failed || switched)
                trace->truncated = true;
        }

        free(stream);
        close(fd);
    }

restore_affinity:
    if (had_prev)
        sched_setaffinity(0, sizeof prev, &prev);
unarm:
    pthread_mutex_lock(&g_cap_lock);
    g_cap_armed = 0;
    pthread_mutex_unlock(&g_cap_lock);
    return rc;
}

#else /* not Linux x86-64 */

int asmtest_ss_btf_available(void) { return 0; }

int asmtest_ss_btf_trace(const void *base, size_t len, void (*run_fn)(void *),
                         void *arg, asmtest_trace_t *trace) {
    (void)base;
    (void)len;
    (void)run_fn;
    (void)arg;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

#endif
