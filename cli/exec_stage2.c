/* exec_stage2.c — the binary exec_victim execve()s into (asmspy-plan Theme B).
 *
 * postexec_fn exists ONLY here. asmspy can name it only if it re-read the
 * symbol table at the exec-stop: the table it loaded at attach was
 * exec_victim's, and this image's text sits at a different load bias entirely.
 * So "postexec_fn [exec_stage2] appeared in the stream" is a direct, positive
 * proof of re-resolution — it cannot be produced by the stale table.
 *
 * Loops forever so the tracer has post-exec code to step for as long as it
 * wants; the smoke kills it.
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

__attribute__((noinline)) long postexec_fn(long x) { return x * 7 + 5; }

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "exec_stage2 pid=%d\n", (int)getpid());
    fflush(stderr);
    volatile long sink = 0;
    for (long i = 0;; i++) {
        sink += postexec_fn(i);
        usleep(2 * 1000);
    }
    return 0;
}
