/*
 * branch.s — a branchy routine under test, for the emulator coverage demo
 * (portable x86-64 / AArch64).
 *
 * long classify(long x);
 *   returns -1 if x < 0, 0 if x == 0, +1 if x > 0.
 *
 * Three return paths through distinct basic blocks: no single input reaches
 * them all, so the UNION of blocks across several inputs is what answers "did
 * the tests exercise every branch?" — see test_emu.c.
 */
#include "asm.h"

ASM_FUNC classify
#if defined(__x86_64__)
    testq   %rdi, %rdi
    js      1f                  /* x < 0  */
    jz      2f                  /* x == 0 */
    movq    $1, %rax            /* x > 0  */
    ret
1:
    movq    $-1, %rax
    ret
2:
    xorq    %rax, %rax
    ret
#elif defined(__aarch64__)
    cmp     x0, #0
    b.lt    1f                  /* x < 0  */
    b.eq    2f                  /* x == 0 */
    mov     x0, #1              /* x > 0  */
    ret
1:
    mov     x0, #-1
    ret
2:
    mov     x0, #0
    ret
#endif
ASM_ENDFUNC classify
