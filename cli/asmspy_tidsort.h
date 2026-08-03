/* asmspy_tidsort.h — order a task-id list: ascending, leader first.
 *
 * Extracted from asmspy_proc.c's per-thread walk (pi_read_threads) so the
 * ordering the "what is it doing now" thread table is displayed in is
 * unit-testable on SYNTHETIC arrays (cli/test_procinfo.c) instead of only
 * reachable through a live /proc/<pid>/task listing. That live listing
 * cannot exercise this at all in practice: Linux hands tids back in
 * creation order (ascending) for any normal process, so a real snapshot's
 * input is already sorted before this function ever runs, and the one case
 * that could disturb that — the leader not being the numerically smallest
 * of its own tids — needs pid wraparound to construct. A synthetic array
 * can build exactly the inputs a live process cannot: leader mid-array,
 * leader last, descending input, duplicate tids, n<=1. Same file-extraction
 * discipline as the other cli/asmspy_*.h view-model modules
 * (asmspy_graphsort.h, asmspy_treefilter.h, asmspy_ghash.h, …): pure,
 * static inline, no I/O, no allocation — and exactly ONE implementation,
 * shared by the production code and its test rather than duplicated.
 *
 * THE ONE INVARIANT: `leader` (if present among tids[0..n)) ends up at
 * index 0, and every OTHER element ends up ascending — never a "the rest is
 * sorted except near wherever the leader used to sit" partial order, which
 * is exactly what a leader SWAP (rather than a rotate) produces. A previous
 * version swapped tids[0] and the leader's slot, which deposited whatever
 * was previously at index 0 (the smallest remaining tid) at the leader's
 * old index — breaking ascending order for every row between the two
 * positions whenever the leader was not already the smallest.
 */
#ifndef ASMSPY_TIDSORT_H
#define ASMSPY_TIDSORT_H

#include <string.h>

/* Sort tids[0..n) ascending in place, then rotate `leader` (if present) to
 * index 0. `n` may be 0 or 1; a `leader` absent from tids[] leaves the
 * sorted array un-rotated (still validly ascending, just with no particular
 * element pinned to the front) rather than corrupting anything. */
static inline void asmspy_tidsort_leader_first(long *tids, int n, long leader) {
    for (int i = 1; i < n; i++) {
        long v = tids[i];
        int j = i - 1;
        while (j >= 0 && tids[j] > v) {
            tids[j + 1] = tids[j];
            j--;
        }
        tids[j + 1] = v;
    }
    /* Rotate, not swap: shift the prefix before the leader's sorted
     * position up one slot (memmove handles the overlap), then place the
     * leader at index 0 — closing the gap it leaves rather than stranding
     * whatever was at index 0 in its place. */
    for (int i = 0; i < n; i++)
        if (tids[i] == leader) {
            long lv = tids[i];
            memmove(&tids[1], &tids[0], (size_t)i * sizeof tids[0]);
            tids[0] = lv;
            break;
        }
}

#endif /* ASMSPY_TIDSORT_H */
