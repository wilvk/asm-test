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

extern void asm_call_capture_win64(regs_t *out, void *fn, const long long *args);
extern void asm_call_capture_vec_win64(regs_t *out, void *fn,
                                       const long long *iargs,
                                       const vec128_t *vargs);

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
    const long long args[6] = {111, 222, 333, 444, 555, 666};
    asm_call_capture_win64(&r, (void *)win64_ret_arg0, args);
    WCHECK(r.ret == 111, "ret = %llu, want 111", (unsigned long long)r.ret);
}

TEST(win64, sum2) {
    regs_t r;
    const long long args[6] = {10, 20, 0, 0, 0, 0};
    asm_call_capture_win64(&r, (void *)win64_sum2, args);
    WCHECK(r.ret == 30, "ret = %llu, want 30", (unsigned long long)r.ret);
}

TEST(win64, abi_preserved) {
    regs_t r;
    const long long args[6] = {111, 0, 0, 0, 0, 0};
    asm_call_capture_win64(&r, (void *)win64_ret_arg0, args);
    WCHECK(r.rbx == ASMTEST_SENTINEL_RBX && r.rbp == ASMTEST_SENTINEL_RBP &&
               r.rdi == ASMTEST_SENTINEL_RDI && r.rsi == ASMTEST_SENTINEL_RSI &&
               r.r15 == ASMTEST_SENTINEL_R15,
           "win64_ret_arg0 did not preserve the Win64 callee-saved set");
}

TEST(win64, detects_clobber) {
    regs_t r;
    const long long args[6] = {111, 0, 0, 0, 0, 0};
    asm_call_capture_win64(&r, (void *)win64_clobber_rbx, args);
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
    for (int i = 6; i <= 15; i++)
        WCHECK(r.vec[i].u64[0] == (unsigned long long)i,
               "xmm%d not preserved across the call", i);
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
