/* spy_victim.c — a non-leaf victim for the asmspy call-graph smoke.
 *
 * work() calls helper() (a resolvable local function) in a loop, so tracing
 * work() exercises asmspy's "functions called" view: a descent edge to helper
 * that the ELF symbol resolver names. Opts in via PR_SET_PTRACER_ANY like the
 * other example victims so attach works in a plain container.
 */
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

__attribute__((noinline)) long helper(long x) { return x * x + 1; }

__attribute__((noinline)) long work(long n) {
    long s = 0;
    for (long i = 0; i < n; i++)
        s += helper(i); /* a real call -> a descent edge to helper */
    return s;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "spy_victim pid=%d\n", (int)getpid());
    fflush(stderr);
    volatile long sink = 0;
    for (;;) {
        sink += work(5);
        usleep(200 * 1000);
    }
    return 0;
}
