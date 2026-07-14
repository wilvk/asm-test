/*
 * attach_probe_victim.c — the already-running native process the external-attach probe attaches
 * TO (dynamorio-attach-tier-plan.md, Increment 2). Started FIRST as a PLAIN native process (NOT
 * under drrun); the probe injects DR + the attach_probe client into it mid-run.
 *
 * It prints its pid (so the lane can target the attach), then loops doing real integer work (a
 * volatile arithmetic loop DR can decode + instrument) and prints a VICTIM_HEARTBEAT every ~100 ms.
 * The heartbeats let the lane confirm the victim KEEPS RUNNING while attached (survival) and that
 * it returns to native + exits cleanly after detach. Bounded by a wall-clock deadline (not an
 * iteration count) so it always terminates even when DR's instrumentation slows the work loop.
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static double elapsed_s(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) +
           (double)(t1.tv_nsec - t0->tv_nsec) / 1e9;
}

int main(void) {
    fprintf(stderr, "VICTIM_START pid=%d\n", (int)getpid());
    fflush(stderr);

    volatile uint64_t acc = 1;
    struct timespec sleep_ts = {0,
                                100 * 1000 * 1000}; /* 100 ms between beats */
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int beat = 0;
    for (;;) {
        for (uint64_t i = 0; i < 2000000ULL; i++) /* real work DR instruments */
            acc = acc * 6364136223846793005ULL + i + 1;
        fprintf(stderr, "VICTIM_HEARTBEAT beat=%d acc=%llu\n", beat,
                (unsigned long long)acc);
        fflush(stderr);
        nanosleep(&sleep_ts, NULL);
        beat++;
        if (elapsed_s(&t0) >= 12.0 ||
            beat >= 300) /* ~12 s or 300 beats, bounded */
            break;
    }

    fprintf(stderr, "VICTIM_END beats=%d acc=%llu\n", beat,
            (unsigned long long)acc);
    fflush(stderr);
    return 0;
}
