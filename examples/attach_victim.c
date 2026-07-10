/* attach_victim.c — an ordinary program asm-test did NOT start.
 *
 * It calls a hot function in a loop and prints where that function lives, so a
 * SEPARATE tracer process (examples/attach_trace.c) can attach to this PID and
 * trace one invocation out of band. This is the "trace a process it hasn't
 * started" demo (docs/guides/tracing/hardware-tracing.md, the foreign-attach path).
 *
 * It opts in to being traced by any same-uid process via PR_SET_PTRACER_ANY so the
 * demo runs under a plain `docker run` even where Yama ptrace_scope=1 forbids
 * attaching to a non-descendant. Delete that one prctl() and you instead need
 * CAP_SYS_PTRACE or ptrace_scope=0 — the real permission model for tracing a
 * process you did not start.
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

__attribute__((noinline)) long hotfn(long n, long k) {
    long s = 0;
    for (long i = 0; i < n; i++)
        s += (i & 1) ? i * k : -i; /* a branch, so the trace has >1 block */
    return s;
}

int main(void) {
    /* Let any same-uid process attach (Yama opt-in); harmless where scope==0. */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "victim pid=%d hotfn=%p\n", (int)getpid(), (void *)hotfn);
    fflush(stderr);
    volatile long sink = 0;
    for (;;) {
        sink += hotfn(6, 7);
        usleep(200 *
               1000); /* ~5 Hz: leaves the tracer time to attach + run_to */
    }
    return 0;
}
