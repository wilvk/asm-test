/* sample_victim.c — a CPU-BUSY victim for asmspy's --sample (AMD IBS-Op) smoke.
 *
 * The IBS-Op sampler tags retired ops of a RUNNING target, so unlike the other
 * victims (which mostly sleep) this one spins a genuine hot loop: hot_spin()'s
 * conditional back-edge is a taken branch IBS samples on nearly every iteration,
 * so it dominates the statistical hot-edge histogram and the smoke can assert the
 * function is named. noinline keeps it a real, resolvable ELF symbol whose address
 * bounds the expected edge. It stays busy (no usleep) but yields periodically so
 * it never wedges a core on a shared CI box.
 */
#include <sched.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

__attribute__((noinline)) long hot_spin(long n) {
    long s = 0;
    for (long i = 0; i < n; i++) {
        if (i & 1)
            s += i;
        else
            s -= i * 3;
    }
    return s;
}

int main(void) {
    /* --sample uses perf (out of band), not ptrace, but opting in keeps this
     * victim usable by the other attach-based smokes too, and is harmless. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "sample_victim pid=%d\n", (int)getpid());
    fflush(stderr);
    volatile long sink = 0;
    for (;;) {
        sink += hot_spin(2000000); /* a hot back-edge for IBS to sample */
        sched_yield();             /* be a good citizen on a shared box */
    }
    return 0;
}
