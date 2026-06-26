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

/* AVX2 256-bit capture (Track D): asm_call_capture_vec256 marshals vec256_t
 * args into ymm0..7 and captures the whole ymm file (out[0] = return). x86-64
 * only (AVX is x86's); ASM_VCALL256* self-skips a host without AVX2. */
#if defined(__x86_64__)
extern void vec_add4d(void); /* vec256 vec_add4d(vec256 a, vec256 b), AVX2 */

TEST(simd, avx2_adds_four_doubles_256bit) {
    vec256_t a = {.f64 = {1.0, 2.0, 3.0, 4.0}};
    vec256_t b = {.f64 = {10.0, 20.0, 30.0, 40.0}};
    vec256_t out[16];
    ASM_VCALL256_2(out, vec_add4d, a, b); /* self-skips without AVX2 */
    ASSERT_DEQ(out[0].f64[0], 11.0);
    ASSERT_DEQ(out[0].f64[1], 22.0);
    ASSERT_DEQ(out[0].f64[2], 33.0);
    ASSERT_DEQ(out[0].f64[3], 44.0); /* the 4th lane needs the FULL 256 bits */

    vec256_t expect = {.f64 = {11.0, 22.0, 33.0, 44.0}};
    ASSERT_VEC256_EQ(out, 0, expect.u8);
}
#endif
