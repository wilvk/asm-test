/*
 * test_qadd.c — codec / SIMD kernel: per-byte unsigned saturating add, captured
 * lane by lane. ASM_VCALL2 passes the two 128-bit vectors in the vector
 * registers and captures the whole vector file (the return is r.vec[0]). A few
 * fixed edges pin the saturation behaviour, then thousands of random 16-byte
 * vectors are checked against a clamping C model — the standard way to catch a
 * lane that wraps at 0xFF instead of clamping.
 */
#include "asmtest.h"

extern void qadd_u8x16(void); /* vec128 qadd_u8x16(vec128 a, vec128 b) */

TEST(codec, saturating_add_known_edges) {
    regs_t r;
    vec128_t a = {.u64 = {0, 0}};
    vec128_t b = {.u64 = {0, 0}};
    a.u8[0] = 200;
    b.u8[0] = 100; /* 300 -> clamps to 255 */
    a.u8[1] = 10;
    b.u8[1] = 20; /* 30  -> exact          */
    a.u8[2] = 255;
    b.u8[2] = 255; /* 510 -> clamps to 255  */
    a.u8[3] = 255;
    b.u8[3] = 0; /* 255 -> the boundary    */
    ASM_VCALL2(&r, qadd_u8x16, a, b);
    ASSERT_UEQ(r.vec[0].u8[0], 255);
    ASSERT_UEQ(r.vec[0].u8[1], 30);
    ASSERT_UEQ(r.vec[0].u8[2], 255);
    ASSERT_UEQ(r.vec[0].u8[3], 255);
}

TEST(codec, saturating_add_matches_model) {
    asmtest_rng_t rng = {0xC0DEC};
    for (int t = 0; t < 5000; t++) {
        vec128_t a = {.u64 = {0, 0}}, b = {.u64 = {0, 0}},
                 want = {.u64 = {0, 0}};
        for (int i = 0; i < 16; i++) {
            a.u8[i] = (unsigned char)asmtest_rng_range(&rng, 0, 255);
            b.u8[i] = (unsigned char)asmtest_rng_range(&rng, 0, 255);
            unsigned s = (unsigned)a.u8[i] + b.u8[i];
            want.u8[i] =
                s > 0xFF ? 0xFF : (unsigned char)s; /* C model: clamp */
        }
        regs_t r;
        ASM_VCALL2(&r, qadd_u8x16, a, b);
        ASSERT_VEC_EQ(&r, 0,
                      want.u8); /* all 16 lanes, saturation edge included */
    }
}
