/* asmspy_graphsort.h — the call-graph view's sort comparator, extracted.
 *
 * Ranks asmspy_gnode_t rows for the whole-process call-graph view (headless
 * --graph --sort=invocations|fanout and TUI mode 4 share it). Factored out of
 * asmspy.c so the ordering/tiebreak contract is unit-testable without ncurses
 * or ptrace (cli/test_graphsort.c) — the same header-extraction pattern as
 * asmspy_logview.h.
 *
 * Contract: DESCENDING by the selected key (invocations or fanout); ties break
 * DESCENDING on the other metric; remaining ties break ASCENDING by name, so
 * the displayed order is deterministic across snapshots/qsort implementations.
 */
#ifndef ASMSPY_GRAPHSORT_H
#define ASMSPY_GRAPHSORT_H

#include <string.h>

#include "asmspy.h"

/* How to rank the whole-process call graph. */
typedef enum {
    GSORT_INVOCATIONS = 0, /* most-called functions first                    */
    GSORT_FANOUT = 1,      /* most distinct callees (functions called) first */
} gsort_t;

/* qsort comparator; the active key is set through this file-scope selector
 * right before each qsort (all callers are single-threaded at the sort point —
 * qsort's context-less comparator forces the file-scope latch). */
static gsort_t graph_sort_key = GSORT_INVOCATIONS;
static int gnode_cmp(const void *a, const void *b) {
    const asmspy_gnode_t *x = a, *y = b;
    unsigned long long kx =
        graph_sort_key == GSORT_FANOUT ? x->fanout : x->invocations;
    unsigned long long ky =
        graph_sort_key == GSORT_FANOUT ? y->fanout : y->invocations;
    if (kx != ky)
        return kx < ky ? 1 : -1; /* descending */
    /* tie-break on the OTHER metric, then name, so the order is stable */
    unsigned long long tx =
        graph_sort_key == GSORT_FANOUT ? x->invocations : x->fanout;
    unsigned long long ty =
        graph_sort_key == GSORT_FANOUT ? y->invocations : y->fanout;
    if (tx != ty)
        return tx < ty ? 1 : -1;
    return strcmp(x->name, y->name);
}

#endif /* ASMSPY_GRAPHSORT_H */
