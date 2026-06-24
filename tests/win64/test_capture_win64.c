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

#include "win64_regs.h"

#if defined(_WIN32)
#  define WIN64ABI /* mingw target is already Microsoft x64 */
#else
#  define WIN64ABI __attribute__((ms_abi))
#endif

extern WIN64ABI void asm_call_capture_win64(win64_regs_t *out, void *fn,
                                            const long long *args);

/* Routines under test — addresses only; never called through these C types. */
extern void win64_ret_arg0(void);
extern void win64_sum2(void);
extern void win64_preserve_rbx(void);
extern void win64_clobber_rbx(void);
extern void win64_ret_arg4(void);

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

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
