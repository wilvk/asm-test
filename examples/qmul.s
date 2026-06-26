/*
 * qmul.s — DSP / fixed-point example (portable x86-64 / AArch64). A Q15
 * fixed-point multiply with round-to-nearest is the multiply-accumulate at the
 * heart of an FIR filter; the rounding bias is exactly where these go wrong, so
 * it makes a clean differential-testing target against a plain-C model.
 *
 * long qmul_q15(long a, long b);  (a*b + 0x4000) >> 15, for inputs in
 *   [-32768, 32767] (the product fits in 31 bits, so the low 64 bits suffice).
 *   x86-64: a -> %rdi, b -> %rsi, result -> %rax
 *   AArch64: a -> x0,  b -> x1,   result -> x0
 */
#include "asm.h"

ASM_FUNC qmul_q15
#if defined(__x86_64__)
    movq    %rdi, %rax
    imulq   %rsi, %rax          /* a * b */
    addq    $0x4000, %rax       /* + 0.5 ulp for round-to-nearest */
    sarq    $15, %rax           /* >> 15, arithmetic (signed) */
    ret
#elif defined(__aarch64__)
    mul     x0, x0, x1
    add     x0, x0, #0x4000     /* 0x4000 == 4 << 12, an encodable add imm */
    asr     x0, x0, #15
    ret
#endif
ASM_ENDFUNC qmul_q15
