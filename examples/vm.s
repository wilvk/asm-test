/*
 * vm.s — a tiny RPN bytecode interpreter, in assembly (portable x86-64 /
 * AArch64). Most example routines compute one expression; this one is a small
 * STATEFUL machine: it walks a byte program, maintains its own operand stack in
 * a local stack frame, and dispatches on an opcode each step. That makes it a
 * good stress for the things the framework inspects — callee-saved discipline
 * across a real loop, and (with a guard page) the promise that it never reads
 * past the program it was handed.
 *
 *   long vm_eval(const signed char *code, long n);
 *
 * Each byte of `code` (a signed value) is one instruction:
 *   v >= 0   push the literal v        (operands 0..127)
 *   -1  ADD  b = pop, a = pop, push a + b
 *   -2  SUB  b = pop, a = pop, push a - b
 *   -3  MUL  b = pop, a = pop, push a * b
 *   -4  NEG  a = pop, push -a
 *   (any other byte is ignored)
 * Returns the top of the operand stack at the end, or 0 if it is empty. The
 * operand stack lives in 256 bytes (32 slots) reserved on this routine's frame.
 */
#include "asm.h"

ASM_FUNC vm_eval
#if defined(__x86_64__)
    pushq   %rbx                   /* i (program cursor)       */
    pushq   %r12                   /* code pointer             */
    pushq   %r13                   /* n (program length)       */
    pushq   %r14                   /* operand-stack depth      */
    pushq   %r15                   /* operand-stack base       */
    subq    $256, %rsp             /* reserve the operand stack */
    movq    %rsp, %r15
    movq    %rdi, %r12
    movq    %rsi, %r13
    xorq    %rbx, %rbx
    xorq    %r14, %r14
6:                                 /* loop: i < n ?            */
    cmpq    %r13, %rbx
    jge     7f
    movsbq  (%r12,%rbx,1), %rax    /* sign-extend code[i]      */
    incq    %rbx
    testq   %rax, %rax
    jns     1f                     /* >= 0 -> push the literal */
    cmpq    $-1, %rax
    je      2f
    cmpq    $-2, %rax
    je      3f
    cmpq    $-3, %rax
    je      4f
    cmpq    $-4, %rax
    je      5f
    jmp     6b                     /* unknown opcode: ignore   */
1:                                 /* push rax                 */
    movq    %rax, (%r15,%r14,8)
    incq    %r14
    jmp     6b
2:                                 /* ADD: a + b               */
    decq    %r14
    movq    (%r15,%r14,8), %rdx    /* b (top)                  */
    decq    %r14
    movq    (%r15,%r14,8), %rax    /* a                        */
    addq    %rdx, %rax
    movq    %rax, (%r15,%r14,8)
    incq    %r14
    jmp     6b
3:                                 /* SUB: a - b               */
    decq    %r14
    movq    (%r15,%r14,8), %rdx
    decq    %r14
    movq    (%r15,%r14,8), %rax
    subq    %rdx, %rax
    movq    %rax, (%r15,%r14,8)
    incq    %r14
    jmp     6b
4:                                 /* MUL: a * b               */
    decq    %r14
    movq    (%r15,%r14,8), %rdx
    decq    %r14
    movq    (%r15,%r14,8), %rax
    imulq   %rdx, %rax
    movq    %rax, (%r15,%r14,8)
    incq    %r14
    jmp     6b
5:                                 /* NEG: -a                  */
    decq    %r14
    movq    (%r15,%r14,8), %rax
    negq    %rax
    movq    %rax, (%r15,%r14,8)
    incq    %r14
    jmp     6b
7:                                 /* result = top, or 0       */
    xorq    %rax, %rax
    testq   %r14, %r14
    jz      8f
    movq    -8(%r15,%r14,8), %rax
8:
    addq    $256, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    ret
#elif defined(__aarch64__)
    stp     x29, x30, [sp, #-272]! /* 16 (fp/lr) + 256 operand stack */
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    add     x23, sp, #48           /* operand-stack base        */
    mov     x19, x0                /* code pointer              */
    mov     x20, x1                /* n                         */
    mov     x21, xzr               /* i                         */
    mov     x22, xzr               /* operand-stack depth       */
1:
    cmp     x21, x20
    b.ge    7f
    ldrsb   x4, [x19, x21]         /* sign-extend code[i]       */
    add     x21, x21, #1
    cmp     x4, #0
    b.ge    2f                     /* >= 0 -> push the literal  */
    cmn     x4, #1                 /* x4 == -1 ? (ADD)          */
    b.eq    3f
    cmn     x4, #2                 /* SUB                       */
    b.eq    4f
    cmn     x4, #3                 /* MUL                       */
    b.eq    5f
    cmn     x4, #4                 /* NEG                       */
    b.eq    6f
    b       1b                     /* unknown opcode: ignore    */
2:                                 /* push x4                   */
    str     x4, [x23, x22, lsl #3]
    add     x22, x22, #1
    b       1b
3:                                 /* ADD                       */
    sub     x22, x22, #1
    ldr     x5, [x23, x22, lsl #3] /* b                         */
    sub     x22, x22, #1
    ldr     x6, [x23, x22, lsl #3] /* a                         */
    add     x6, x6, x5
    str     x6, [x23, x22, lsl #3]
    add     x22, x22, #1
    b       1b
4:                                 /* SUB: a - b                */
    sub     x22, x22, #1
    ldr     x5, [x23, x22, lsl #3]
    sub     x22, x22, #1
    ldr     x6, [x23, x22, lsl #3]
    sub     x6, x6, x5
    str     x6, [x23, x22, lsl #3]
    add     x22, x22, #1
    b       1b
5:                                 /* MUL                       */
    sub     x22, x22, #1
    ldr     x5, [x23, x22, lsl #3]
    sub     x22, x22, #1
    ldr     x6, [x23, x22, lsl #3]
    mul     x6, x6, x5
    str     x6, [x23, x22, lsl #3]
    add     x22, x22, #1
    b       1b
6:                                 /* NEG                       */
    sub     x22, x22, #1
    ldr     x6, [x23, x22, lsl #3]
    neg     x6, x6
    str     x6, [x23, x22, lsl #3]
    add     x22, x22, #1
    b       1b
7:                                 /* result = top, or 0        */
    mov     x0, xzr
    cbz     x22, 8f
    sub     x5, x22, #1
    ldr     x0, [x23, x5, lsl #3]
8:
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #272
    ret
#endif
ASM_ENDFUNC vm_eval
