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
    ASSERT_EQ(r.rax, 7);
    ASSERT_ABI_PRESERVED(&r);
}

TEST(demo, abi_violation_detected) {
    /* clobbers_rbx computes the right answer but trashes a callee-saved reg. */
    regs_t r;
    ASM_CALL2(&r, clobbers_rbx, 3, 4);
    ASSERT_EQ(r.rax, 7);
    ASSERT_ABI_PRESERVED(&r); /* fails: rbx not restored */
}

TEST(demo, buffer_overrun_caught) {
    /* Writing one past the end hits the guard page -> SIGSEGV -> failure,
     * instead of silently corrupting memory or crashing the runner. */
    unsigned char *p = asmtest_guarded_alloc(8);
    p[8] = 0x41;
    asmtest_guarded_free(p, 8);
}
