/* int3_victim.c — a victim that executes its OWN int3 breakpoints, for asmspy's
 * app-SIGTRAP (si_code split) smoke.
 *
 * JITs and debuggers embed int3 (0xCC) software breakpoints in their own code and
 * handle the resulting SIGTRAP themselves. A single-step tracer must tell that
 * app-delivered trap apart from its own single-step trap (via PTRACE_GETSIGINFO
 * si_code) and RE-INJECT it, or the app's breakpoint logic silently breaks — and
 * it must re-inject via PTRACE_CONT, since re-arming the single-step Trap Flag
 * fires a fatal #DB inside the (SIGTRAP-masked) handler and kills the target.
 *
 * This victim installs a SIGTRAP handler that records "my breakpoint fired", then
 * loops executing int3. If the handler did NOT run (the trap was swallowed by a
 * tracer), it prints "SWALLOWED" to stdout. So:
 *   - untraced, or under a CORRECT tracer (re-inject via CONT): handler runs every
 *     time, nothing is printed, the victim lives.
 *   - under a tracer that SWALLOWS the trap: it prints SWALLOWED.
 *   - under a tracer that re-injects via SINGLESTEP: the victim is KILLED.
 * The smoke asserts the victim survives AND never prints SWALLOWED. x86-64.
 * Opts in via PR_SET_PTRACER_ANY like the other example victims.
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
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

static volatile sig_atomic_t g_handled;
static void on_sigtrap(int s) {
    (void)s;
    g_handled = 1;
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigtrap;
    sigemptyset(
        &sa.sa_mask); /* SIGTRAP still masked inside the handler by default */
    sigaction(SIGTRAP, &sa, NULL);

    fprintf(stderr, "int3_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    struct timespec nap = {0, 50 * 1000 * 1000}; /* 50 ms between breakpoints */
    for (;;) {
        g_handled = 0;
        __asm__ __volatile__("int3"); /* the app's own software breakpoint */
        if (!g_handled)
            /* our handler never ran -> a tracer swallowed the trap. write()
             * (not stdio) so the record survives even unflushed. */
            write(STDOUT_FILENO, "SWALLOWED\n", 10);
        nanosleep(&nap, NULL);
    }
    return 0;
}
