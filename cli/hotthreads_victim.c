/* hotthreads_victim.c — several threads hammering ONE entry, for the perf-free
 * picker's concurrent-arrival path (cli/test_ptracesample.c).
 *
 * WHY THIS VICTIM EXISTS. Phase 3 arms an int3 at a candidate entry and then, on
 * each arrival, restores the byte, single-steps the arriving thread over it, and
 * re-plants. Every other victim that gets far enough to be ARMED is
 * single-threaded — auto_victim, sigload_victim and forkhot_victim all are, and
 * tid_victim is only ever used with a capped seize, where phase 3 is skipped by
 * design. So the entire CONCURRENT half of phase 3 had no coverage at all:
 *
 *  - a second thread arriving at the entry while the first is mid-step, whose
 *    stop is consumed by a pump that is NOT collecting arrivals. It must be
 *    rewound and RELEASED. Handing it its SIGTRAP back instead — which is what
 *    an int3's si_code == SI_KERNEL invites, since it looks exactly like the
 *    target's own breakpoint — kills a process with no SIGTRAP handler.
 *  - the race between removing the byte and a trap already in flight, which is
 *    why "the address we armed" and "the byte is in the text" are two facts.
 *
 * THE SHAPE IS THE TEST: PS_THREADS threads, all calling the SAME
 * `shared_entry`, at ~100% duty. That maximises the chance of a second arrival
 * landing inside the step-and-rearm window, which is the whole point — with one
 * thread that window is empty by construction.
 *
 * It prints `beat=<n>` at ~20 Hz, line-buffered, from the LEADER, so a reader
 * can measure forward progress during and after the attach the way
 * sigload_victim does: a victim killed by a mishandled trap stops printing, and
 * a frozen one stops printing too.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here.
 */
#define _GNU_SOURCE
#include <pthread.h>
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

#define PS_THREADS 3

static volatile long g_sink;

/* THE entry. Small, noinline, and called from every thread's inner loop, so it
 * is both what the picker nominates and what several threads arrive at at once. */
__attribute__((noinline)) long shared_entry(long x) {
    return x * 2654435761u + 1;
}

static void *worker(void *a) {
    long k = (long)(intptr_t)a;
    for (long i = 0;; i++) {
        for (int j = 0; j < 256; j++)
            g_sink += shared_entry(i + j + k);
    }
    return NULL;
}

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);

    pthread_t t[PS_THREADS];
    for (long i = 0; i < PS_THREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i);

    fprintf(stderr, "hotthreads_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    /* The leader arrives at the same entry as the workers AND reports progress,
     * so "is it still running" and "is it still arriving" are one measurement. */
    long beats = 0, next = mono_ms() + 50;
    for (;;) {
        for (int j = 0; j < 256; j++)
            g_sink += shared_entry(j);
        long now = mono_ms();
        if (now >= next) {
            printf("beat=%ld\n", beats++);
            next = now + 50;
        }
    }
    return 0;
}
