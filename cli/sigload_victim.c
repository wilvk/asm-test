/* sigload_victim.c — a victim that is BUSY *and* SIGNAL-DRIVEN, for the
 * perf-free region picker's signal-fidelity check (cli/test_ptracesample.c).
 *
 * WHY THIS VICTIM EXISTS, and why auto_victim cannot do its job.
 *
 * The ptrace residency sampler stops the target ~997 times a second with
 * PTRACE_INTERRUPT and resumes it. The tempting resume is the one the prototype
 * shipped: `PTRACE_CONT(tid, 0)` unconditionally. man 2 ptrace is blunt about
 * what that costs — "If sig is 0, then a signal is not delivered" — and every
 * stop the tracer consumes that was really a SIGNAL-DELIVERY-stop has its
 * signal thrown away by that 0.
 *
 * MEASURED against this victim's shape (100 Hz ITIMER_REAL, 2026-08-06):
 * the unconditional-CONT prototype destroyed 89% of the target's SIGALRMs and
 * collapsed its forward progress ~99% — 2 utime ticks and 0 progress lines per
 * 2 s, against 200 and 93 both at baseline and after detach. Against a
 * SIGNAL-FREE spinner (auto_victim) the identical defect costs about 1% and is
 * completely invisible, which is exactly how it shipped. So a lane that only
 * ever samples a spinner is blind to the single most destructive thing this
 * sampler can do to a process it does not own.
 *
 * THE SHAPE IS THE TEST, in three parts, all three load-bearing:
 *
 *  1. It SPINS in user space between ticks. The sampler only interrupts threads
 *     that /proc/<tid>/syscall reports as `running`, so a victim that sleeps or
 *     blocks between signals is never sampled and the check becomes vacuous.
 *  2. Its ITIMER_REAL fires at 100 Hz, ~10x faster than the printing rate, so a
 *     swallowed signal shows up as a missing COUNT rather than as jitter.
 *  3. It prints `ticks=<n>` at ~20 Hz, LINE-BUFFERED, so a reader can compute a
 *     RATE over a sub-second window while the sampler is still attached. That
 *     matters: the prototype's damage was transient — the victim recovered its
 *     full rate the moment the tracer detached — so a before/after comparison
 *     around the call cannot see it. Only a measurement taken DURING the
 *     attach can, and 20 Hz is what makes a ~1 s during-window meaningful.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here: Yama
 * ptrace_scope=1 is the Ubuntu default and is NOT namespaced, so without it the
 * attach is denied and the failure reads like a tracer bug.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* sig_atomic_t + volatile: the only thing a handler may safely touch here. The
 * PRINTING happens in the main loop, never in the handler. */
static volatile sig_atomic_t g_ticks;
static volatile long g_sink;

static void on_alarm(int sig) {
    (void)sig;
    g_ticks++;
}

/* CLOCK_MONOTONIC via the vDSO, so the pacing read is NOT a syscall — a victim
 * that syscalled its way around this loop would spend its time reported by
 * /proc/<tid>/syscall as a blocked syscall number rather than `running`, and the
 * sampler would skip it (see part 1 of the shape note above). */
static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    /* Line-buffered: the reader parses whole `ticks=<n>` lines as they appear.
     * Full buffering would batch a second of them into one 4 KiB flush and
     * destroy the sub-second rate the check is built on. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    struct sigaction sa;
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* don't turn a swallowed signal into an EINTR
                               * story: the only thing under test is delivery */
    sigaction(SIGALRM, &sa, NULL);

    struct itimerval it;
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 10000; /* 100 Hz */
    it.it_value = it.it_interval;
    setitimer(ITIMER_REAL, &it, NULL);

    fprintf(stderr, "sigload_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    long next = mono_ms() + 50;
    for (;;) {
        /* Real user-space work, so the thread is genuinely RUNNING (not merely
         * runnable) when the sampler looks at it. Sized so the pacing check
         * below still lands within a millisecond or two of its deadline. */
        long s = 0;
        for (int i = 0; i < 20000; i++)
            s += (long)i * 3 + (s & 7);
        g_sink += s;

        long now = mono_ms();
        if (now >= next) {
            /* One line per ~50 ms. `(int)` because sig_atomic_t is int-shaped
             * and the reader parses a plain decimal. */
            printf("ticks=%d\n", (int)g_ticks);
            next = now + 50;
        }
    }
    return 0;
}
