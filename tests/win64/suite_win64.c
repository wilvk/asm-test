/* suite_win64.c — a real test suite driven by the framework runner, built for
 * the native Win64 tier (Phase 4 integration). Registered via the standard
 * TEST() macro and run by asmtest.c's main() under Wine, exercising the Win64
 * capture trampoline plus the runner's in-process crash/timeout containment.
 *
 * Passing tests prove discovery + run + assert + the captured Win64 ABI; the
 * crash and hang tests are *expected* to fail — they prove the runner's
 * facility turns a fault/timeout into a reported failure while surviving.
 */
#include "asmtest.h"

extern void win64_ret_arg0(void);
extern void win64_sum2(void);
extern void win64_clobber_rbx(void);
extern void win64_vec_preserve_xmm6(void);

#define WCHECK(cond, ...)                                                      \
    do {                                                                       \
        if (!(cond))                                                           \
            asmtest_fail(__FILE__, __LINE__, __VA_ARGS__);                     \
    } while (0)

TEST(win64, ret_arg0) {
    regs_t r;
    ASM_CALL_WIN64_1(&r, win64_ret_arg0, 111);
    WCHECK(r.ret == 111, "ret = %llu, want 111", (unsigned long long)r.ret);
}

TEST(win64, sum2) {
    regs_t r;
    ASM_CALL_WIN64_2(&r, win64_sum2, 10, 20);
    WCHECK(r.ret == 30, "ret = %llu, want 30", (unsigned long long)r.ret);
}

TEST(win64, abi_preserved) {
    regs_t r;
    ASM_CALL_WIN64_1(&r, win64_ret_arg0, 111);
    /* The framework's ABI-preservation assertion, now Win64-aware: it covers the
     * full Win64 integer callee-saved set (rbx, rbp, rdi, rsi, r12-r15). */
    ASSERT_ABI_PRESERVED(&r);
}

TEST(win64, detects_clobber) {
    regs_t r;
    ASM_CALL_WIN64_1(&r, win64_clobber_rbx, 111);
    WCHECK(r.rbx != ASMTEST_SENTINEL_RBX,
           "expected a clobbered rbx to be observable");
}

TEST(win64, fp_preserved) {
    regs_t r;
    const long long iargs[6] = {0};
    vec128_t vargs[4];
    for (int i = 0; i < 4; i++) {
        vargs[i].u64[0] = 0;
        vargs[i].u64[1] = 0;
    }
    vargs[0].f64[0] = 2.5;
    asm_call_capture_vec_win64(&r, (void *)win64_vec_preserve_xmm6, iargs, vargs);
    WCHECK(r.fret == 2.5, "fret = %g, want 2.5", r.fret);
    /* The framework's Win64 vector ABI-preservation assertion (xmm6-15). */
    ASSERT_ABI_PRESERVED_VEC(&r);
}

/* Expected failures: prove the runner contains a crash / a hang as a reported
 * failure rather than dying or wedging. */
TEST(win64, crash_contained) {
    volatile int *p = (volatile int *)0;
    *p = 1; /* access violation -> caught by the runner's vectored handler */
}

TEST(win64, hang_timed_out) {
    for (;;) {
    } /* infinite loop -> caught by the runner's per-test watchdog timeout */
}
