/*
 * qadd.s — codec / image kernel: per-byte unsigned saturating add (portable
 * x86-64 SSE2 / AArch64 NEON). The pixel-clamp at the heart of an image blend or
 * video filter, where 0xFF + anything must stay 0xFF rather than wrap. The
 * saturation edge is the bug magnet, so test_qadd.c fuzzes random 16-byte
 * vectors lane-wise against a clamping C model.
 *
 * vec128 qadd_u8x16(vec128 a, vec128 b);  16 lanes of uint8 saturating add
 *   x86-64: a -> %xmm0, b -> %xmm1, result -> %xmm0
 *   AArch64: a -> v0,   b -> v1,    result -> v0
 */
#include "asm.h"

ASM_FUNC qadd_u8x16
#if defined(__x86_64__)
    paddusb %xmm1, %xmm0        /* packed add unsigned saturate, bytes */
    ret
#elif defined(__aarch64__)
    uqadd   v0.16b, v0.16b, v1.16b
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    /* rv64gc has no 128-bit vector registers; a stub so the symbol resolves.
     * Never called: asmtest_cpu_has_vec128() is false on rv64, so test_qadd's
     * ASM_VCALL2 self-skips. */
    ret
#endif
ASM_ENDFUNC qadd_u8x16
