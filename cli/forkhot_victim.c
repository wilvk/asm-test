/* forkhot_victim.c — a victim that FORKS from inside its hot loop, for the
 * perf-free region picker's copy-on-write trap check (cli/test_ptracesample.c).
 *
 * WHY THIS VICTIM EXISTS. Phase 3 of the picker plants an int3 at a candidate
 * entry and leaves it armed for up to 50 ms per candidate. A fork(2) inside that
 * window hands the child a copy-on-write copy of the target's text — INCLUDING
 * our trap byte. The child is a different process with no tracer, so the first
 * time it executes that entry it takes a SIGTRAP whose default action kills it.
 * A fork-per-request server is an entirely ordinary target.
 *
 * Nothing else in this tree could catch that. clone_victim exercises the THREAD
 * path (same address space, covered by PTRACE_O_TRACECLONE); fork_victim forks
 * exactly once, two seconds in, and then sleeps — so it is never hot enough to
 * be nominated and never forks inside an armed window.
 *
 * THE SHAPE IS THE TEST:
 *
 *  1. `hot_entry` is called constantly from the parent's inner loop, so it is
 *     what phase 3 arms. Without that, no window is armed and the check is
 *     vacuous.
 *  2. The CHILD calls both entries after the fork. That is the whole point: an
 *     inherited trap byte is only fatal if the child reaches it — and `hot_entry`
 *     alone is not enough, because its armed window closes in microseconds.
 *  3. The child prints `kid=<n>` and _exit(0)s. A child killed by SIGTRAP prints
 *     NOTHING, so the parent's line rate is a direct, countable measure of
 *     whether children are surviving — the same trick sigload_victim uses for
 *     signal delivery, for the same reason: the failure is silent otherwise.
 *  4. It forks often (~200 Hz) so a sub-second armed window contains several.
 *
 * SIGCHLD is set to SIG_IGN so children are reaped by the kernel and this never
 * accumulates zombies — the tracer's waitpid(-1) must not find a pile of them.
 *
 * A SECOND MODE, ASMSPY_FORKHOT_EXEC=1: the child execve()s itself after
 * running the entries, and the new image sleeps before printing. That is
 * fork+exec, the commonest fork shape there is, and it is the ONE shape that
 * breaks the tracer's restore path — `armed_bp` names an address in the PARENT's
 * image, and after the exec that address is either unmapped or someone else's
 * code entirely. A tracer that treats "PTRACE_PEEKTEXT failed" as "we could not
 * tell" then reports a target it never touched as DAMAGED. The sleep is what
 * makes it reproducible: children are still alive, still traced, and still
 * carrying a stale restore address when the teardown walks the table.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* The entry phase 3 will arm, and the one the child re-enters after the fork. */
__attribute__((noinline)) long hot_entry(long x) { return x * 2654435761u + 1; }

/* The entry that makes this check real. The parent enters it about every 20 ms,
 * so four arrivals cannot be collected inside phase 3's per-candidate budget and
 * the window stays ARMED for the whole of it — around ten forks' worth. And the
 * CHILD enters it immediately after the fork, so a copy-on-write copy of the
 * trap is not merely inherited, it is EXECUTED.
 *
 * Without this the check was hollow: MEASURED, disabling the fork-child release
 * entirely was caught 0 times in 4, because every entry armed while a fork was
 * in flight was a fork-path one (_Fork, __libc_fork, fork@plt) that the child
 * never calls again, and hot_entry's own window closes in microseconds. */
__attribute__((noinline)) long slow_entry(long x) { return x ^ 0x5bf03635; }

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Everything that is not the hot loop, behind ONE call each — and that is a
 * requirement of the test, not tidiness.
 *
 * Phase 2 of the picker admits the DIRECT CALL TARGETS of every shortlisted body
 * as candidates, in the order they appear in that body's bytes, and phase 3 then
 * pays ASMSPY_PS_CONFIRM_MS for each one the target never enters. Spelling this
 * plumbing out inline at the top of main() put four never-entered @plt entries
 * (strcmp, nanosleep, printf, fflush) AHEAD of slow_entry in main's scan, which
 * pushed slow_entry — the one entry whose armed window stays open long enough
 * for a fork to land in it — past phase 3's budget. MEASURED: §6's fork-child
 * check went from catching the release mutation to missing it entirely. */
__attribute__((noinline)) static int forkhot_mode(int argc, char **argv) {
    /* THE EXEC'D CHILD (ASMSPY_FORKHOT_EXEC mode). A brand new image: nothing of
     * the parent's text — and therefore nothing of any trap byte planted in it —
     * exists here any more. It sleeps first so that it is still ALIVE, and still
     * traced, when the sampler tears down and walks its table of "tasks holding
     * a copy of our byte". */
    if (argc > 2 && strcmp(argv[1], "--kid") == 0) {
        struct timespec nap = {0, 40 * 1000 * 1000};
        nanosleep(&nap, NULL);
        printf("kid=%s\n", argv[2]);
        fflush(stdout);
        _exit(0);
    }
    const char *exec_mode = getenv("ASMSPY_FORKHOT_EXEC");
    return exec_mode && *exec_mode && *exec_mode != '0';
}

/* The child's exec, also behind one call, for the same reason. */
__attribute__((noinline)) static void forkhot_exec_kid(char **argv, long seq) {
    char s[32];
    snprintf(s, sizeof s, "%ld", seq);
    execl("/proc/self/exe", argv[0], "--kid", s, (char *)NULL);
    /* exec refused: the caller prints the line itself rather than leaving a hole
     * the test would read as a death. */
}

int main(int argc, char **argv) {
    int do_exec = forkhot_mode(argc, argv);

    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Kernel-reaped children: the tracer runs waitpid(-1) and must not have to
     * pick through this victim's zombies. */
    signal(SIGCHLD, SIG_IGN);

    fprintf(stderr, "forkhot_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    long kids = 0, beats = 0;
    long next = mono_ms() + 5;
    for (long i = 0;; i++) {
        /* Hot enough that hot_entry is what the residency phase nominates and
         * what its callers name. */
        for (int k = 0; k < 500; k++)
            g_sink += hot_entry(i + k);

        long now = mono_ms();
        if (now < next)
            continue;
        next = now + 5;       /* ~200 forks/s */
        if (++beats % 4 == 0) /* ~50 Hz: slow enough to keep a window open */
            g_sink += slow_entry(beats);
        pid_t p = fork();
        if (p == 0) {
            /* THE CHILD. It runs both entries the parent's trap may be planted
             * at; an inherited int3 kills it right here, before it can print. */
            for (int k = 0; k < 200; k++)
                g_sink += hot_entry(k);
            g_sink += slow_entry(beats);
            /* Same sequence number, printed by the new image, so the gap
             * counting the test does is unchanged by the mode. */
            if (do_exec)
                forkhot_exec_kid(argv, kids);
            printf("kid=%ld\n", kids);
            _exit(0);
        }
        if (p > 0)
            kids++;
    }
    return 0;
}
