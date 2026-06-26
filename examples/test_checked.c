/*
 * test_checked.c — runtime / compiler-rt primitive: a checked add whose whole
 * job is the overflow *flag*, not just the sum. The return value alone can't
 * tell a caller that a+b wrapped; ASM_CALL captures the flags through the real
 * call so the overflow signal is assertable. This is the test a
 * __builtin_add_overflow / __addvdi3 lowering needs.
 */
#include "asmtest.h"

#include <limits.h>

extern long checked_add(long a, long b);

TEST(runtime, ordinary_add_clears_overflow) {
    regs_t r;
    ASM_CALL2(&r, checked_add, 2, 3);
    ASSERT_EQ(r.ret, 5);
    ASSERT_FLAG_CLEAR(&r, OF);
}

TEST(runtime, negative_result_no_overflow) {
    regs_t r;
    ASM_CALL2(&r, checked_add, -5, 2);
    ASSERT_EQ(r.ret, -3);
    ASSERT_FLAG_CLEAR(&r, OF);
}

TEST(runtime, positive_overflow_sets_OF) {
    regs_t r;
    ASM_CALL2(&r, checked_add, LONG_MAX, 1); /* wraps to LONG_MIN */
    ASSERT_FLAG_SET(&r, OF);
}

TEST(runtime, negative_overflow_sets_OF) {
    regs_t r;
    ASM_CALL2(&r, checked_add, LONG_MIN, -1); /* wraps to LONG_MAX */
    ASSERT_FLAG_SET(&r, OF);
}
