/*
 * test_capture.c — exercises register/flags capture, ABI-preservation checks,
 * and guard-page buffers against the routines in flags.s.
 */
#include "asmtest.h"

#include <string.h>

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

#if defined(__riscv) && __riscv_xlen == 64
/* T3: fs0-fs11 (FP callee-saved) preservation, proven through a REAL _fp capture
 * (rv64 has no _vec path, so ASSERT_ABI_PRESERVED_VEC rides asm_call_capture_fp,
 * which seeds/captures fs0-fs11). preserves_fs saves/restores fs2 (compliant);
 * clobbers_fs2 trashes it. */
extern double preserves_fs(double x);
extern double clobbers_fs2(double x);
TEST(capture, fp_callee_saved_preserved_rv64) {
    regs_t r;
    ASM_FCALL1(&r, preserves_fs, 3.0);
    ASSERT_FP_EQ(&r, 6.0);        /* returns 2*x */
    ASSERT_ABI_PRESERVED_VEC(&r); /* fs0-fs11 all restored -> passes */
}
TEST(capture, fp_callee_saved_violation_detected_rv64) {
    regs_t r;
    char msg[128];
    ASM_FCALL1(&r, clobbers_fs2, 3.0);
    ASSERT_FP_EQ(&r, 6.0);
    /* The non-jumping verdict shim flags the clobber and names fs2. */
    ASSERT_TRUE(asmtest_check_abi_vec(&r, msg, sizeof msg) != 0);
    ASSERT_TRUE(strstr(msg, "fs2 not restored") != NULL);
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
