/*
 * dataflow_objid.c — Phase 4 (increment 4): real OBJECT identity over an L0
 * value trace. See asmtest_valtrace.h.
 *
 * Increment 2 (dataflow_gcmove.c) keys managed memory def-use on ADDRESSES: it
 * forwards every pre-compaction access to the object's final resting place.
 * That reconnects an object's own def-use across a GC and separates an
 * unrelated object that later reuses a vacated slot — it correctly shipped and
 * nothing here changes it. What address identity cannot express is one
 * residual: a pre-window record touching memory that a GC then slides a LIVE
 * object into ALIASES that object, forging a def-use edge between two things
 * that were never the same object.
 *
 * This pass adds real object identity. A heap SNAPSHOT — live {addr, size,
 * type_id} nodes in the snapshot address space (after every move has been
 * applied) — is joined with the increment-2 move set (extended to keep
 * recording until the snapshot). asmtest_objid_locate INVERTS
 * asmtest_gcmove_canon (walk the batches DESCENDING by step, inverting the
 * at-most-one range whose NEW span covers the running address), so for any
 * record (step s, raw address x) we can ask which node occupied x at step s:
 *   - owned byte             -> key (node.addr + offset): object identity, equal
 *                               to the forward canon for genuinely-owned bytes.
 *   - no owner, no collision -> key canon(addr): the increment-2 address floor.
 *   - no owner, canon(addr) inside a live node -> key canon(addr) | NOOBJ: the
 *                               false alias, re-keyed out of the object's space.
 *
 * The inverse walk needs the new spans disjoint within a batch. A single GC's
 * batch has that (two live objects cannot occupy one address); a COMPOSED batch
 * (the lane pre-chains same-boundary GCs) can legitimately emit overlapping new
 * spans when an object dies mid-window and a later GC slides another into its
 * image. Those overlaps are DROPPED for the inverse walk only (both sides — a
 * dropped range inverts nothing, i.e. a conservative "no owner", never a guessed
 * identity), mirroring the tier's rvec_disjointify policy but on the NEW spans.
 *
 * PURE C — no Capstone, no Unicorn, no runtime — the same dependency tier as
 * dataflow.c / dataflow_gcmove.c, so it compiles and unit-tests on every host.
 * It links against dataflow_gcmove.o for the forward canon it refines.
 */
#include "asmtest_valtrace.h"

#include <stdlib.h>
#include <string.h>

uint64_t asmtest_objid_locate(const asmtest_gcmove_t *moves, size_t nmoves,
                              uint32_t step, uint64_t snap_addr) {
    if (moves == NULL || nmoves == 0)
        return snap_addr;

    /* Walk the compactions in REVERSE timeline (step) order — the inverse of
     * asmtest_gcmove_canon's ascending forward walk — inverting `cur` through
     * every batch whose boundary POSTDATES `step`. `moves` is sorted ascending
     * by step (precondition), so ties form contiguous batches; within a batch
     * the NEW ranges are disjoint (the caller drops overlaps), so `cur` matches
     * at most ONE new span and inverting exactly one range per batch is well
     * defined. Iterate from the high end to visit batches highest-step first. */
    uint64_t cur = snap_addr;
    size_t j = nmoves;
    while (j > 0) {
        uint32_t bstep = moves[j - 1].step;
        size_t i = j;
        while (i > 0 && moves[i - 1].step == bstep)
            i--;
        if (bstep > step) {
            for (size_t k = i; k < j; k++) {
                if (cur >= moves[k].new_base &&
                    cur - moves[k].new_base < moves[k].len) {
                    cur = moves[k].old_base + (cur - moves[k].new_base);
                    break; /* at most one inversion per batch */
                }
            }
        }
        j = i;
    }
    return cur;
}

/* Sort key: batches by step ascending, and by new_base ascending WITHIN a batch
 * (the order the running-max new-span disjointify needs). With disjoint OLD
 * ranges a batch's forward canon is order-independent, so this ordering is also
 * safe for asmtest_gcmove_canon over the same array. */
static int objid_cmp_step_new(const void *a, const void *b) {
    const asmtest_gcmove_t *x = (const asmtest_gcmove_t *)a;
    const asmtest_gcmove_t *y = (const asmtest_gcmove_t *)b;
    if (x->step != y->step)
        return (x->step > y->step) - (x->step < y->step);
    return (x->new_base > y->new_base) - (x->new_base < y->new_base);
}

/* A private copy of `moves` sorted by (step, new_base); NULL on OOM / empty. */
static asmtest_gcmove_t *objid_sorted_copy(const asmtest_gcmove_t *moves,
                                           size_t nmoves) {
    if (moves == NULL || nmoves == 0)
        return NULL;
    asmtest_gcmove_t *c = (asmtest_gcmove_t *)malloc(nmoves * sizeof *c);
    if (c == NULL)
        return NULL;
    memcpy(c, moves, nmoves * sizeof *c);
    qsort(c, nmoves, sizeof *c, objid_cmp_step_new);
    return c;
}

/* Enforce asmtest_objid_locate's precondition on a (step, new_base)-sorted array:
 * within each batch drop BOTH sides of any NEW-span overlap, compacting in place.
 * Mirrors gccanon_tracer.c's rvec_disjointify (a running max of the ends, plus a
 * one-ahead look that drops the long range which out-spans its first overlapper)
 * but on the NEW spans, because the inverse walk matches on new spans. A dropped
 * range inverts nothing — a conservative "no owner", never a guessed identity.
 * Returns the compacted length. */
static size_t objid_disjointify_new(asmtest_gcmove_t *m, size_t n) {
    size_t w = 0, i = 0;
    while (i < n) {
        uint32_t bstep = m[i].step;
        size_t j = i;
        while (j < n && m[j].step == bstep)
            j++;
        /* batch [i, j), sorted by new_base */
        uint64_t maxend = 0;
        int have = 0;
        for (size_t k = i; k < j; k++) {
            int bad =
                (have && m[k].new_base < maxend) ||
                (k + 1 < j && m[k + 1].new_base < m[k].new_base + m[k].len);
            uint64_t end = m[k].new_base + m[k].len;
            if (!have || end > maxend) {
                maxend = end;
                have = 1;
            }
            if (bad)
                continue;
            m[w++] = m[k];
        }
        i = j;
    }
    return w;
}

/* Find the node containing `raw_addr` at `step` over an ALREADY-prepared (sorted
 * + new-disjoint) move array. First matching node in `nodes` order — snapshot
 * nodes are disjoint, so at most one matches for well-formed input. */
static int objid_owner_prepared(const asmtest_gcnode_t *nodes, size_t nnodes,
                                const asmtest_gcmove_t *dj, size_t ndj,
                                uint32_t step, uint64_t raw_addr, size_t *owner,
                                uint64_t *offset) {
    for (size_t i = 0; i < nnodes; i++) {
        if (nodes[i].size == 0)
            continue;
        uint64_t base = asmtest_objid_locate(dj, ndj, step, nodes[i].addr);
        if (raw_addr >= base && raw_addr - base < nodes[i].size) {
            if (owner != NULL)
                *owner = i;
            if (offset != NULL)
                *offset = raw_addr - base;
            return 0;
        }
    }
    return -1;
}

int asmtest_objid_owner(const asmtest_gcnode_t *nodes, size_t nnodes,
                        const asmtest_gcmove_t *moves, size_t nmoves,
                        uint32_t step, uint64_t raw_addr, size_t *owner,
                        uint64_t *offset) {
    if (nodes == NULL || nnodes == 0)
        return -1;
    if (moves == NULL || nmoves == 0)
        return objid_owner_prepared(nodes, nnodes, NULL, 0, step, raw_addr,
                                    owner, offset);
    asmtest_gcmove_t *dj = objid_sorted_copy(moves, nmoves);
    if (dj == NULL)
        return -1; /* OOM: cannot run the inverse walk -> conservative miss */
    size_t ndj = objid_disjointify_new(dj, nmoves);
    int rc = objid_owner_prepared(nodes, nnodes, dj, ndj, step, raw_addr, owner,
                                  offset);
    free(dj);
    return rc;
}

size_t asmtest_objid_canonicalize(asmtest_valtrace_t *v,
                                  const asmtest_gcnode_t *nodes, size_t nnodes,
                                  const asmtest_gcmove_t *moves,
                                  size_t nmoves) {
    if (v == NULL)
        return (size_t)-1;
    /* No nodes -> pure address identity, exactly asmtest_gcmove_canonicalize. */
    if (nodes == NULL || nnodes == 0)
        return asmtest_gcmove_canonicalize(v, moves, nmoves);

    /* Two derived move arrays: `full` (sorted, ALL ranges) drives the forward
     * canon — the address floor and the collision test — and computes exactly
     * what asmtest_gcmove_canonicalize would; `dj` (sorted + new-disjoint)
     * drives the inverse owner walk. On OOM fall back to the address floor. */
    asmtest_gcmove_t *full = NULL;
    asmtest_gcmove_t *dj = NULL;
    size_t ndj = 0;
    if (nmoves > 0) {
        full = objid_sorted_copy(moves, nmoves);
        dj = objid_sorted_copy(moves, nmoves);
        if (full == NULL || dj == NULL) {
            free(full);
            free(dj);
            return asmtest_gcmove_canonicalize(v, moves, nmoves);
        }
        ndj = objid_disjointify_new(dj, nmoves);
    }

    size_t changed = 0;
    if (v->recs != NULL) {
        for (size_t r = 0; r < v->recs_len; r++) {
            /* GC moves live in the absolute heap space only; registers and
             * routine-relative offsets are identity-stable already. */
            if (v->recs[r].kind != AT_LOC_MEM_ABS)
                continue;
            uint64_t before = v->recs[r].addr;
            uint32_t step = v->recs[r].step;
            uint64_t after;

            size_t owner = 0;
            uint64_t offset = 0;
            if (objid_owner_prepared(nodes, nnodes, dj, ndj, step, before,
                                     &owner, &offset) == 0) {
                /* Owned byte: object identity. Byte-equal to canon(before) for a
                 * genuinely-owned byte, but computed via the inverse walk so a
                 * dead-object or vacated address never reaches this branch. */
                after = nodes[owner].addr + offset;
            } else {
                /* No owner: the increment-2 address floor, UNLESS the forwarded
                 * address collides with a live node's snapshot range — then it
                 * is the false alias and is re-keyed out of the object space. */
                uint64_t c = asmtest_gcmove_canon(full, nmoves, step, before);
                after = c;
                for (size_t i = 0; i < nnodes; i++) {
                    if (nodes[i].size == 0)
                        continue;
                    if (c >= nodes[i].addr &&
                        c - nodes[i].addr < nodes[i].size) {
                        after = c | ASMTEST_OBJID_NOOBJ;
                        break;
                    }
                }
            }

            if (after != before) {
                v->recs[r].addr = after;
                changed++;
            }
        }
    }

    free(full);
    free(dj);
    return changed;
}
