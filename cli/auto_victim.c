/* auto_victim.c — a victim for `asmspy --dataflow --auto` (asmspy-plan Theme H).
 *
 * THE SHAPE IS THE TEST. Two functions, and the picker must tell them apart —
 * because the obvious rule and the correct rule DISAGREE here, on purpose:
 *
 *   grind_forever()  is entered EXACTLY ONCE (main calls it and it never returns)
 *                    and burns CPU forever. It is where the time goes, so a
 *                    residency/PC histogram — the intuitive "what's hot?" —
 *                    picks it. Feeding it to the data-flow producer arms an int3
 *                    at an entry that can NEVER be reached again: the capture
 *                    blocks until the entry-wait bound gives up. It is the exact
 *                    shape of `main`, of an event loop, of every real program's
 *                    hottest frame.
 *
 *   entered_often()  is called from grind_forever's inner loop, so its ENTRY is
 *                    arrived at constantly. It is the only correct pick, and the
 *                    only one the entry breakpoint can actually catch.
 *
 * So "--auto picked entered_often" cannot pass by accident: a residency rule
 * yields grind_forever, and a rule that ranks the hottest EDGE outright yields
 * grind_forever's loop back-edge (mid-function, unusable as a region). Only a
 * rule that ranks ENTRY ARRIVALS lands here. And grind_forever is the control in
 * the strong sense — the rival answer does not merely differ, it HANGS.
 *
 * quiet_helper() is the module/negative-control third wheel: a sized, resolvable
 * function that is never called at all, so it must never be picked.
 *
 * IBS-Op samples RETIRED OPS of a RUNNING target, so unlike the sleepy victims
 * this one must genuinely spin (attach_victim's 5Hz hotfn yields zero samples in
 * a 400ms window — measured). It yields periodically so it never wedges a core on
 * a shared CI box, exactly like sample_victim.
 *
 * Opts in via PR_SET_PTRACER_ANY like every other victim here: Yama
 * ptrace_scope=1 is the Ubuntu default and is NOT namespaced, so without it the
 * sibling attach is denied and the failure reads like a tracer bug.
 */
#include <sched.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

static volatile long g_sink;

/* Never called. A sized, resolvable symbol that must never be picked — the
 * negative control for "the picker names something it actually observed". */
__attribute__((noinline)) long quiet_helper(long x) { return x * 7 + 1; }

/* Entered constantly: THE correct pick. Small and noinline so it stays a real,
 * sized ELF symbol whose entry address bounds the expected edge. */
__attribute__((noinline)) long entered_often(long x) {
    return x * 2654435761u + 1;
}

/* Entered ONCE, returns NEVER: the residency winner, and the wrong answer. */
__attribute__((noinline)) void grind_forever(void) {
    for (long i = 0;; i++) {
        /* the entry arrivals the picker must find */
        g_sink += entered_often(i);
        /* in-function work, so residency favours THIS frame over the callee */
        for (int j = 0; j < 64; j++)
            g_sink += (i ^ j) * 3;
        if ((i & 0xffff) == 0)
            sched_yield(); /* be a good citizen on a shared box */
    }
}

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "auto_victim pid=%d entered_often=%p grind_forever=%p\n",
            (int)getpid(), (void *)entered_often, (void *)grind_forever);
    fflush(stderr);
    if (getpid() ==
        -1) /* never true: keeps quiet_helper linked, never called */
        g_sink += quiet_helper(1);
    grind_forever();
    return 0;
}
