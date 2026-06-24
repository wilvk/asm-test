/*
 * test_bittricks.c — an UNUSUAL USE CASE: treat the property-testing engine as a
 * verifier for a small branchless bit-manipulation library.
 *
 * Bit hacks (SWAR popcount, round-up-to-power-of-two, bit-reversal) are notorious
 * for being correct on hand-picked examples yet subtly wrong at an edge: the
 * all-zero / all-ones word, an exact power of two, the highest bit. A handful of
 * fixed cases will not find those. Instead we pin a few obvious cases AND fuzz
 * thousands of random inputs against an *independent* C model (a plain bit-loop,
 * not the same trick), so any divergence — even one input in 2^40 — is reported
 * with the offending value and the seed to reproduce it.
 */
#include "asmtest.h"

extern unsigned long popcount64(unsigned long x);
extern unsigned long next_pow2(unsigned long x);
extern unsigned long reverse_byte(unsigned long x);

/* --- independent C reference models (deliberately naive bit-loops) -------- */
static long ref_popcount(long x) {
    unsigned long v = (unsigned long)x;
    long c = 0;
    while (v) {
        c += (long)(v & 1u);
        v >>= 1;
    }
    return c;
}

static long ref_next_pow2(long x) {
    unsigned long v = (unsigned long)x, p = 1;
    while (p < v)
        p <<= 1; /* doubles until it reaches/passes x */
    return (long)p;
}

static long ref_reverse_byte(long x) {
    unsigned v = (unsigned)x & 0xFFu, r = 0;
    for (int i = 0; i < 8; i++) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return (long)r;
}

/* --- input generators ---------------------------------------------------- */
/* Full 64-bit patterns: popcount must be right for every bit position. */
static int gen_word(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = (long)asmtest_rng_u64(rng);
    return 1;
}
/* Positive range (well past 32-bit) where next_pow2 is defined and its result
 * still fits in a long; covers exact powers and the values just off them. */
static int gen_pos(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, 1, 1L << 40);
    return 1;
}
/* Every byte value reaches reverse_byte's whole domain. */
static int gen_byte(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, 0, 255);
    return 1;
}

/* --- popcount: fixed corners + a full-width fuzz -------------------------- */
TEST(bittricks, popcount_known_values) {
    ASSERT_EQ(popcount64(0), 0);
    ASSERT_EQ(popcount64(1), 1);
    ASSERT_EQ(popcount64(0xFFFFFFFFFFFFFFFFUL), 64);
    ASSERT_EQ(popcount64(0xAAAAAAAAAAAAAAAAUL), 32);
    ASSERT_EQ(popcount64(0x8000000000000000UL), 1); /* the top bit alone */
}
TEST(bittricks, popcount_matches_model) {
    ASSERT_MATCHES_REF1(popcount64, ref_popcount, gen_word, 20000);
}

/* --- next_pow2: the exact-power edge is where this routine breaks ---------- */
TEST(bittricks, next_pow2_known_values) {
    ASSERT_EQ(next_pow2(1), 1);
    ASSERT_EQ(next_pow2(2), 2);   /* exact power: must NOT double to 4 */
    ASSERT_EQ(next_pow2(3), 4);
    ASSERT_EQ(next_pow2(5), 8);
    ASSERT_EQ(next_pow2(1UL << 20), 1UL << 20);
    ASSERT_EQ(next_pow2((1UL << 20) + 1), 1UL << 21);
}
TEST(bittricks, next_pow2_matches_model) {
    ASSERT_MATCHES_REF1(next_pow2, ref_next_pow2, gen_pos, 20000);
}

/* --- reverse_byte: every input, exhaustively coverable by fuzzing --------- */
TEST(bittricks, reverse_byte_known_values) {
    ASSERT_EQ(reverse_byte(0x01), 0x80);
    ASSERT_EQ(reverse_byte(0x80), 0x01);
    ASSERT_EQ(reverse_byte(0xFF), 0xFF);
    ASSERT_EQ(reverse_byte(0xAA), 0x55);
    ASSERT_EQ(reverse_byte(0x00), 0x00);
}
TEST(bittricks, reverse_byte_matches_model) {
    ASSERT_MATCHES_REF1(reverse_byte, ref_reverse_byte, gen_byte, 20000);
}

/* reverse_byte is an involution: reversing twice is the identity. A property
 * that holds for the whole domain regardless of what the "right" answer is. */
TEST(bittricks, reverse_byte_is_its_own_inverse) {
    for (unsigned b = 0; b < 256; b++)
        ASSERT_EQ(reverse_byte(reverse_byte(b)), b);
}
