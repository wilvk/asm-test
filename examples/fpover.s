/*
 * fpover.s — routines with more FP/vector args than there are argument
 * registers, so the 9th+ argument is passed on the stack (portable x86-64 /
 * AArch64). Exercises asm_call_capture_fp_n / asm_call_capture_vec_n.
 *
 * double fp_sum10(double a0 .. a9);  sum of all ten doubles
 * double fp_stack2(double a0 .. a9); a8 + a9 only (the two stack args)
 * vec128 vec_sum10(vec128 a0 .. a9); lane-wise sum of all ten vectors
 *
 *   x86-64: a0..a7 -> xmm0..7; the stack args sit just above the return
 *           address (doubles at 8/16(%rsp); 16-byte vectors at 8/24(%rsp)).
 *   AArch64: a0..a7 -> d0..7 / v0..7; the stack args start at [sp, #0]
 *           (doubles at #0/#8; 16-byte vectors at #0/#16).
 */
#include "asm.h"

ASM_FUNC fp_sum10
#if defined(__x86_64__)
    addsd   %xmm1, %xmm0
    addsd   %xmm2, %xmm0
    addsd   %xmm3, %xmm0
    addsd   %xmm4, %xmm0
    addsd   %xmm5, %xmm0
    addsd   %xmm6, %xmm0
    addsd   %xmm7, %xmm0
    addsd   8(%rsp), %xmm0         /* a8 */
    addsd   16(%rsp), %xmm0        /* a9 */
    ret
#elif defined(__aarch64__)
    fadd    d0, d0, d1
    fadd    d0, d0, d2
    fadd    d0, d0, d3
    fadd    d0, d0, d4
    fadd    d0, d0, d5
    fadd    d0, d0, d6
    fadd    d0, d0, d7
    ldr     d16, [sp, #0]          /* a8 */
    fadd    d0, d0, d16
    ldr     d16, [sp, #8]          /* a9 */
    fadd    d0, d0, d16
    ret
#endif
ASM_ENDFUNC fp_sum10

ASM_FUNC fp_stack2
#if defined(__x86_64__)
    movsd   8(%rsp), %xmm0         /* a8 */
    addsd   16(%rsp), %xmm0        /* a9 */
    ret
#elif defined(__aarch64__)
    ldr     d0, [sp, #0]           /* a8 */
    ldr     d1, [sp, #8]           /* a9 */
    fadd    d0, d0, d1
    ret
#endif
ASM_ENDFUNC fp_stack2

ASM_FUNC vec_sum10
#if defined(__x86_64__)
    addps   %xmm1, %xmm0
    addps   %xmm2, %xmm0
    addps   %xmm3, %xmm0
    addps   %xmm4, %xmm0
    addps   %xmm5, %xmm0
    addps   %xmm6, %xmm0
    addps   %xmm7, %xmm0
    addps   8(%rsp), %xmm0         /* a8 (16-byte aligned) */
    addps   24(%rsp), %xmm0        /* a9 */
    ret
#elif defined(__aarch64__)
    fadd    v0.4s, v0.4s, v1.4s
    fadd    v0.4s, v0.4s, v2.4s
    fadd    v0.4s, v0.4s, v3.4s
    fadd    v0.4s, v0.4s, v4.4s
    fadd    v0.4s, v0.4s, v5.4s
    fadd    v0.4s, v0.4s, v6.4s
    fadd    v0.4s, v0.4s, v7.4s
    ldr     q16, [sp, #0]          /* a8 */
    fadd    v0.4s, v0.4s, v16.4s
    ldr     q16, [sp, #16]         /* a9 */
    fadd    v0.4s, v0.4s, v16.4s
    ret
#endif
ASM_ENDFUNC vec_sum10
