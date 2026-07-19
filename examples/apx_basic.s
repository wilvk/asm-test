/*
 * apx_basic.s — Intel APX (Advanced Performance Extensions) example routines.
 *
 * Exercises the extended GP registers r16-r31 (reached via REX2 / EVEX encodings)
 * and the new-data-destination (NDD) 3-operand form. No non-Intel silicon ships
 * APX yet, so these bodies #UD on today's hardware — the matching suite
 * (test_apx_basic.c) gates every case on asmtest_cpu_has_apx() and only runs them
 * under `sde64 -future`, whose emulated CPUID reports APX_F. Needs an APX-capable
 * assembler (GAS with EGPR/NDD support, i.e. binutils >= 2.42/2.43); the pinned
 * Dockerfile.sde uses binutils 2.46.1. x86-64 only.
 */
#include "asm.h"

/* long apx_sum4(long a, long b, long c, long d);  EGPR/REX2 exercise:
 * (a+b) + (c+d), routed entirely through r16-r19. */
ASM_FUNC apx_sum4
#if defined(__x86_64__)
    movq    %rdi, %r16          /* REX2-encoded: EGPR destinations   */
    movq    %rsi, %r17
    movq    %rdx, %r18
    movq    %rcx, %r19
    addq    %r17, %r16
    addq    %r19, %r18
    leaq    (%r16,%r18), %rax   /* EGPR base+index through REX2 lea  */
    ret
#endif
ASM_ENDFUNC apx_sum4

/* long apx_ndd_add(long a, long b);  new-data-destination form:
 * rax = rdi + rsi with rdi/rsi left unmodified (needs GAS with NDD support). */
ASM_FUNC apx_ndd_add
#if defined(__x86_64__)
    addq    %rsi, %rdi, %rax    /* APX NDD: rax = rdi + rsi */
    ret
#endif
ASM_ENDFUNC apx_ndd_add

/* long apx_egpr_carry(long a, long b);  an EGPR add whose result carries out of
 * bit 63 sets CF, so the captured flags prove SDE models APX flag effects. */
ASM_FUNC apx_egpr_carry
#if defined(__x86_64__)
    movq    %rdi, %r16
    movq    %rsi, %r17
    addq    %r17, %r16          /* r16 = a + b; CF = unsigned overflow */
    movq    %r16, %rax          /* mov is flag-neutral: CF survives to ret */
    ret
#endif
ASM_ENDFUNC apx_egpr_carry
