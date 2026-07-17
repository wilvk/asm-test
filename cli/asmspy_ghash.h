/* asmspy_ghash.h — open-addressed index over an append-only table.
 *
 * Extracted from asmspy_engine.c (the asmspy_graphsort.h treatment) so the
 * probe/grow/put contracts are unit-testable with FORCED collisions: the
 * engine's own graphs are far too small to collide (measured: ≤7 nodes in a
 * 128-slot table), so an end-to-end smoke cannot distinguish a correct probe
 * loop from one that trusts the hash and skips the key compare — that mutant
 * emits byte-identical output there. test_ghash.c owns that gap.
 *
 * Contracts (all load-bearing, all asserted by the unit test):
 *  - Slots hold `table index + 1`, so 0 means empty and no separate
 *    tombstone/occupancy array is needed (nothing is ever deleted).
 *  - Capacity is a power of two; linear probing; grown below 70% load.
 *  - cap == 0 means UNALLOCATED: put is a no-op, find answers "no index"
 *    (-1), and the caller falls back to its linear scan — slower, never
 *    wrong. On grow-OOM the index is dropped entirely (cap back to 0)
 *    rather than left stale: a half-populated index would MISS and let the
 *    caller append a duplicate entry, which is wrong, whereas the scan is
 *    merely slow.
 *  - The hash ROUTES, the eq callback DECIDES. A find that accepts an
 *    occupied slot without consulting eq over-merges distinct keys the
 *    moment two keys share a slot.
 */
#ifndef ASMSPY_GHASH_H
#define ASMSPY_GHASH_H

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint32_t *slot;
    size_t cap; /* power of two; 0 = unallocated (callers scan linearly) */
} asmspy_ghash_t;

/* splitmix64 finalizer: cheap, and mixes the low bits that matter here —
 * node keys are function addresses, whose low bits are alignment zeros. */
static uint64_t asmspy_gh_mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Probe for the entry whose key hashes to `h64`, asking `eq(ctx, k)` whether
 * table entry k really is the key being sought (compare the KEY, not the
 * hash). Returns the entry index, or -1 when absent — or when the index is
 * unallocated (cap == 0), which callers that keep a linear-scan fallback must
 * distinguish via `cap` BEFORE calling (to them, -1 from an allocated index
 * is authoritative absence; -1 from cap == 0 means "go scan"). */
static long asmspy_gh_find(const asmspy_ghash_t *h, uint64_t h64,
                           const void *ctx, int (*eq)(const void *, size_t)) {
    if (!h->cap)
        return -1;
    size_t m = h->cap - 1, i = (size_t)(h64 & m);
    while (h->slot[i]) {
        size_t k = h->slot[i] - 1;
        if (eq(ctx, k))
            return (long)k;
        i = (i + 1) & m;
    }
    return -1;
}

/* Insert `idx` under hash `h64`. The caller must already have established that
 * the key is absent (it just probed for it). No-op if the index is unallocated. */
static void asmspy_gh_put(asmspy_ghash_t *h, uint64_t h64, size_t idx) {
    if (!h->cap)
        return;
    size_t m = h->cap - 1, i = (size_t)(h64 & m);
    while (h->slot[i])
        i = (i + 1) & m;
    h->slot[i] = (uint32_t)(idx + 1);
}

/* Grow the index to hold `n+1` entries below a 70% load factor, re-inserting
 * every live entry (`hashof(ctx, k)` re-derives entry k's hash). Returns 0 on
 * success (or when no growth is needed), -1 on OOM — after which the index is
 * left EMPTY and the caller scans linearly, so a failure here costs speed,
 * never correctness. */
static int asmspy_gh_grow(asmspy_ghash_t *h, size_t n, const void *ctx,
                          uint64_t (*hashof)(const void *, size_t)) {
    if (h->cap && (n + 1) * 10 < h->cap * 7)
        return 0;
    size_t nc = h->cap ? h->cap * 2 : 128;
    while ((n + 1) * 10 >= nc * 7)
        nc *= 2;
    uint32_t *ns = calloc(nc, sizeof *ns);
    if (!ns) {
        /* DROP the index rather than keep a stale one. The caller skips its
         * put when we fail, so the entry it is adding would exist in the
         * table but not in the index — and the next lookup for that key
         * would MISS and create a duplicate node/edge. Falling back to the
         * linear scan (cap == 0) is slow; a silently duplicated entry is
         * wrong. */
        free(h->slot);
        h->slot = NULL;
        h->cap = 0;
        return -1;
    }
    free(h->slot);
    h->slot = ns;
    h->cap = nc;
    size_t m = nc - 1;
    for (size_t k = 0; k < n; k++) {
        size_t i = (size_t)(hashof(ctx, k) & m);
        while (h->slot[i])
            i = (i + 1) & m;
        h->slot[i] = (uint32_t)(k + 1);
    }
    return 0;
}

#endif /* ASMSPY_GHASH_H */
