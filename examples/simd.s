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
#elif defined(__riscv) && __riscv_xlen == 64
    /* rv64gc has no 128-bit vector registers; a stub so the symbol resolves.
     * Never called: asmtest_cpu_has_vec128() is false on rv64, so test_simd's
     * ASM_VCALL macros self-skip. */
    ret
#endif
ASM_ENDFUNC vec_add4f

#if defined(__x86_64__)
/*
 * vec256 vec_add4d(vec256 a, vec256 b);  lane-wise add of four 64-bit doubles
 * (AVX2): a -> %ymm0, b -> %ymm1, result -> %ymm0. x86-64 only — AVX is x86's;
 * the native vec256 capture path (asm_call_capture_vec256) is AVX2-gated, and
 * the matching test self-skips where AVX2 is absent.
 */
ASM_FUNC vec_add4d
    vaddpd  %ymm1, %ymm0, %ymm0 /* ymm0 = ymm0 + ymm1, packed double */
    ret
ASM_ENDFUNC vec_add4d

/*
 * vec512 vec_add8d(vec512 a, vec512 b);  lane-wise add of EIGHT 64-bit doubles
 * (AVX-512): a -> %zmm0, b -> %zmm1, result -> %zmm0. x86-64 only — AVX-512 is x86's;
 * the native vec512 capture path (asm_call_capture_vec512) is AVX-512F-gated, and
 * the matching test self-skips where AVX-512 is absent.
 */
ASM_FUNC vec_add8d
    vaddpd  %zmm1, %zmm0, %zmm0 /* zmm0 = zmm0 + zmm1, packed double (8 lanes) */
    ret
ASM_ENDFUNC vec_add8d
#endif

#if defined(__aarch64__) && defined(__linux__)
/*
 * svec sve_addd(svec a, svec b);  lane-wise add of VL/8 64-bit doubles
 * (SVE): a -> z0, b -> z1, result -> z0. AArch64 Linux only; the capture
 * path is HWCAP_SVE-gated and the test self-skips where SVE is absent.
 * p3 is the scratch governing predicate: p0-p3 are argument/caller-saved
 * registers, while p4-p15 are callee-saved HERE because this routine takes
 * SVE arguments (AAPCS64) — so only p0-p3 may be clobbered. `.arch` last in
 * the file, mirroring the capture.s end-of-file rule (nothing SVE below).
 */
    .arch   armv8-a+sve
ASM_FUNC sve_addd
    ptrue   p3.d
    fadd    z0.d, p3/m, z0.d, z1.d
    ret
ASM_ENDFUNC sve_addd
#endif
