/*
 * test_refmatch.c — differential / property testing (Phase 7).
 *
 * Instead of fixed input vectors, supply both the assembly routine and a C
 * reference model, then fuzz thousands of random inputs and assert the two
 * agree. A generator draws each input tuple from a seeded RNG; on a mismatch
 * the framework reports the offending input (and the seed, to reproduce).
 */
#include "asmtest.h"

extern long imax(long, long);
extern long iabs(long);
extern long iclamp(long, long, long);

/* --- C reference models ------------------------------------------------- */
static long ref_imax(long a, long b) { return a > b ? a : b; }
static long ref_iabs(long x) { return x < 0 ? -x : x; }
static long ref_iclamp(long x, long lo, long hi) {
    if (x < lo)
        return lo;
    if (x > hi)
        return hi;
    return x;
}

/* --- input generators --------------------------------------------------- */
static int gen_pair(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, -1000, 1000);
    a[1] = asmtest_rng_range(rng, -1000, 1000);
    return 2;
}

static int gen_one(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, -1000, 1000);
    return 1;
}

/* Wide range (well past 32-bit) to exercise full 64-bit operands; bounded so
 * the negation in ref_iabs cannot overflow. */
static int gen_one_wide(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, -(1L << 48), (1L << 48));
    return 1;
}

static int gen_clamp(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    long lo = asmtest_rng_range(rng, -1000, 1000);
    long hi = asmtest_rng_range(rng, lo, 1000); /* ensure lo <= hi */
    a[0] = asmtest_rng_range(rng, -1500, 1500); /* x may fall outside [lo,hi] */
    a[1] = lo;
    a[2] = hi;
    return 3;
}

/* --- property tests ----------------------------------------------------- */
TEST(refmatch, max_matches_model) {
    ASSERT_MATCHES_REF2(imax, ref_imax, gen_pair, 10000);
}

TEST(refmatch, abs_matches_model) {
    ASSERT_MATCHES_REF1(iabs, ref_iabs, gen_one, 10000);
}

TEST(refmatch, abs_matches_model_wide) {
    ASSERT_MATCHES_REF1(iabs, ref_iabs, gen_one_wide, 20000);
}

TEST(refmatch, clamp_matches_model) {
    ASSERT_MATCHES_REF3(iclamp, ref_iclamp, gen_clamp, 10000);
}

/* Sanity: a correct routine must agree with itself when its reference is the
 * model, and so must equal-valued inputs (covers the cmov/csel tie case). */
static int gen_equal_pair(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = a[1] = asmtest_rng_range(rng, -1000, 1000);
    return 2;
}
TEST(refmatch, max_handles_equal_operands) {
    ASSERT_MATCHES_REF2(imax, ref_imax, gen_equal_pair, 2000);
}
