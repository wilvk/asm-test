/*
 * dataflow_gcmove.c — Phase 4 (increment 2): GC-move address canonicalization
 * over an L0 value trace. See asmtest_valtrace.h.
 *
 * The hard half of the managed-interpretability layer. A compacting .NET GC
 * relocates a live object from OldRangeBase to NewRangeBase; a raw value trace
 * captured across the compaction then keys its memory def-use on addresses that
 * alias FALSELY — the store before the move (old address) and the load after it
 * (new address) look unrelated, while an unrelated object that later occupies
 * the vacated old address aliases the first. This pass consumes the move-range
 * records EventPipe's GCBulkMovedObjectRanges publishes ({old_base, new_base,
 * len}, tagged with the trace-step boundary the GC takes effect at) and rewrites
 * every absolute memory address to a STABLE canonical identity — the object's
 * FINAL resting place — so def-use survives the compaction and the offset within
 * the object (its (object, field) identity) is preserved.
 *
 * The canonical address is the object's FINAL address rather than its original
 * one on purpose: forwarding a PRE-move access to where the object ends up (and
 * leaving POST-move accesses in place) keeps an object's own def and use on the
 * same key WHILE keeping an unrelated object that reuses the vacated old slot on
 * a DIFFERENT key. Mapping back to the original address would instead re-alias
 * the two, since both used the old address (at different times).
 *
 * PURE C — no Capstone, no Unicorn, no runtime — the same dependency tier as
 * dataflow.c, so it compiles and unit-tests on every host. The LIVE EventPipe
 * feed (GCBulkMovedObjectRanges events -> asmtest_gcmove_t at step boundaries)
 * is a later increment; this is the pure transform it drives.
 */
#include "asmtest_valtrace.h"

#include <stdlib.h>
#include <string.h>

uint64_t asmtest_gcmove_canon(const asmtest_gcmove_t *moves, size_t nmoves,
                              uint32_t step, uint64_t phys) {
    if (moves == NULL || nmoves == 0)
        return phys;

    /* Walk the compactions in timeline (step) order, forwarding `cur` through
     * every move whose boundary POSTDATES this record's step. `moves` is sorted
     * ascending by step (precondition), so ties form contiguous GROUPS: one
     * GCBulkMovedObjectRanges batch. Within a batch the old ranges are disjoint,
     * so an address matches at most ONE range; applying exactly one relocation
     * per batch (then advancing past the group) avoids double-applying when a
     * relocated address happens to fall inside another entry's OLD range in the
     * same batch (compaction can slide a new range over a sibling's old span). */
    uint64_t cur = phys;
    size_t i = 0;
    while (i < nmoves) {
        uint32_t bstep = moves[i].step;
        size_t j = i;
        while (j < nmoves && moves[j].step == bstep)
            j++;
        if (bstep > step) {
            for (size_t k = i; k < j; k++) {
                if (cur >= moves[k].old_base &&
                    cur - moves[k].old_base < moves[k].len) {
                    cur = moves[k].new_base + (cur - moves[k].old_base);
                    break; /* at most one relocation per batch */
                }
            }
        }
        i = j;
    }
    return cur;
}

static int gcmove_cmp_step(const void *a, const void *b) {
    uint32_t x = ((const asmtest_gcmove_t *)a)->step;
    uint32_t y = ((const asmtest_gcmove_t *)b)->step;
    return (x > y) - (x < y);
}

size_t asmtest_gcmove_canonicalize(asmtest_valtrace_t *v,
                                   const asmtest_gcmove_t *moves,
                                   size_t nmoves) {
    if (v == NULL)
        return (size_t)-1;
    if (moves == NULL || nmoves == 0)
        return 0;

    /* asmtest_gcmove_canon requires the moves sorted ascending by step. Use the
     * caller's array directly when it is already sorted (the usual EventPipe-
     * ordered case, allocation-free); otherwise sort a private copy. On an
     * allocation failure fall back to the caller's order (best effort). */
    const asmtest_gcmove_t *sorted = moves;
    asmtest_gcmove_t *copy = NULL;
    bool ordered = true;
    for (size_t i = 1; i < nmoves; i++)
        if (moves[i].step < moves[i - 1].step) {
            ordered = false;
            break;
        }
    if (!ordered) {
        copy = (asmtest_gcmove_t *)malloc(nmoves * sizeof *copy);
        if (copy != NULL) {
            memcpy(copy, moves, nmoves * sizeof *copy);
            qsort(copy, nmoves, sizeof *copy, gcmove_cmp_step);
            sorted = copy;
        }
    }

    size_t changed = 0;
    if (v->recs != NULL) {
        for (size_t r = 0; r < v->recs_len; r++) {
            /* GC moves live in the absolute heap space only; registers and
             * routine-relative offsets are identity-stable already. */
            if (v->recs[r].kind != AT_LOC_MEM_ABS)
                continue;
            uint64_t before = v->recs[r].addr;
            uint64_t after =
                asmtest_gcmove_canon(sorted, nmoves, v->recs[r].step, before);
            if (after != before) {
                v->recs[r].addr = after;
                changed++;
            }
        }
    }

    free(copy);
    return changed;
}
