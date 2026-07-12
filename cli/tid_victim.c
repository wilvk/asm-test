/* tid_victim.c — two threads running DISTINCT hot functions (alpha_work /
 * beta_work), for asmspy's per-thread (--tid) filter smoke. Each worker prints
 * its own tid, so the smoke can trace exactly one and assert the stream contains
 * that thread's function and NOT the other's. Opts in via PR_SET_PTRACER_ANY.
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

static void *alpha(void *a) {
    (void)a;
    fprintf(stderr, "alpha tid=%ld\n", gettid_());
    fflush(stderr);
    /* Heavy compute, a tiny nap only every so often, so the thread is almost
     * always executing alpha_work — a bounded single-step window reliably lands
     * in it (the --tid filter smoke asserts alpha_work appears, beta_work never). */
    struct timespec nap = {0, 50 * 1000}; /* 0.05 ms */
    for (unsigned r = 0;; r++) {
        g_sink += alpha_work(8000);
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
        g_sink += beta_work(8000);
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
    struct timespec nap = {0, 5 * 1000 * 1000};
    for (;;)
        nanosleep(&nap, NULL);
    return 0;
}
