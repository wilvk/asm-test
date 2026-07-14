/*
 * taint_markerless_victim.c — a native workload that calls NO taint markers, for the DR ATTACH
 * tier's MARKER-LESS config path (dynamorio-attach-tier-plan.md, Increment 3). The DR taint
 * client learns WHAT to instrument (region), WHAT to seed, and WHERE to report ENTIRELY from
 * client OPTIONS + runtime module+offset resolution — not from PC-resolved marker clean calls the
 * app emits. This is the config an ATTACHED foreign target needs (it fires no markers), validated
 * here under LAUNCH first (drrun -c <client> region=<mod>+<off> seed=<mod>+<off> ... -- victim).
 *
 * So the region + seed have STABLE, module-relative addresses (resolvable from a symbol offset
 * even under ASLR), both live as STATIC globals:
 *   - g_fixture[] holds the SAME taint_sink_chain bytes the emulator oracle replays (so the
 *     out-of-process oracle diff matches), made executable at runtime via mprotect. Its symbol
 *     offset is what `region=<module>+0x<off>` resolves against.
 *   - g_seedbuf is the seeded 8-byte source the fixture loads; `seed=<module>+0x<off>,8,<color>`.
 * The `attach` arg makes it loop for ~12 s by default (a long-running target the external-attach
 * lane can seize mid-run, Increment 4); an optional 2nd arg overrides the duration in seconds, so
 * the K-round attach/detach CYCLING lane (Increment 5 completion) can hold ONE pid alive long
 * enough for K seize->capture->detach->native rounds. The no-arg default runs the fixture a
 * handful of times and exits (enough for the LAUNCH client, configured at init, to instrument it).
 * It prints its pid + result.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* strtod — optional attach-duration arg */
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* The seeded source (module-relative; `seed=<module>+offsetof(g_seedbuf)`). Value 7 -> the
 * fixture computes 7 + 5 = 12 (jz not taken), the same result the launch/attach oracle expects. */
uint64_t g_seedbuf = 7;

/* The instrumented region: the taint_sink_chain fixture bytes (KEEP IN SYNC with
 * examples/taint_validator.c / taint_workload.c), in a page-aligned static buffer so mprotect can
 * make just it executable and `region=<module>+offsetof(g_fixture)` resolves to its runtime PC. */
__attribute__((aligned(4096))) unsigned char g_fixture[4096] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]   (SEED origin)     */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax (spill)           */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8] (reload)          */
    0x48, 0x01, 0xf1,             /* 0x0d add rcx, rsi     (rcx + flags)     */
    0x74, 0x03,                   /* 0x10 jz 0x15          (SINK: taint ZF)  */
    0x48, 0x89, 0xc8,             /* 0x12 mov rax, rcx                       */
    0xc3,                         /* 0x15 ret                                */
};
#define FIXTURE_LEN 0x16 /* bytes 0x00..0x15 (KEEP IN SYNC) */

typedef long (*fn2_t)(long, long);

static double elapsed_s(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (double)(t1.tv_sec - t0->tv_sec) +
           (double)(t1.tv_nsec - t0->tv_nsec) / 1e9;
}

int main(int argc, char **argv) {
    int attach = (argc > 1 && strcmp(argv[1], "attach") == 0);
    /* Optional attach-window duration in seconds (default 12): the K-round cycling lane needs a
     * longer-lived victim than the single external-attach lane. Bounded so it always terminates. */
    double attach_secs = 12.0;
    if (attach && argc > 2) {
        double d = strtod(argv[2], NULL);
        if (d > 0.0 && d <= 300.0)
            attach_secs = d;
    }
    fprintf(stderr, "MARKERLESS_VICTIM start pid=%d attach=%d secs=%.0f\n",
            (int)getpid(), attach, attach ? attach_secs : 0.0);
    fflush(stderr);

    /* Make ONLY the fixture bytes executable (the region the client is told about). */
    if (mprotect(g_fixture, 4096, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        perror("taint_markerless_victim: mprotect");
        return 2;
    }
    fn2_t fixture = (fn2_t)(void *)g_fixture;

    long r = 0;
    if (attach) {
        /* Long-running: loop the fixture for attach_secs so the external-attach / cycling lanes
         * can seize it mid-run (Increments 4-5). Bounded by wall-clock so it always terminates. */
        struct timespec t0, ts = {0, 20 * 1000 * 1000}; /* 20 ms */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int beat = 0; elapsed_s(&t0) < attach_secs && beat < 1000000;
             beat++) {
            r = fixture((long)(uintptr_t)&g_seedbuf, 5);
            /* Heartbeat every 16 beats (~0.3 s): the attach/detach/cycle lanes assert the victim
             * advances NATIVELY after a detach by counting heartbeats across a ~2 s window, so a
             * dense-enough cadence makes that return-to-native signal robust. */
            if ((beat & 15) == 0) {
                fprintf(stderr, "MARKERLESS_VICTIM heartbeat beat=%d r=%ld\n",
                        beat, r);
                fflush(stderr);
            }
            nanosleep(&ts, NULL);
        }
    } else {
        /* Launch: the client is configured + the region flushed at init (before app entry), so the
         * region is instrumented on its FIRST execution. Run the fixture ONCE for clean 1-hit /
         * 7-step semantics that the out-of-process oracle diffs exactly like the launch lane. */
        r = fixture((long)(uintptr_t)&g_seedbuf, 5);
    }

    fprintf(stderr, "MARKERLESS_VICTIM done r=%ld\n", r);
    fflush(stderr);
    return 0;
}
