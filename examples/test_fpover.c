/*
 * test_fpover.c — FP/vector argument overflow (Phase 6 completion).
 * ASM_FCALLN / ASM_VCALLN pass more than 8 floating-point / vector args, so the
 * 9th and 10th spill onto the stack per the ABI. Verifies both that every arg
 * reaches the routine and that the callee-saved integer registers survive.
 */
#include "asmtest.h"

extern double fp_sum10(double, double, double, double, double, double, double,
                       double, double, double);
extern double fp_stack2(double, double, double, double, double, double, double,
                        double, double, double);
extern void vec_sum10(void); /* vec128 vec_sum10(vec128 a0 .. a9) */

/* 1+2+4+...+512 = 1023; each value is a distinct power of two, so a misplaced
 * or dropped argument changes the sum detectably. */
#define TEN_POWERS 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 512.0

TEST(fpover, ten_doubles_all_contribute) {
    regs_t r;
    ASM_FCALLN(&r, fp_sum10, TEN_POWERS);
    ASSERT_FP_EQ(&r, 1023.0);
}

TEST(fpover, stack_doubles_read_in_order) {
    /* fp_stack2 returns only the two stack args (9th + 10th): 256 + 512. */
    regs_t r;
    ASM_FCALLN(&r, fp_stack2, TEN_POWERS);
    ASSERT_FP_EQ(&r, 768.0);
}

TEST(fpover, ten_doubles_abi_preserved) {
    regs_t r;
    ASM_FCALLN(&r, fp_sum10, TEN_POWERS);
    ASSERT_FP_EQ(&r, 1023.0);
    ASSERT_ABI_PRESERVED(&r);
}

TEST(fpover, ten_vectors_sum_lanewise) {
    /* Lane l of arg k holds 2^k + l, so lane l sums to 1023 + 10*l — distinct
     * per lane, confirming lanes stay independent across the stack spill. */
    regs_t r;
    vec128_t v[10];
    for (int k = 0; k < 10; k++)
        for (int l = 0; l < 4; l++)
            v[k].f32[l] = (float)((1u << k) + (unsigned)l);
    ASM_VCALLN(&r, vec_sum10, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
               v[8], v[9]);
    ASSERT_FEQ(r.vec[0].f32[0], 1023.0f);
    ASSERT_FEQ(r.vec[0].f32[1], 1033.0f);
    ASSERT_FEQ(r.vec[0].f32[2], 1043.0f);
    ASSERT_FEQ(r.vec[0].f32[3], 1053.0f);
}

TEST(fpover, ten_vectors_abi_preserved) {
    regs_t r;
    vec128_t z = {.u64 = {0, 0}};
    ASM_VCALLN(&r, vec_sum10, z, z, z, z, z, z, z, z, z, z);
    ASSERT_ABI_PRESERVED(&r);
}
