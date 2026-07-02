/*
 * structparam.s — example routines taking structs by value (x86-64 / AArch64).
 *
 *   long pst2(struct pair p);            small all-int struct: 2 INTEGER
 *                                        eightbytes -> rdi,rsi / x0,x1
 *   long pst_mixed(struct mixed m);      {long; double}: SysV classifies the two
 *                                        eightbytes INTEGER + SSE -> rdi + xmm0;
 *                                        AAPCS64 passes the whole non-HFA
 *                                        composite in GP regs -> x0 + x1 (b's
 *                                        bit pattern in x1, NOT d0; only HFAs
 *                                        reach SIMD regs, rule C.2/C.12)
 *   long bigsum(struct big s);           24 bytes (memory class): x86-64 reads
 *                                        it from the stack, AArch64 via pointer
 */
#include "asm.h"

ASM_FUNC pst2
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    ret
#endif
ASM_ENDFUNC pst2

ASM_FUNC pst_mixed
#if defined(__x86_64__)
    cvttsd2si %xmm0, %rax       /* (long)m.b */
    addq    %rdi, %rax          /* + m.a     */
    ret
#elif defined(__aarch64__)
    /* AAPCS64: {long; double} is a non-HFA composite, so both eightbytes go in
     * GP regs — m.a in x0, m.b's bit pattern in x1 (d0 holds nothing). Move the
     * bits into an FP reg to convert to integer. */
    fmov    d0, x1
    fcvtzs  x1, d0
    add     x0, x0, x1
    ret
#endif
ASM_ENDFUNC pst_mixed

ASM_FUNC bigsum
#if defined(__x86_64__)
    movq    8(%rsp), %rax       /* s.a (struct copied onto the stack) */
    addq    16(%rsp), %rax      /* s.b */
    addq    24(%rsp), %rax      /* s.c */
    ret
#elif defined(__aarch64__)
    ldr     x1, [x0, #0]        /* s passed by pointer in x0 */
    ldr     x2, [x0, #8]
    ldr     x3, [x0, #16]
    add     x0, x1, x2
    add     x0, x0, x3
    ret
#endif
ASM_ENDFUNC bigsum
