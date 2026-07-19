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

/* AVX-512 512-bit capture: asm_call_capture_vec512 marshals vec512_t args into
 * zmm0..7 and captures the whole zmm file zmm0..31 (out[0] = return). x86-64 only;
 * ASM_VCALL512* self-skips a host without AVX-512F. */
extern void vec_add8d(void); /* vec512 vec_add8d(vec512 a, vec512 b), AVX-512 */

TEST(simd, avx512_adds_eight_doubles_512bit) {
    vec512_t a = {.f64 = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0}};
    vec512_t b = {.f64 = {10.0, 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0}};
    vec512_t out[32];
    ASM_VCALL512_2(out, vec_add8d, a, b); /* self-skips without AVX-512 */
    ASSERT_DEQ(out[0].f64[0], 11.0);
    ASSERT_DEQ(out[0].f64[3], 44.0);
    ASSERT_DEQ(out[0].f64[7], 88.0); /* the 8th lane needs the FULL 512 bits */

    vec512_t expect = {.f64 = {11.0, 22.0, 33.0, 44.0, 55.0, 66.0, 77.0, 88.0}};
    ASSERT_VEC512_EQ(out, 0, expect.u8);
}
#elif defined(__aarch64__) && defined(__linux__)
/* SVE scalable-vector capture (Track D): asm_call_capture_sve marshals svec_t
 * args into z0..z7 and captures the whole z/predicate file at whatever VL the
 * host provides (z[0] = return). AArch64 Linux only; ASM_SVCALL_* self-skips a
 * host without SVE (Apple silicon has no non-streaming SVE; a native non-SVE
 * arm64 host also skips — only qemu-user under TCG exposes SVE here). */
extern void sve_addd(void); /* svec sve_addd(svec a, svec b), SVE */

TEST(simd, sve_adds_doubles_at_any_vl) {
    svec_t a = {{0}}, b = {{0}};
    for (int i = 0; i < 32; i++) { /* fill to VLmax so ANY VL is covered */
        a.f64[i] = (double)(i + 1);
        b.f64[i] = 10.0 * (double)(i + 1);
    }
    svec_t z[32];
    spred_t p[16];
    ASM_SVCALL_2(z, p, sve_addd, a, b); /* self-skips without SVE */

    unsigned long vl = asmtest_sve_vl(); /* bytes; >= 16 once here */
    ASSERT_UGE(vl, 16);
    for (unsigned long i = 0; i < vl / 8; i++)
        ASSERT_DEQ(z[0].f64[i], 11.0 * (double)(i + 1));

    svec_t expect = {{0}};
    for (unsigned long i = 0; i < vl / 8; i++)
        expect.f64[i] = 11.0 * (double)(i + 1);
    ASSERT_SVEC_EQ(z, 0, expect.u8); /* compares exactly vl bytes */

    /* ptrue p3.d in the routine: one predicate bit per vector byte, so a
     * .d all-true pattern reads 0x01 in every live predicate byte. */
    for (unsigned long i = 0; i < vl / 8; i++)
        ASSERT_UEQ(p[3].u8[i], 0x01);
}
#endif
