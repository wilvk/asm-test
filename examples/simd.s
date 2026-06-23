/*
 * simd.s — example SIMD routine (portable x86-64 SSE / AArch64 NEON).
 *
 * vec128 vec_add4f(vec128 a, vec128 b);  lane-wise add of four 32-bit floats
 *   x86-64: a -> %xmm0, b -> %xmm1, result -> %xmm0
 *   AArch64: a -> v0,   b -> v1,    result -> v0
 */
#include "asm.h"

ASM_FUNC vec_add4f
#if defined(__x86_64__)
    addps   %xmm1, %xmm0        /* xmm0 = xmm0 + xmm1, packed single */
    ret
#elif defined(__aarch64__)
    fadd    v0.4s, v0.4s, v1.4s
    ret
#endif
ASM_ENDFUNC vec_add4f
