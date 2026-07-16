/* clone_victim.c — spawns threads DURING the trace, not before it.
 *
 * Every other multi-threaded victim here starts its workers up front, so they
 * are all present at attach and get seized by seize_threads. This one keeps
 * creating short-lived threads while asmspy is already attached, which is the
 * only way to reach two paths:
 *
 *  1. POST-ATTACH CLONE FOLLOWING (asmspy-plan Theme D) — a thread that did not
 *     exist at attach must still be followed, via PTRACE_O_TRACECLONE and the
 *     clone-event handler, not via the /proc scan seize_threads does once.
 *     spawned_fn runs ONLY on those threads, so naming it proves the path.
 *
 *  2. The thr_get OOM path (asmspy-plan Theme C) — an untabled task can only
 *     appear where a NEW task shows up mid-trace. Under ASMSPY_TEST_THR_OOM the
 *     engine refuses to table these, and the target must SURVIVE that.
 *
 * main_fn runs on the leader throughout, so the smoke can tell "the leader is
 * being traced" from "the spawned threads are being followed".
 *
 * Opts in via PR_SET_PTRACER_ANY like the other victims.
 */
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

static volatile long sink;

/* runs ONLY on threads created after the attach */
__attribute__((noinline)) long spawned_fn(long x) { return x * 19 + 7; }

/* runs ONLY on the leader */
__attribute__((noinline)) long main_fn(long x) { return x * 23 + 9; }

static void *worker(void *arg) {
    long k = (long)(intptr_t)arg;
    for (int i = 0; i < 40; i++)
        sink += spawned_fn(k + i);
    usleep(30 * 1000);
    return NULL;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "clone_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    /* Let the tracer attach while we are still SINGLE-threaded: every worker
     * below is then unambiguously a post-attach clone. */
    usleep(1500 * 1000);

    for (long i = 0;; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, (void *)(intptr_t)i) == 0)
            pthread_detach(t); /* detached: no join, threads come and go */
        for (int j = 0; j < 20; j++)
            sink += main_fn(i + j);
        usleep(60 * 1000);
    }
    return 0;
}
