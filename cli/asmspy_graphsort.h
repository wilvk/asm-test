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

/* asmspy_gnode_t is an ENGINE type, so this reaches for the engine's public
 * header (libasmspy.h) rather than the CLI front-end's asmspy.h. Graphsort
 * itself stays CLI-side, out of the library's own header set, because of the
 * file-scope qsort latch below. */
#include "libasmspy.h"

/* How to rank the whole-process call graph. */
typedef enum {
    GSORT_INVOCATIONS = 0, /* most-called functions first                    */
    GSORT_FANOUT = 1,      /* most distinct callees (functions called) first */
} gsort_t;

/* Context-explicit core: a pure total order over two rows under an EXPLICIT key,
 * safe to call from any thread. Concurrent consumers — the GUI's multi-panel
 * call graph views (docs/internal/gui/03-desktop-shell.md) — call this directly
 * (e.g. std::sort + a lambda), never the latch below. */
static inline int gnode_cmp_key(const asmspy_gnode_t *x,
                                const asmspy_gnode_t *y, gsort_t key) {
    unsigned long long kx = key == GSORT_FANOUT ? x->fanout : x->invocations;
    unsigned long long ky = key == GSORT_FANOUT ? y->fanout : y->invocations;
    if (kx != ky)
        return kx < ky ? 1 : -1; /* descending */
    /* tie-break on the OTHER metric, then name, so the order is stable */
    unsigned long long tx = key == GSORT_FANOUT ? x->invocations : x->fanout;
    unsigned long long ty = key == GSORT_FANOUT ? y->invocations : y->fanout;
    if (tx != ty)
        return tx < ty ? 1 : -1;
    return strcmp(x->name, y->name);
}

/* qsort(3) adapter over the file-scope latch: the active key is set through this
 * selector right before each qsort. SINGLE-THREADED call sites only (the TUI) —
 * qsort's context-less comparator forces the latch; concurrent consumers (GUI
 * panels) must use gnode_cmp_key instead. The explicit casts also make this
 * header valid C++ (an implicit const void* -> const T* conversion is a C-only
 * allowance), so the desktop can reuse it through vm_compat.cpp. */
static gsort_t graph_sort_key = GSORT_INVOCATIONS;
static int gnode_cmp(const void *a, const void *b) {
    return gnode_cmp_key((const asmspy_gnode_t *)a, (const asmspy_gnode_t *)b,
                         graph_sort_key);
}

#endif /* ASMSPY_GRAPHSORT_H */
