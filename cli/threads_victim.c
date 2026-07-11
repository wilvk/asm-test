/* threads_victim.c — a MULTI-threaded victim for asmspy's thread-follow smoke.
 *
 * Spawns several worker threads that each loop on a cheap, frequent syscall,
 * alongside the main thread, so an out-of-process tracer that follows every
 * thread (PTRACE_SEIZE + PTRACE_O_TRACECLONE) sees syscalls from more than one
 * tid. A single-thread attach would only ever see the main thread. Opts in via
 * PR_SET_PTRACER_ANY like the other example victims so attach works in a plain
 * container.
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

static volatile long g_sink;

static void *worker(void *arg) {
    (void)arg;
    /* A short nap keeps this a good CPU citizen, but the compute loop dominates
     * so the thread is RUNNABLE most of the time — which is what lets the
     * single-stepping instruction stream interleave all threads instead of one
     * runnable thread monopolizing a short capture window. */
    struct timespec nap = {0, 100 * 1000}; /* 0.1 ms */
    for (;;) {
        long s = 0;
        for (int i = 0; i < 4000; i++) /* user-space work to single-step */
            s += (long)i * i;
        g_sink += s;
        getpid(); /* a cheap, unmistakable syscall attributable to this tid */
        nanosleep(&nap, NULL);
    }
    return NULL;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "threads_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    pthread_t t[3];
    for (int i = 0; i < 3; i++)
        pthread_create(&t[i], NULL, worker, NULL);

    struct timespec nap = {0, 5 * 1000 * 1000}; /* main thread syscalls too */
    for (;;)
        nanosleep(&nap, NULL);
    return 0;
}
