/* exec_victim.c — a LAUNCHER victim for the exec-stop re-resolution smoke
 * (asmspy-plan Theme B).
 *
 * Reproduces the shape that goes wrong: a process asmspy attaches to, which then
 * execve()s a DIFFERENT binary. The symtab asmspy loaded at attach describes
 * only this image; after the exec the text, the load bias, and every symbol
 * belong to exec_stage2 instead. A tracer that does not re-resolve keeps naming
 * the new code from this binary's table.
 *
 * Two distinct, resolvable functions make that observable from the outside:
 * preexec_fn lives HERE and can only ever be seen before the exec; postexec_fn
 * lives in exec_stage2 and can only ever be named AFTER a successful reload.
 *
 * argv[1] is the stage-2 binary to exec (the smoke passes $BUILD/exec_stage2).
 * Opts in via PR_SET_PTRACER_ANY like the other example victims.
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* noinline + a real call so the tracer sees an entry to name */
__attribute__((noinline)) long preexec_fn(long x) { return x * 11 + 3; }

int main(int argc, char **argv) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: exec_victim <stage2-binary>\n");
        return 2;
    }
    fprintf(stderr, "exec_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    /* Give the smoke time to attach BEFORE the exec — the whole point is to be
     * tracing across it, not to attach to the post-exec image. */
    usleep(1500 * 1000);

    /* A bounded pre-exec phase: enough calls for the tracer to name preexec_fn
     * (the control proving the pre-exec table worked), then exec. Bounded by a
     * COUNT, not a clock, so a single-stepped run reaches the exec too. */
    volatile long sink = 0;
    for (int i = 0; i < 12; i++) {
        sink += preexec_fn(i);
        usleep(2 * 1000);
    }
    (void)sink;

    char *args[] = {argv[1], NULL};
    execv(argv[1], args);
    perror("execv"); /* only reached if the exec failed */
    return 127;
}
