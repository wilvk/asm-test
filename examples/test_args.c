/*
 * test_args.c — arbitrary-count integer arguments (Phase 6, stack passing).
 * ASM_CALLN places the first 6/8 args in registers and the rest on the stack.
 */
#include "asmtest.h"

extern long sum3(long, long, long);
extern long sum8(long, long, long, long, long, long, long, long);
extern long sum10(long, long, long, long, long, long, long, long, long, long);

TEST(args, three_args_register_only) {
    regs_t r;
    ASM_CALLN(&r, sum3, 40, 1, 1);
    ASSERT_EQ(r.ret, 42);
}

TEST(args, eight_args) {
    regs_t r;
    ASM_CALLN(&r, sum8, 1, 2, 3, 4, 5, 6, 7, 8);
    ASSERT_EQ(r.ret, 36);
}

TEST(args, ten_args_with_stack) {
    regs_t r;
    ASM_CALLN(&r, sum10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    ASSERT_EQ(r.ret, 55);
}

TEST(args, distinguishes_argument_positions) {
    /* Powers of two make a wrong slot/order obvious in the sum. */
    regs_t r;
    ASM_CALLN(&r, sum10, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512);
    ASSERT_EQ(r.ret, 1023);
}

TEST(args, abi_preserved_with_stack_args) {
    regs_t r;
    ASM_CALLN(&r, sum10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    ASSERT_EQ(r.ret, 55);
    ASSERT_ABI_PRESERVED(&r); /* rbx, r12-r15 (and x19-x28 on AArch64) */
}
