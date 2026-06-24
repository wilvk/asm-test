/*
 * test_failure_demo.c — intentionally failing tests that show the framework's
 * failure reporting. Built and run via `make demo-fail`; not part of the green
 * `make test` build.
 */
#include "asmtest.h"

extern long sum_via_rbx(long a, long b);
extern long clobbers_rbx(long a, long b);
extern double fp_add(double a, double b);
extern long imax_wrong(long a, long b); /* returns the minimum (a bug) */

static long ref_imax(long a, long b) { return a > b ? a : b; }
static int gen_pair(asmtest_rng_t *rng, long *a, int cap) {
    (void)cap;
    a[0] = asmtest_rng_range(rng, -1000, 1000);
    a[1] = asmtest_rng_range(rng, -1000, 1000);
    return 2;
}

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

TEST(demo, fp_exact_mismatch) {
    /* 0.1 + 0.2 is not exactly 0.3; ASSERT_FP_EQ reports the difference. */
    regs_t r;
    ASM_FCALL2(&r, fp_add, 0.1, 0.2);
    ASSERT_FP_EQ(&r, 0.3);
}

TEST(demo, ref_mismatch_reports_input) {
    /* Differential testing finds the first random input where the routine and
     * its C model disagree, and reports that input (and the seed to replay). */
    ASSERT_MATCHES_REF2(imax_wrong, ref_imax, gen_pair, 10000);
}
