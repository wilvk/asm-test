#ifndef ASMTEST_GROW_H
#define ASMTEST_GROW_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* Overflow-checked geometric growth for the framework's malloc/realloc pools
 * (repo-review S6). Doubles the capacity (seeded at 8) until it holds >= `need`
 * elements of `elem` bytes, reallocs the array, returns 1; on OOM or an
 * unrepresentable request returns 0 leaving the array and cap untouched.
 * Hardened form of the repo's
 * `if (n == cap) { nc = cap ? cap * 2 : S; realloc; }` idiom: without the
 * SIZE_MAX/elem clamp a caller-driven `need` (or a cap near 2^63) doubles past
 * SIZE_MAX and wraps ncap to 0 — then realloc(_, 0) hands back a tiny/NULL
 * buffer while cap is recorded huge. Bounded in practice by instruction/memory
 * budgets, so this makes the overflow impossible by construction rather than by
 * luck. `elem` is always a sizeof (>= 1), so SIZE_MAX / elem never divides by 0. */
static inline int asmtest_grow(void **arr, size_t *cap, size_t need,
                               size_t elem) {
    if (need <= *cap)
        return 1;
    size_t maxcap = SIZE_MAX / elem;
    if (need > maxcap)
        return 0;
    size_t ncap = *cap ? *cap : 8;
    while (ncap < need) {
        if (ncap > maxcap / 2) { /* the next double would wrap size_t */
            ncap = need;
            break;
        }
        ncap *= 2;
    }
    void *p = realloc(*arr, ncap * elem);
    if (p == NULL)
        return 0;
    *arr = p;
    *cap = ncap;
    return 1;
}

/* Overflow-checked next power-of-two that is > cap AND >= min_slots (open-
 * addressed pools mask with & (cap - 1), so the capacity must stay a power of
 * two). Returns 1 with *out set, or 0 if no such power of two fits in a size_t
 * for elem-byte slots. Always makes progress past cap (>= one doubling). */
static inline int asmtest_grow_pow2(size_t cap, size_t min_slots, size_t elem,
                                    size_t *out) {
    size_t maxcap = SIZE_MAX / elem;
    size_t ncap = cap ? cap : 1;
    for (;;) {
        if (ncap > maxcap / 2)
            return 0;
        ncap *= 2; /* always makes progress past cap */
        if (ncap >= min_slots)
            break;
    }
    *out = ncap;
    return 1;
}

#endif /* ASMTEST_GROW_H */
