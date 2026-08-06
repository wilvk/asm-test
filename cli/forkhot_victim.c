/* forkhot_victim.c — a victim that FORKS from inside its hot loop, for the
 * perf-free region picker's copy-on-write trap check (cli/test_ptracesample.c).
 *
 * WHY THIS VICTIM EXISTS. Phase 3 of the picker plants an int3 at a candidate
 * entry and leaves it armed for up to 50 ms per candidate. A fork(2) inside that
 * window hands the child a copy-on-write copy of the target's text — INCLUDING
 * our trap byte. The child is a different process with no tracer, so the first
 * time it executes that entry it takes a SIGTRAP whose default action kills it.
 * A fork-per-request server is an entirely ordinary target.
 *
 * Nothing else in this tree could catch that. clone_victim exercises the THREAD
 * path (same address space, covered by PTRACE_O_TRACECLONE); fork_victim forks
 * exactly once, two seconds in, and then sleeps — so it is never hot enough to
 * be nominated and never forks inside an armed window.
 *
 * THE SHAPE IS THE TEST:
 *
 *  1. `hot_entry` is called constantly from the parent's inner loop, so it is
 *     what phase 3 arms. Without that, no window is armed and the check is
 *     vacuous.
 *  2. The CHILD calls both entries after the fork. That is the whole point: an
 *     inherited trap byte is only fatal if the child reaches it — and `hot_entry`
 *     alone is not enough, because its armed window closes in microseconds.
 *  3. The child prints `kid=<n>` and _exit(0)s. A child killed by SIGTRAP prints
 *     NOTHING, so the parent's line rate is a direct, countable measure of
 *     whether children are surviving — the same trick sigload_victim uses for
 *     signal delivery, for the same reason: the failure is silent otherwise.
 *  4. It forks often (~200 Hz) so a sub-second armed window contains several.
 *
 * SIGCHLD is set to SIG_IGN so children are reaped by the kernel and this never
 * accumulates zombies — the tracer's waitpid(-1) must not find a pile of them.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

static volatile long g_sink;

/* The entry phase 3 will arm, and the one the child re-enters after the fork. */
__attribute__((noinline)) long hot_entry(long x) { return x * 2654435761u + 1; }

/* The entry that makes this check real. The parent enters it about every 20 ms,
 * so four arrivals cannot be collected inside phase 3's per-candidate budget and
 * the window stays ARMED for the whole of it — around ten forks' worth. And the
 * CHILD enters it immediately after the fork, so a copy-on-write copy of the
 * trap is not merely inherited, it is EXECUTED.
 *
 * Without this the check was hollow: MEASURED, disabling the fork-child release
 * entirely was caught 0 times in 4, because every entry armed while a fork was
 * in flight was a fork-path one (_Fork, __libc_fork, fork@plt) that the child
 * never calls again, and hot_entry's own window closes in microseconds. */
__attribute__((noinline)) long slow_entry(long x) { return x ^ 0x5bf03635; }

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Kernel-reaped children: the tracer runs waitpid(-1) and must not have to
     * pick through this victim's zombies. */
    signal(SIGCHLD, SIG_IGN);

    fprintf(stderr, "forkhot_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    long kids = 0, beats = 0;
    long next = mono_ms() + 5;
    for (long i = 0;; i++) {
        /* Hot enough that hot_entry is what the residency phase nominates and
         * what its callers name. */
        for (int k = 0; k < 500; k++)
            g_sink += hot_entry(i + k);

        long now = mono_ms();
        if (now < next)
            continue;
        next = now + 5;       /* ~200 forks/s */
        if (++beats % 4 == 0) /* ~50 Hz: slow enough to keep a window open */
            g_sink += slow_entry(beats);
        pid_t p = fork();
        if (p == 0) {
            /* THE CHILD. It runs both entries the parent's trap may be planted
             * at; an inherited int3 kills it right here, before it can print. */
            for (int k = 0; k < 200; k++)
                g_sink += hot_entry(k);
            g_sink += slow_entry(beats);
            printf("kid=%ld\n", kids);
            _exit(0);
        }
        if (p > 0)
            kids++;
    }
    return 0;
}
