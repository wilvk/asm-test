/*
 * test_failure_demo.c — intentionally failing tests that show the framework's
 * failure reporting. Built and run via `make demo-fail`; not part of the green
 * `make test` build.
 */
#include "asmtest.h"

extern long sum_via_rbx(long a, long b);
extern long clobbers_rbx(long a, long b);

TEST(demo, compliant_routine_passes) {
    regs_t r;
    ASM_CALL2(&r, sum_via_rbx, 3, 4);
    ASSERT_EQ(r.ret, 7);
    ASSERT_ABI_PRESERVED(&r);
}

TEST(demo, abi_violation_detected) {
    /* clobbers_rbx computes the right answer but trashes a callee-saved reg. */
    regs_t r;
    ASM_CALL2(&r, clobbers_rbx, 3, 4);
    ASSERT_EQ(r.ret, 7);
    ASSERT_ABI_PRESERVED(&r); /* fails: callee-saved register not restored */
}

TEST(demo, buffer_overrun_caught) {
    /* Writing one past the end hits the guard page -> SIGSEGV -> failure,
     * instead of silently corrupting memory or crashing the runner. */
    unsigned char *p = asmtest_guarded_alloc(8);
    p[8] = 0x41;
    asmtest_guarded_free(p, 8);
}

TEST(demo, buffer_underrun_caught) {
    /* A leading guard page catches one-before-the-start (underrun) writes. */
    unsigned char *p = asmtest_guarded_alloc_under(8);
    p[-1] = 0x41;
    asmtest_guarded_free_under(p, 8);
}

TEST(demo, mem_diff_shows_hexdump) {
    /* ASSERT_MEM_EQ reports a hexdump window of expected vs actual. */
    unsigned char actual[16], expect[16];
    for (int i = 0; i < 16; i++)
        actual[i] = expect[i] = (unsigned char)i;
    actual[5] = 0xff; /* introduce a single-byte difference */
    ASSERT_MEM_EQ(actual, expect, 16);
}
