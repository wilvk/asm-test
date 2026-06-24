/*
 * refmatch.s — routines for differential / property testing (Phase 7). Each has
 * a simple C reference model (see test_refmatch.c); the framework fuzzes inputs
 * and asserts the assembly matches the model. Branchy/conditional code is where
 * subtle bugs hide, so these use cmov / csel rather than straight-line math.
 *
 *   long imax(long a, long b);            signed maximum
 *   long iabs(long x);                    signed absolute value (branchless)
 *   long iclamp(long x, long lo, long hi);clamp x to [lo, hi]
 *   long imax_wrong(long a, long b);      deliberately returns the *minimum*
 *                                         (used by the failure demo)
 */
#include "asm.h"

ASM_FUNC imax
#if defined(__x86_64__)
    movq    %rdi, %rax
    cmpq    %rsi, %rdi
    cmovlq  %rsi, %rax          /* if a < b, result = b */
    ret
#elif defined(__aarch64__)
    cmp     x0, x1
    csel    x0, x0, x1, gt      /* x0 = (a > b) ? a : b */
    ret
#endif
ASM_ENDFUNC imax

ASM_FUNC iabs
#if defined(__x86_64__)
    movq    %rdi, %rax
    movq    %rdi, %rdx
    sarq    $63, %rdx           /* rdx = (x < 0) ? -1 : 0 (sign mask) */
    xorq    %rdx, %rax
    subq    %rdx, %rax          /* (x ^ mask) - mask == |x| */
    ret
#elif defined(__aarch64__)
    cmp     x0, #0
    cneg    x0, x0, lt          /* x0 = (x < 0) ? -x : x */
    ret
#endif
ASM_ENDFUNC iabs

ASM_FUNC iclamp
#if defined(__x86_64__)
    movq    %rdi, %rax
    cmpq    %rsi, %rax
    cmovlq  %rsi, %rax          /* rax = max(x, lo) */
    cmpq    %rdx, %rax
    cmovgq  %rdx, %rax          /* rax = min(rax, hi) */
    ret
#elif defined(__aarch64__)
    cmp     x0, x1
    csel    x0, x0, x1, gt      /* x0 = max(x, lo) */
    cmp     x0, x2
    csel    x0, x0, x2, lt      /* x0 = min(x0, hi) */
    ret
#endif
ASM_ENDFUNC iclamp

ASM_FUNC imax_wrong
#if defined(__x86_64__)
    movq    %rdi, %rax
    cmpq    %rsi, %rdi
    cmovgq  %rsi, %rax          /* BUG: keeps the smaller operand */
    ret
#elif defined(__aarch64__)
    cmp     x0, x1
    csel    x0, x0, x1, lt      /* BUG: keeps the smaller operand */
    ret
#endif
ASM_ENDFUNC imax_wrong
