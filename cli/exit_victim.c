/*
 * exit_victim.c — the one victim that EXITS on its own, for the negative-`n`
 * "run until exit" smoke (asmspy-plan Theme D). Every OTHER example victim loops
 * forever, which is exactly why that row was never testable: `--log <pid> -1` /
 * `--stream <pid> -1` are documented to run until every tracee is gone and then
 * return rc 0, and nothing here ever left.
 *
 * Shape mirrors argdecode_victim: opt in via PR_SET_PTRACER_ANY, print "ready",
 * then ~400 iterations of a 5 ms nanosleep + getpid() (≈2 s of decodable
 * syscalls, and few enough instructions that a single-step run also finishes in
 * seconds), then return 0. The ~2 s runtime against the smoke's 1 s settle
 * guarantees the attach happens while it is still alive.
 */
#define _GNU_SOURCE
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

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    printf("ready\n");
    fflush(stdout);

    for (int i = 0; i < 400; i++) {
        struct timespec ts = {0, 5 * 1000 * 1000}; /* 5 ms */
        nanosleep(&ts, NULL);
        getpid();
    }
    return 0;
}
