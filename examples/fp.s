/*
 * fp.s — example floating-point routines (portable x86-64 / AArch64).
 *
 * double fp_add(double a, double b);  a + b
 * double fp_mul(double a, double b);  a * b
 *   x86-64: a -> %xmm0, b -> %xmm1, result -> %xmm0
 *   AArch64: a -> d0,   b -> d1,    result -> d0
 * double int_to_double(long n);       (double)n  — an integer arg into an XMM
 *   result, so an emulator run reachable via integer args still exercises the
 *   guest XMM file (x86-64: n -> %rdi, result -> %xmm0; AArch64: x0 -> d0).
 */
#include "asm.h"

ASM_FUNC fp_add
#if defined(__x86_64__)
    addsd   %xmm1, %xmm0        /* xmm0 = xmm0 + xmm1 */
    ret
#elif defined(__aarch64__)
    fadd    d0, d0, d1
    ret
#endif
ASM_ENDFUNC fp_add

ASM_FUNC fp_mul
#if defined(__x86_64__)
    mulsd   %xmm1, %xmm0        /* xmm0 = xmm0 * xmm1 */
    ret
#elif defined(__aarch64__)
    fmul    d0, d0, d1
    ret
#endif
ASM_ENDFUNC fp_mul

ASM_FUNC int_to_double
#if defined(__x86_64__)
    cvtsi2sd %rdi, %xmm0        /* xmm0 = (double)rdi */
    ret
#elif defined(__aarch64__)
    scvtf   d0, x0
    ret
#endif
ASM_ENDFUNC int_to_double
