/*
 * test_qmul.c — DSP / fixed-point kernel: a Q15 multiply with rounding, driven
 * by the differential / property engine. Q15 represents [-1, 1) as a 16-bit
 * fraction; the multiply is (a*b + 0x4000) >> 15. A naive C model is the
 * reference, and the framework fuzzes tens of thousands of operand pairs against
 * it — the rounding term and the arithmetic shift on negatives are where a
 * hand-written kernel diverges.
 */
#include "asmtest.h"

extern long qmul_q15(long a, long b);

/* The obvious C reference model. */
static long ref_qmul_q15(long a, long b) { return (a * b + 0x4000) >> 15; }

/* Operands across the full signed Q15 range. */
static int gen_q15(asmtest_rng_t *rng, long *v, int cap) {
    (void)cap;
    v[0] = asmtest_rng_range(rng, -32768, 32767);
    v[1] = asmtest_rng_range(rng, -32768, 32767);
    return 2;
}

TEST(dsp, qmul_q15_known_values) {
    ASSERT_EQ(qmul_q15(16384, 16384), 8192);  /* 0.5 * 0.5 = 0.25 */
    ASSERT_EQ(qmul_q15(0, 12345), 0);
    ASSERT_EQ(qmul_q15(-16384, 16384), -8192); /* -0.5 * 0.5 = -0.25 */
}

TEST(dsp, qmul_q15_matches_model) {
    ASSERT_MATCHES_REF2(qmul_q15, ref_qmul_q15, gen_q15, 50000);
}
