/*
 * test_capture.c — exercises register/flags capture, ABI-preservation checks,
 * and guard-page buffers against the routines in flags.s.
 */
#include "asmtest.h"

#if !defined(ASMTEST_NO_FLAGS)
extern long set_carry(void);
extern long clear_carry(void);
#endif
extern long sum_via_rbx(long a, long b);

TEST(capture, return_value_captured) {
    regs_t r;
    ASM_CALL2(&r, sum_via_rbx, 20, 22);
    ASSERT_EQ(r.ret, 42);
}

TEST(capture, callee_saved_preserved) {
    regs_t r;
    ASM_CALL2(&r, sum_via_rbx, 1, 2);
    ASSERT_EQ(r.ret, 3);
    ASSERT_ABI_PRESERVED(&r); /* sum_via_rbx saves/restores its scratch reg */
}

#if !defined(ASMTEST_NO_FLAGS)
TEST(capture, carry_flag_set) {
    regs_t r;
    ASM_CALL0(&r, set_carry);
    ASSERT_FLAG_SET(&r, CF);
}

TEST(capture, carry_flag_clear) {
    regs_t r;
    ASM_CALL0(&r, clear_carry);
    ASSERT_FLAG_CLEAR(&r, CF);
}
#else
/* rv64 has no condition-flags register, so ASSERT_FLAG_* is a compile error by
 * design (no ASMTEST_CF macro) and set_carry/clear_carry have no analog. A named
 * SKIP keeps the ISA fact visible in the TAP stream rather than silently
 * dropping the cases. */
TEST(capture, flags_unavailable_rv64) {
    SKIP("RISC-V has no condition-flags register (ASMTEST_NO_FLAGS)");
}
#endif

TEST(guard, in_bounds_write_ok) {
    unsigned char *p = asmtest_guarded_alloc(8);
    ASSERT_TRUE(p != NULL);
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)i;
    ASSERT_EQ(p[7], 7);
    asmtest_guarded_free(p, 8);
}
