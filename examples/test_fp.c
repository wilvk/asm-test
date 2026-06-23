/*
 * test_fp.c — floating-point return + argument capture (Phase 5, scalars).
 * Uses asm_call_capture_fp via ASM_FCALLn; the double return is in r.fret.
 */
#include "asmtest.h"

extern double fp_add(double a, double b);
extern double fp_mul(double a, double b);

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
