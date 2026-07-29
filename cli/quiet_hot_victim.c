/* quiet_hot_victim.c — a region that goes HOT, then QUIET, then hot again.
 *
 * Backs the 39 T4 "continuous survives a quiet window" smoke. A --continuous
 * dataflow capture pinned to hotfn must SURVIVE a stretch where the entry
 * breakpoint fires for no one and keep capturing the next hot burst — rather
 * than ending at the first lull, which is the bug 39 T4 fixes (35's re-arm loop
 * cleared its stop flag only on a productive pass).
 *
 * Unlike attach_victim's uniform ~5 Hz (which offers no quiet stretch a moderate
 * entry wait could ever observe), this deliberately alternates a DENSE hot burst
 * (hotfn every ~2 ms, so any entry wing catches it at once) with a QUIET stretch
 * (~1.5 s in which hotfn is never called, so a moderate entry wait times out and
 * the engine reports DF_PTRACE_NEVER — an armed-but-quiet window, a 0-step
 * df_invocation marker). A long WARMUP hot phase leads, so the capture — whose
 * SEIZE + symtab load takes a moment — reliably catches the FIRST entry before
 * its wait expires (a first quiet pass is NEVER_RAN by design and would end the
 * session before it began). The pattern is phase-independent, so the smoke is
 * not flaky.
 *
 * Opts in to foreign attach via PR_SET_PTRACER_ANY so it runs under a plain
 * `docker run`, exactly as attach_victim / auto_victim do.
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

/* One dense hot burst: `iters` entries of hotfn, ~2 ms apart. */
static void hot_burst(volatile long *sink, int iters) {
    for (int i = 0; i < iters; i++) {
        *sink += hotfn(6, 7);
        usleep(2 * 1000);
    }
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "quiet_hot_victim pid=%d hotfn=%p\n", (int)getpid(),
            (void *)hotfn);
    fflush(stderr);
    volatile long sink = 0;
    /* Warmup: ~2.4 s hot, so a capture attaching now catches the first entry. */
    hot_burst(&sink, 1200);
    for (;;) {
        hot_burst(&sink, 500); /* ~1.0 s hot   */
        usleep(1500 * 1000);   /* ~1.5 s QUIET */
    }
    return 0;
}
