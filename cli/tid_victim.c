/* tid_victim.c — two threads running DISTINCT hot functions (alpha_work /
 * beta_work), for asmspy's per-thread (--tid) filter smoke and its --tree [tid]
 * tagging smoke. Each worker prints its own tid, so the smoke can trace exactly
 * one and assert the stream contains that thread's function and NOT the other's.
 * Opts in via PR_SET_PTRACER_ANY.
 *
 * The two workers are the ONLY sources of call lines here — main blocks in
 * pause() rather than sleeping in a loop. That is deliberate: see main().
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

static long gettid_(void) { return syscall(SYS_gettid); }
static volatile long g_sink;

__attribute__((noinline)) static long alpha_work(long n) {
    long s = 0;
    for (long i = 0; i < n; i++)
        s += i * i + 3;
    return s;
}
__attribute__((noinline)) static long beta_work(long n) {
    long s = 1;
    for (long i = 0; i < n; i++)
        s += i * i + 7;
    return s;
}

/* MANY SMALL calls per round rather than one huge one, and that shape matters to
 * two different views:
 *
 *  --stream counts INSTRUCTIONS, so it only needs the thread to be inside
 *  *_work almost always. 64 iterations of work per ~5 instructions of call
 *  overhead keeps that true (~98% of steps land inside), which is what the --tid
 *  filter smoke relies on.
 *
 *  --tree counts CALLS, and this used to be one alpha_work(8000) call wrapping
 *  an 8000-iteration loop with no calls in it — ONE call line per ~80,000
 *  single-steps. A bounded --tree window therefore contained almost nothing from
 *  these threads. 128x64 keeps the compute per round (~8192 iterations) while
 *  emitting 128 call lines instead of 1.
 */
#define WORK_CALLS 128
#define WORK_ITERS 64

static void *alpha(void *a) {
    (void)a;
    fprintf(stderr, "alpha tid=%ld\n", gettid_());
    fflush(stderr);
    struct timespec nap = {0, 50 * 1000}; /* 0.05 ms */
    for (unsigned r = 0;; r++) {
        for (int k = 0; k < WORK_CALLS; k++)
            g_sink += alpha_work(WORK_ITERS);
        if ((r & 7) == 0)
            nanosleep(&nap, NULL);
    }
    return NULL;
}
static void *beta(void *a) {
    (void)a;
    fprintf(stderr, "beta tid=%ld\n", gettid_());
    fflush(stderr);
    struct timespec nap = {0, 50 * 1000};
    for (unsigned r = 0;; r++) {
        for (int k = 0; k < WORK_CALLS; k++)
            g_sink += beta_work(WORK_ITERS);
        if ((r & 7) == 0)
            nanosleep(&nap, NULL);
    }
    return NULL;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    pthread_t a, b;
    pthread_create(&a, NULL, alpha, NULL);
    pthread_create(&b, NULL, beta, NULL);
    /* BLOCK, do not loop. main used to nanosleep(5ms) forever, which emitted
     * FOUR call lines every 5ms (nanosleep@plt -> clock_nanosleep -> two libc
     * frames) — MEASURED as ~34 of any 40-line --tree window, i.e. it drowned
     * the two worker threads the [tid] test is actually about. It got them for
     * free, too: a sleeping thread costs the single-stepper nothing, so main's
     * line rate is WALL-CLOCK bound while the workers' is stepper-throughput
     * bound. On a slower box main's share only grows and theirs only shrinks,
     * which is precisely how this test went red on CI. pause() blocks forever
     * and emits one call line, ever. */
    for (;;)
        pause();
    return 0;
}
