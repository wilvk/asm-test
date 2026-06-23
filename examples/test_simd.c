/*
 * test_simd.c — full 128-bit vector capture (Phase 5, SIMD).
 * ASM_VCALLn passes vec128_t args in the vector registers and captures the
 * whole vector file; the vector return is r.vec[0].
 */
#include "asmtest.h"

extern void vec_add4f(void); /* vec128 vec_add4f(vec128 a, vec128 b) */

TEST(simd, adds_four_floats_lanewise) {
    regs_t r;
    vec128_t a = {.f32 = {1.0f, 2.0f, 3.0f, 4.0f}};
    vec128_t b = {.f32 = {10.0f, 20.0f, 30.0f, 40.0f}};
    ASM_VCALL2(&r, vec_add4f, a, b);
    ASSERT_FEQ(r.vec[0].f32[0], 11.0f);
    ASSERT_FEQ(r.vec[0].f32[1], 22.0f);
    ASSERT_FEQ(r.vec[0].f32[2], 33.0f);
    ASSERT_FEQ(r.vec[0].f32[3], 44.0f);
}

TEST(simd, vector_return_bytewise) {
    regs_t r;
    vec128_t a = {.f32 = {1.5f, 2.5f, 3.5f, 4.5f}};
    vec128_t b = {.f32 = {0.5f, 0.5f, 0.5f, 0.5f}};
    ASM_VCALL2(&r, vec_add4f, a, b);
    vec128_t expect = {.f32 = {2.0f, 3.0f, 4.0f, 5.0f}};
    ASSERT_VEC_EQ(&r, 0, expect.u8);
}

TEST(simd, lane_bit_pattern) {
    regs_t r;
    vec128_t a = {.f32 = {0.0f, 0.0f, 0.0f, 0.0f}};
    vec128_t b = {.f32 = {1.0f, 1.0f, 1.0f, 1.0f}};
    ASM_VCALL2(&r, vec_add4f, a, b);
    ASSERT_UEQ(r.vec[0].u32[0], 0x3f800000UL); /* IEEE-754 bits of 1.0f */
}

TEST(simd, abi_preserved_across_vec_call) {
    regs_t r;
    vec128_t z = {.u64 = {0, 0}};
    ASM_VCALL2(&r, vec_add4f, z, z);
    ASSERT_ABI_PRESERVED(&r); /* callee-saved GP registers untouched */
}
