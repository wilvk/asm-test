/*
 * test_fp.c — floating-point return + argument capture (Phase 5, scalars).
 * Uses asm_call_capture_fp via ASM_FCALLn (all-double args) and ASM_MIXCALL
 * (integer-file + FP-file args together); the double return is in r.fret.
 */
#include "asmtest.h"

#include <math.h> /* INFINITY / NAN for the specials generator */

extern double fp_add(double a, double b);
extern double fp_mul(double a, double b);
extern double scale_long(long n, double s);

TEST(fp, add_returns_double) {
    regs_t r;
    ASM_FCALL2(&r, fp_add, 1.5, 2.25);
    ASSERT_FP_EQ(&r, 3.75);
}

TEST(fp, mul_returns_double) {
    regs_t r;
    ASM_FCALL2(&r, fp_mul, 3.0, 0.5);
    ASSERT_FP_EQ(&r, 1.5);
}

TEST(fp, near_tolerates_rounding) {
    /* 0.1 + 0.2 is the classic value one ULP away from 0.3. */
    regs_t r;
    ASM_FCALL2(&r, fp_add, 0.1, 0.2);
    ASSERT_FP_NEAR(&r, 0.3, 1);
}

TEST(fp, negative_and_zero) {
    regs_t r;
    ASM_FCALL2(&r, fp_add, -2.5, 2.5);
    ASSERT_FP_EQ(&r, 0.0);
}

TEST(fp, abi_preserved_across_fp_call) {
    /* Callee-saved integer registers must still be intact after an FP call. */
    regs_t r;
    ASM_FCALL2(&r, fp_mul, 2.0, 4.0);
    ASSERT_FP_EQ(&r, 8.0);
    ASSERT_ABI_PRESERVED(&r);
}

TEST(fp, mixcall_marshals_both_register_files) {
    /* scale_long takes n in the integer file and s in the FP file — the
     * ptr+len+scalar shape. ASM_MIXCALL marshals each parenthesized group into
     * its register file; no hand-built arrays. */
    regs_t r;
    ASM_MIXCALL(&r, scale_long, (21), (2.0));
    ASSERT_FP_EQ(&r, 42.0);
    ASSERT_ABI_PRESERVED(&r);
}

/* --- FP differential testing (ASSERT_MATCHES_FREF) ---------------------- */

static double ref_fadd(double a, double b) { return a + b; }
static double ref_fmul(double a, double b) { return a * b; }

static int gen_fpair(asmtest_rng_t *rng, double *f, int cap) {
    (void)cap;
    /* Dyadic rationals over a wide range: exact doubles, so the model and the
     * routine see identical operands. */
    f[0] = (double)asmtest_rng_range(rng, -(1L << 40), 1L << 40) / 64.0;
    f[1] = (double)asmtest_rng_range(rng, -(1L << 40), 1L << 40) / 64.0;
    return 2;
}

/* Cycle the values where FP bugs hide: signed zeros, infinities, NaN, and the
 * extremes of the finite range. The engine treats NaN-vs-NaN as a match. */
static int gen_fspecials(asmtest_rng_t *rng, double *f, int cap) {
    static const double specials[] = {0.0,     -0.0,    1.0,     -1.0,
                                      INFINITY, -INFINITY, NAN,
                                      2.2250738585072014e-308 /* DBL_MIN */,
                                      1.7976931348623157e+308 /* DBL_MAX */};
    const int k = (int)(sizeof specials / sizeof specials[0]);
    (void)cap;
    f[0] = specials[(int)(asmtest_rng_u64(rng) % (uint64_t)k)];
    f[1] = specials[(int)(asmtest_rng_u64(rng) % (uint64_t)k)];
    return 2;
}

TEST(fp, add_matches_model_bitexact) {
    /* One addsd/fadd vs the C `+`: same IEEE operation, so 0 ulps holds. */
    ASSERT_MATCHES_FREF2(fp_add, ref_fadd, gen_fpair, 10000, 0);
}

TEST(fp, mul_matches_model_on_specials) {
    ASSERT_MATCHES_FREF2(fp_mul, ref_fmul, gen_fspecials, 200, 0);
}
