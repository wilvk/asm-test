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
 *
 * The exec is gated on a real HANDSHAKE — we do not proceed until the kernel
 * says a tracer is attached — so "asmspy was tracing across the exec" is a fact
 * the victim establishes, not a wall-clock bet the smoke makes.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Is somebody tracing us? Reads TracerPid from /proc/self/status.
 *
 * Raw open/read/close rather than stdio ON PURPOSE: the LAST iteration of the
 * wait loop below runs under the single-stepper and is charged to the smoke's
 * step budget, and fopen's buffering + malloc would cost thousands of steps
 * where this costs a few hundred. TracerPid sits in the first ~200 bytes of
 * status (after Name/Umask/State/Tgid/Ngid/Pid/PPid), so one short read covers
 * it. */
static int tracer_attached(void) {
    char buf[1024];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    const char *p = strstr(buf, "TracerPid:");
    if (!p)
        return 0;
    p += sizeof "TracerPid:" - 1;
    while (*p == ' ' || *p == '\t')
        p++;
    return *p != '0';
}

/* Block until a tracer attaches. Bounded (~20s) so a broken run FAILS rather
 * than hangs — an unbounded wait here would turn a missing attach into a
 * timeout, which is a much worse diagnostic than "no tracer attached". */
static int wait_until_traced(void) {
    for (int i = 0; i < 20000; i++) {
        if (tracer_attached())
            return 1;
        usleep(1000);
    }
    return 0;
}

int main(int argc, char **argv) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: exec_victim <stage2-binary>\n");
        return 2;
    }
    fprintf(stderr, "exec_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    /* Wait until a tracer is ACTUALLY attached, rather than guessing with a
     * clock. This used to be usleep(1500ms) racing the smoke's sleep 1, which
     * left ~500ms of margin for asmspy to start and seize; on a slow, loaded box
     * that margin is not guaranteed. A late attach makes the preexec_fn control
     * fail loudly rather than silently — but a loud flake is still a flake, and
     * the fix is for the victim to observe the attach itself. */
    if (!wait_until_traced()) {
        fprintf(stderr, "exec_victim: no tracer attached — giving up\n");
        return 3;
    }

    /* A bounded pre-exec phase: enough calls for the tracer to name preexec_fn
     * (the control proving the pre-exec table worked), then exec. Bounded by a
     * COUNT, and with no sleep: we are already being single-stepped by the time
     * we get here, so every usleep here was pure libc noise charged to the
     * smoke's step budget for no benefit. */
    volatile long sink = 0;
    for (int i = 0; i < 12; i++)
        sink += preexec_fn(i);
    (void)sink;

    char *args[] = {argv[1], NULL};
    execv(argv[1], args);
    perror("execv"); /* only reached if the exec failed */
    return 127;
}
