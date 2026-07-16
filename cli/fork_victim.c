/* fork_victim.c — a FORKING victim for the child-following smoke
 * (asmspy-plan Theme B, `strace -f` parity).
 *
 * Shape, and why each piece is there:
 *
 *  * parent and child each run a DISTINCT function (parent_fn / child_fn), so a
 *    single-step view proves which processes are actually being followed.
 *
 *  * parent and child each open a DIFFERENT file AFTER the fork, so both land on
 *    the SAME fd number (3 — stdin/stdout/stderr take 0..2, and nothing else is
 *    open yet). This is deliberate and is the sharpest assertion in the suite: a
 *    followed child has its own fd table, so if asmspy resolves the child's
 *    write(3) through the PARENT's /proc/<pid>/fd it reports the parent's path —
 *    the right syscall with a confidently wrong argument, which renders as a
 *    perfectly plausible log line. Same fd number, different file = that bug
 *    cannot hide.
 *
 *  * argv[1], if given, is a binary the CHILD execs after a few iterations. That
 *    puts a followed child into an image of its own, so per-process SYMBOL
 *    resolution is exercised too (the child's symbols and the parent's are then
 *    different tables entirely).
 *
 * Opts in via PR_SET_PTRACER_ANY, in both processes, like the other victims.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#define PARENT_PATH "/tmp/asmspy_fork_parent.txt"
#define CHILD_PATH  "/tmp/asmspy_fork_child.txt"

__attribute__((noinline)) long parent_fn(long x) { return x * 13 + 1; }
__attribute__((noinline)) long child_fn(long x) { return x * 17 + 2; }

int main(int argc, char **argv) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "fork_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    /* Let the tracer attach before the fork. PTRACE_O_TRACEFORK reports forks
     * that happen WHILE traced; it cannot retroactively adopt a child that
     * already existed at attach (same as `strace -f -p`, which also only picks
     * up children forked after it attaches — `--procs` is the view that rescans
     * /proc for pre-existing children). So the smoke must be attached before
     * this fork or it would be testing nothing. 2s against the smoke's 1s
     * settle leaves a full second of margin under load. */
    usleep(2000 * 1000);

    pid_t kid = fork();
    if (kid < 0) {
        perror("fork");
        return 1;
    }

    volatile long sink = 0;
    if (kid == 0) { /* ---- child ---- */
        prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        /* Die with the parent. The smoke only knows the parent's pid, and an
         * orphaned child looping forever would outlive the test run (and, if it
         * exec'd, under a different name). Survives execve. */
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
        /* opened AFTER the fork => fd 3 in the child's OWN fd table */
        int fd = open(CHILD_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        for (long i = 0;; i++) {
            sink += child_fn(i);
            if (fd >= 0)
                write(fd, "c", 1);
            usleep(5 * 1000);
            /* optional: move into an image of our own, so the tracer needs a
             * per-process symbol table rather than the parent's */
            if (argc >= 2 && i == 3) {
                char *args[] = {argv[1], NULL};
                execv(argv[1], args);
                perror("child execv");
                _exit(127);
            }
        }
    }

    /* ---- parent ---- */
    int fd = open(PARENT_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    for (long i = 0;; i++) {
        sink += parent_fn(i);
        if (fd >= 0)
            write(fd, "p", 1);
        usleep(5 * 1000);
    }
    return 0;
}
