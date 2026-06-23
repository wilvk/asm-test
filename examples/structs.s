/*
 * structs.s — example struct-returning routines (portable x86-64 / AArch64).
 *
 *   struct big { long a, b, c; } make_big(long a, long b, long c);
 *     24 bytes > 16, so returned via the hidden result pointer (rdi / x8).
 *   struct pair { long a, b; } make_pair(long a, long b);
 *     16 bytes, returned in registers (rax:rdx / x0:x1).
 */
#include "asm.h"

ASM_FUNC make_big
#if defined(__x86_64__)
    /* rdi = result ptr; a=rsi, b=rdx, c=rcx */
    movq    %rsi, 0(%rdi)
    movq    %rdx, 8(%rdi)
    movq    %rcx, 16(%rdi)
    movq    %rdi, %rax          /* return the result pointer */
    ret
#elif defined(__aarch64__)
    /* x8 = result ptr; a=x0, b=x1, c=x2 */
    str     x0, [x8, #0]
    str     x1, [x8, #8]
    str     x2, [x8, #16]
    mov     x0, x8             /* return the result pointer */
    ret
#endif
ASM_ENDFUNC make_big

ASM_FUNC make_pair
#if defined(__x86_64__)
    /* a=rdi, b=rsi -> struct returned in rax:rdx */
    movq    %rdi, %rax
    movq    %rsi, %rdx
    ret
#elif defined(__aarch64__)
    /* a=x0, b=x1 -> struct returned in x0:x1 (already in place) */
    ret
#endif
ASM_ENDFUNC make_pair
