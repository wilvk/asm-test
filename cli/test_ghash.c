/* test_ghash.c — headless unit test for the open-addressed table index
 * (cli/asmspy_ghash.h), with FORCED collisions.
 *
 * Why forcing matters: the engine's own graphs are far too small to collide
 * (≤7 nodes in a 128-slot table), so the end-to-end smoke cannot tell a
 * correct probe loop from one that trusts the hash and accepts the first
 * occupied slot without comparing the key — MEASURED (asmspy-plan.md Theme E),
 * that mutant emits byte-identical `--graph --json` output on the smoke's
 * graphs, and on a larger graph it silently over-merged an edge (4→3). This
 * test brute-forces keys into one slot so exactly that mutant, the idx+1
 * slot-encoding mutant, and a grow-without-rehash mutant all fail here.
 * Built + run by `make cli-smoke` (no ncurses, no ptrace).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmspy_ghash.h"

static int failures;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (cond)                                                              \
            printf("ok - %s\n", what);                                         \
        else {                                                                 \
            fprintf(stderr, "FAIL %s\n", what);                                \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* The table under index: plain keys, entry k's key is keys[k]. */
static uint64_t keys[512];
static size_t nkeys;

static uint64_t hashof(const void *ctx, size_t k) {
    (void)ctx;
    return asmspy_gh_mix(keys[k]);
}

typedef struct {
    uint64_t key;
} eq_ctx;
static int eq(const void *ctx, size_t k) {
    const eq_ctx *c = ctx;
    return keys[k] == c->key;
}

/* Append `key` to the table and index it (grow-then-put, the engine's order). */
static void put_key(asmspy_ghash_t *h, uint64_t key) {
    keys[nkeys] = key;
    if (asmspy_gh_grow(h, nkeys, NULL, hashof) == 0)
        asmspy_gh_put(h, asmspy_gh_mix(key), nkeys);
    nkeys++;
}

static long find_key(const asmspy_ghash_t *h, uint64_t key) {
    eq_ctx c = {key};
    return asmspy_gh_find(h, asmspy_gh_mix(key), &c, eq);
}

int main(void) {
    /* --- unallocated-index degradation contract (cap == 0) ------------- */
    {
        asmspy_ghash_t h = {0};
        asmspy_gh_put(&h, asmspy_gh_mix(0x1000), 0); /* must not crash */
        eq_ctx c = {0x1000};
        CHECK(asmspy_gh_find(&h, asmspy_gh_mix(0x1000), &c, eq) == -1 &&
                  h.slot == NULL && h.cap == 0,
              "cap==0: put is a no-op and find answers 'no index' (-1)");
    }

    /* --- entry 0 findable: the slot encoding is index+1 ---------------- */
    asmspy_ghash_t h = {0};
    nkeys = 0;
    put_key(&h, 0x400000); /* entry index 0 — 0 in a slot must mean EMPTY */
    CHECK(find_key(&h, 0x400000) == 0,
          "entry 0 is findable (slot stores idx+1, so 0 stays 'empty')");
    CHECK(h.cap == 128, "first grow allocates the 128-slot table");

    /* --- forced collisions: three distinct keys, one slot --------------- */
    /* Brute-force two MORE keys landing in entry 0's slot of the 128-slot
     * table. Alignment-shaped keys (function-address-like, low bits zero) so
     * the search is honest about the mixer's real input distribution. */
    size_t slot0 = (size_t)(asmspy_gh_mix(0x400000) & (h.cap - 1));
    uint64_t coll[2];
    size_t ncoll = 0;
    for (uint64_t k = 0x401000; ncoll < 2; k += 0x10) {
        if ((asmspy_gh_mix(k) & (h.cap - 1)) == slot0)
            coll[ncoll++] = k;
    }
    put_key(&h, coll[0]); /* entry 1 — probes past entry 0 */
    put_key(&h, coll[1]); /* entry 2 — probes past entries 0 and 1 */
    CHECK(find_key(&h, coll[1]) == 2,
          "3-deep collision chain: find returns the entry whose KEY matches, "
          "not the first occupied slot (the measured over-merge mutant)");
    CHECK(find_key(&h, coll[0]) == 1,
          "middle of the collision chain resolves by key too");

    /* A key that ROUTES to the same slot but was never inserted: the probe
     * must walk the whole chain and stop at the first empty slot, answering
     * absent — not claim a neighbouring entry. */
    uint64_t ghost = 0;
    for (uint64_t k = 0x900000;; k += 0x10) {
        if ((asmspy_gh_mix(k) & (h.cap - 1)) == slot0 && k != 0x400000 &&
            k != coll[0] && k != coll[1]) {
            ghost = k;
            break;
        }
    }
    CHECK(find_key(&h, ghost) == -1,
          "absent key that hashes into the chain answers -1, not a neighbour");

    /* --- growth rehashes: the chain survives two doublings --------------- */
    /* 128 slots grow past 70% load at ~90 entries; add 300 to force multiple
     * doublings, then require every key — the collided three included — to
     * still resolve. A grow that fails to re-insert (or re-inserts under the
     * wrong hash) loses exactly these. */
    for (uint64_t k = 0; k < 300; k++)
        put_key(&h, 0xA00000 + k * 0x40);
    CHECK(h.cap >= 512, "load factor forced the table past two doublings");
    int all = 1;
    for (size_t k = 0; k < nkeys; k++)
        if (find_key(&h, keys[k]) != (long)k)
            all = 0;
    CHECK(all, "every entry (303) still resolves to its own index after "
               "rehashing, collision chain included");
    CHECK(find_key(&h, 0xDEAD0000) == -1,
          "absent key still absent in the grown table");

    free(h.slot);

    if (failures) {
        fprintf(stderr, "test_ghash: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_ghash: PASS\n");
    return 0;
}
