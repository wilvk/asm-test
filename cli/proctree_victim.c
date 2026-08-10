/* proctree_victim.c — a MULTI-PROCESS tree with threads parked in a blocking
 * syscall, for the topology engine's teardown-safety smoke (asmspy --procs
 * --count=calls).
 *
 * WHY THIS VICTIM EXISTS. Every other multi-threaded victim here is a SINGLE
 * process, where the topology engine's single-step teardown is harmless: a
 * followed thread shares the leader's address space, so draining its queued
 * step #DB against the leader's memory reads the right bytes. A followed child
 * PROCESS does not — it has its own (fork+exec) address space. If the engine
 * tables that child without its thread-group id, the teardown drains the
 * child's parked worker against the ROOT leader's memory, decides the wrong
 * thing from unrelated bytes, and the mishandled step outlives the DETACH as a
 * fatal SIGTRAP a second later. That killed multi-process targets — Firefox,
 * a fork-per-request server — on attach. This victim is the shape that catches
 * it: a fork+exec CHILD whose worker threads sit in poll(), plus a hot call
 * loop so --count=calls actually single-steps the tree.
 *
 * THE SHAPE IS THE TEST:
 *  1. The parent fork+execs a CHILD (argv[1]=="child"), so the child is a
 *     SEPARATE process with its OWN address space and ASLR — the case the bug
 *     needs. The child already exists at attach, so it is seized via the
 *     descendant /proc walk (seize_process_tree), the path Firefox hits.
 *  2. The child spawns worker threads that park in poll(), the state a real
 *     content process's event-loop threads sit in almost always.
 *  3. The child prints its pid on fd 3 and a heartbeat on stdout, so the smoke
 *     can attach to the PARENT, detach, and check the child SURVIVED the grace
 *     window (a killed child goes silent — the sigload_victim trick).
 *  4. Both parent and child run a hot call loop so --count=calls has calls to
 *     count and single-steps the whole tree.
 *
 * SIGCHLD is SIG_IGN so a child that does exit is reaped by the kernel and the
 * tracer's waitpid(-1) never trips over a zombie. Opts in via PR_SET_PTRACER_ANY
 * like every other victim here.
 */
#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
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

/* A worker that spends almost all its time inside poll(), like a content
 * process's event-loop thread — the parked-in-a-syscall state the teardown
 * drain mishandled when it forgot the child's address space. A short timeout
 * keeps it USUALLY in the syscall while still letting the topology engine reach
 * its call budget and detach CLEANLY (a longer block would make single-stepping
 * to the budget so slow the smoke's `timeout` would kill the tracer mid-run —
 * the kernel then auto-detaches and the child survives regardless, a vacuous
 * pass). This one stays parked enough to sit in poll at teardown, yet lets the
 * run finish. */
static void *parker(void *arg) {
    (void)arg;
    for (;;)
        poll(NULL, 0, 10);
    return NULL;
}

/* A real call site for --count=calls to single-step and count. */
static __attribute__((noinline)) long spin_once(long x) {
    return x * 1103515245 + 12345;
}

static int run_child(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, parker, NULL);
    char msg[32];
    int n = snprintf(msg, sizeof msg, "%d\n", (int)getpid());
    if (write(3, msg, (size_t)n) < 0) { /* fd 3 may be closed in manual runs */
    }
    /* a tight call loop with NO sleep, so the topology engine's single-step
     * counter reaches its budget quickly and detaches cleanly. A heartbeat
     * every ~1M iters keeps a killed child observably silent without slowing
     * the call rate. */
    long acc = 1;
    for (long i = 0;; i++) {
        acc = spin_once(acc);
        if ((i & 0xfffff) == 0) {
            char h = '.';
            if (write(1, &h, 1) < 0)
                _exit(2);
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0)
        return run_child();

    signal(SIGCHLD, SIG_IGN);
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    pid_t kid = fork();
    if (kid == 0) {
        execl(argv[0], argv[0], "child", (char *)NULL);
        _exit(127);
    }
    /* the parent goes quiet after forking so almost every single-step the
     * topology engine takes lands on the CHILD's threads — where the bug is */
    (void)kid;
    for (;;) {
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return 0;
}
