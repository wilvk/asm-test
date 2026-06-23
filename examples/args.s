/*
 * args.s — example routines with varying argument counts, to exercise the
 * register + stack argument passing of asm_call_capture_args.
 *
 *   long sum3(a, b, c);                 (register-only)
 *   long sum8(a, b, c, d, e, f, g, h);  (x86-64: 2 stack args; AArch64: 0)
 *   long sum10(a..j);                   (x86-64: 4 stack args; AArch64: 2)
 *
 * x86-64 stack args sit at 8(%rsp), 16(%rsp)... at entry (after the call's
 * return address); AArch64 stack args sit at [sp,#0], [sp,#8]...
 */
#include "asm.h"

ASM_FUNC sum3
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    addq    %rdx, %rax
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    add     x0, x0, x2
    ret
#endif
ASM_ENDFUNC sum3

ASM_FUNC sum8
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    addq    %rdx, %rax
    addq    %rcx, %rax
    addq    %r8, %rax
    addq    %r9, %rax
    addq    8(%rsp), %rax       /* 7th arg */
    addq    16(%rsp), %rax      /* 8th arg */
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    add     x0, x0, x2
    add     x0, x0, x3
    add     x0, x0, x4
    add     x0, x0, x5
    add     x0, x0, x6
    add     x0, x0, x7
    ret
#endif
ASM_ENDFUNC sum8

ASM_FUNC sum10
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    addq    %rdx, %rax
    addq    %rcx, %rax
    addq    %r8, %rax
    addq    %r9, %rax
    addq    8(%rsp), %rax       /* 7th  */
    addq    16(%rsp), %rax      /* 8th  */
    addq    24(%rsp), %rax      /* 9th  */
    addq    32(%rsp), %rax      /* 10th */
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    add     x0, x0, x2
    add     x0, x0, x3
    add     x0, x0, x4
    add     x0, x0, x5
    add     x0, x0, x6
    add     x0, x0, x7
    ldr     x9, [sp, #0]        /* 9th arg  */
    add     x0, x0, x9
    ldr     x9, [sp, #8]        /* 10th arg */
    add     x0, x0, x9
    ret
#endif
ASM_ENDFUNC sum10
