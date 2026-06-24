/* test_capture_win64.c — Phase 1 driver for the native Win64 capture trampoline.
 *
 * Calls asm_call_capture_win64 (src/capture_win64.asm) on a handful of Win64-ABI
 * routines and checks the captured return value, stack-arg/shadow-space handling,
 * and ABI preservation over the Win64 callee-saved set.
 *
 * Two build lanes, same source:
 *   - ms_abi native lane (Linux/macOS x86-64): the host is System V, so the
 *     trampoline is declared __attribute__((ms_abi)) to be called the Win64 way.
 *   - PE/Wine lane (mingw-w64): the target is already Win64, so no attribute.
 */
#include <stdio.h>
#include <string.h>

#include "win64_regs.h"

#if defined(_WIN32)
#  define WIN64ABI /* mingw target is already Microsoft x64 */
#else
#  define WIN64ABI __attribute__((ms_abi))
#endif

extern WIN64ABI void asm_call_capture_win64(win64_regs_t *out, void *fn,
                                            const long long *args);
extern WIN64ABI void asm_call_capture_args_win64(win64_regs_t *out, void *fn,
                                                 const long long *args,
                                                 int nargs);
extern WIN64ABI void asm_call_capture_vec_win64(win64_regs_t *out, void *fn,
                                                const long long *iargs,
                                                const win64_vec128_t *vargs);

/* Routines under test — addresses only; never called through these C types. */
extern void win64_ret_arg0(void);
extern void win64_sum2(void);
extern void win64_preserve_rbx(void);
extern void win64_clobber_rbx(void);
extern void win64_ret_arg4(void);
extern void win64_sum6(void);
extern void win64_addsd(void);
extern void win64_vec_preserve_xmm6(void);
extern void win64_vec_clobber_xmm6(void);

static int fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("ok   - %s\n", (msg));                                      \
        } else {                                                               \
            printf("FAIL - %s\n", (msg));                                      \
            fails++;                                                           \
        }                                                                      \
    } while (0)

static int abi_preserved(const win64_regs_t *r) {
    return r->rbx == WIN64_SENTINEL_RBX && r->rbp == WIN64_SENTINEL_RBP &&
           r->rdi == WIN64_SENTINEL_RDI && r->rsi == WIN64_SENTINEL_RSI &&
           r->r12 == WIN64_SENTINEL_R12 && r->r13 == WIN64_SENTINEL_R13 &&
           r->r14 == WIN64_SENTINEL_R14 && r->r15 == WIN64_SENTINEL_R15;
}

/* xmm6..15 preservation: each seeded with lanes both = its register index. */
static int xmm_callee_preserved(const win64_regs_t *r) {
    for (int i = WIN64_XMM_CALLEE_LO; i <= WIN64_XMM_CALLEE_HI; i++) {
        if (r->vec[i].lo != (uint64_t)i || r->vec[i].hi != (uint64_t)i)
            return 0;
    }
    return 1;
}

static win64_vec128_t lane_from_double(double d) {
    win64_vec128_t v;
    v.lo = 0;
    v.hi = 0;
    memcpy(&v.lo, &d, sizeof d);
    return v;
}

int main(void) {
    win64_regs_t r;
    const long long args[6] = {111, 222, 333, 444, 555, 666};

    asm_call_capture_win64(&r, (void *)win64_ret_arg0, args);
    CHECK(r.ret == 111, "win64_ret_arg0 returns the 1st int arg (rcx)");
    CHECK(abi_preserved(&r), "win64_ret_arg0 preserves the callee-saved set");

    asm_call_capture_win64(&r, (void *)win64_sum2, args);
    CHECK(r.ret == 333, "win64_sum2 returns rcx+rdx (111+222)");

    asm_call_capture_win64(&r, (void *)win64_ret_arg4, args);
    CHECK(r.ret == 555, "win64_ret_arg4 returns the 5th arg (stack + shadow)");

    asm_call_capture_win64(&r, (void *)win64_preserve_rbx, args);
    CHECK(r.ret == 111 && abi_preserved(&r),
          "win64_preserve_rbx: correct result and callee-saved intact");

    asm_call_capture_win64(&r, (void *)win64_clobber_rbx, args);
    CHECK(r.ret == 111, "win64_clobber_rbx still returns rcx");
    CHECK(!abi_preserved(&r) && r.rbx != WIN64_SENTINEL_RBX,
          "win64_clobber_rbx: ABI violation detected (rbx not preserved)");

    /* asm_call_capture_args_win64: arbitrary integer arity (register + stack). */
    asm_call_capture_args_win64(&r, (void *)win64_sum6, args, 6);
    CHECK(r.ret == 111 + 222 + 333 + 444 + 555 + 666,
          "win64_sum6 via _args: 4 register + 2 stack args summed");

    asm_call_capture_args_win64(&r, (void *)win64_sum2, args, 2);
    CHECK(r.ret == 333, "win64_sum2 via _args with nargs=2");

    /* asm_call_capture_vec_win64: FP return + full vector file + xmm6-15. */
    win64_vec128_t vargs[4];
    vargs[0] = lane_from_double(1.5);
    vargs[1] = lane_from_double(2.25);
    vargs[2] = lane_from_double(0.0);
    vargs[3] = lane_from_double(0.0);

    asm_call_capture_vec_win64(&r, (void *)win64_addsd, args, vargs);
    CHECK(r.fret == 3.75, "win64_addsd via _vec: xmm0+xmm1 FP return captured");
    CHECK(xmm_callee_preserved(&r),
          "win64_addsd preserves the xmm6-15 callee-saved set");

    asm_call_capture_vec_win64(&r, (void *)win64_vec_preserve_xmm6, args, vargs);
    CHECK(r.fret == 1.5 && xmm_callee_preserved(&r),
          "win64_vec_preserve_xmm6: correct FP result and xmm6-15 intact");

    asm_call_capture_vec_win64(&r, (void *)win64_vec_clobber_xmm6, args, vargs);
    CHECK(!xmm_callee_preserved(&r) && r.vec[6].lo == 0 && r.vec[6].hi == 0,
          "win64_vec_clobber_xmm6: FP ABI violation detected (xmm6 not preserved)");
    CHECK(r.vec[7].lo == 7 && r.vec[15].lo == 15,
          "win64_vec_clobber_xmm6: the other xmm sentinels survive");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
