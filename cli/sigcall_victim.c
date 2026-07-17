/*
 * sigcall_victim.c — an INDIRECT call racing a signal, for the indirect-call
 * attribution smoke (asmspy-plan Theme C).
 *
 * main loops on an indirect call (through a volatile function pointer, so the
 * compiler cannot devirtualise it into a direct `call rel32`) to
 * indirect_target(). Nothing here raises a signal: the race is forced by the
 * TRACER, which queues one while the tracee sits stopped at the call site with
 * a pending call armed (asmspy's ASMSPY_TEST_SIGRACE lever). That makes the
 * one-instruction window the bug lives in deterministic instead of lucky.
 *
 * The three functions are named so the smoke can tell apart the three things
 * that must be true:
 *
 *   indirect_target  the REAL callee. Must be attributed to the call site — the
 *                    guard against a fix that just drops indirect calls, and
 *                    against a run that never traced the loop at all.
 *   sig_handler_fn   the handler the kernel jumps to. Must NEVER appear as a
 *                    callee: nothing CALLS it, so an edge into it is precisely
 *                    the fabricated attribution under test.
 *   handler_helper   called (directly) only BY the handler. Its appearance is
 *                    what proves the injected signal was really delivered and
 *                    the handler really ran under the tracer — without it, the
 *                    "no sig_handler_fn edge" assertion would pass on a run
 *                    where no signal ever arrived, testing nothing.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

volatile int helper_hits;
volatile int target_hits;

/* Reached ONLY from the handler, by a direct call — the delivery witness. */
__attribute__((noinline)) void handler_helper(void) {
    helper_hits++;
    __asm__ volatile("" ::: "memory");
}

void sig_handler_fn(int s) {
    (void)s;
    handler_helper();
}

/* The real callee of the indirect call. */
__attribute__((noinline)) void indirect_target(void) {
    target_hits++;
    __asm__ volatile("" ::: "memory");
}

/* volatile: forces a reload + a genuine `call *reg/mem` every iteration, which
 * is the whole instruction class under test. Without it the optimiser is free
 * to turn this into a direct call and the fixture would test nothing. */
static void (*volatile fnptr)(void) = indirect_target;

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sig_handler_fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);

    printf("ready\n");
    fflush(stdout);

    /* Spin on the indirect call. No syscall in the loop: every instruction the
     * tracer steps here is the call site, the callee, or the return, so the
     * forced race cannot be spent somewhere uninteresting. */
    for (;;)
        fnptr();
    return 0;
}
