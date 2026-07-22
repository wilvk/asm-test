/*
 * grow_overflow.c — unit test for the overflow-checked pool-growth helpers
 * (src/asmtest_grow.h, repo-review S6). Proves both the normal geometric-growth
 * path AND the fail-closed clamp at physically-unreachable sizes: without the
 * SIZE_MAX/elem guard the doubling wraps size_t to 0 and realloc(_, 0) hands
 * back a tiny buffer while cap is recorded huge. Standalone (no framework
 * runtime), compiled with -Isrc and run by `make check`, mirroring
 * tests/glob_parity.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "asmtest_grow.h"

static int failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "grow_overflow: FAIL at %s:%d: %s\n", __FILE__,    \
                    __LINE__, #cond);                                          \
            failures++;                                                        \
        }                                                                      \
    } while (0)

int main(void) {
    /* --- asmtest_grow: normal geometric growth --------------------------- */
    {
        long *v = NULL;
        size_t cap = 0;
        CHECK(asmtest_grow((void **)&v, &cap, 1, sizeof *v)); /* seed to 8 */
        CHECK(cap == 8);
        CHECK(v != NULL);
        CHECK(asmtest_grow((void **)&v, &cap, 9, sizeof *v)); /* 8 -> 16 */
        CHECK(cap == 16);
        size_t before = cap;
        CHECK(asmtest_grow((void **)&v, &cap, 3, sizeof *v)); /* need <= cap */
        CHECK(cap == before);                                 /* no-op */
        free(v);
    }

    /* --- asmtest_grow: overflow clamp fails closed ----------------------- */
    {
        void *q = NULL;
        size_t qcap = 0;
        /* need * elem overflows size_t (need > SIZE_MAX / 8) -> refuse, and
         * leave the pool untouched rather than wrap cap to a huge lie. */
        CHECK(asmtest_grow(&q, &qcap, (SIZE_MAX / 8) + 1, 8) == 0);
        CHECK(q == NULL);
        CHECK(qcap == 0);
    }

    /* --- asmtest_grow_pow2: power-of-two growth + clamp ------------------ */
    {
        size_t out = 0;
        CHECK(
            asmtest_grow_pow2(0, 20, sizeof(long), &out)); /* seed reaches 32 */
        CHECK(out == 32);
        CHECK(asmtest_grow_pow2(64, 65, sizeof(long), &out)); /* 64 -> 128 */
        CHECK(out == 128);
        /* A cap already past SIZE_MAX/2/elem cannot double again -> refuse. */
        CHECK(asmtest_grow_pow2((size_t)1 << 62, ((size_t)1 << 62) + 1,
                                sizeof(long), &out) == 0);
    }

    if (failures) {
        fprintf(stderr, "grow_overflow: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("grow_overflow: PASS\n");
    return 0;
}
