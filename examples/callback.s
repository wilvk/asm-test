/*
 * callback.s — routines that call back into C through a function pointer
 * (portable x86-64 / AArch64). These demonstrate the "libc-callback" pattern:
 * an assembly routine that takes a pointer to caller data plus a callback and
 * invokes it per element — the same shape as qsort()'s comparator or a map().
 *
 *   long sum_map(const long *arr, long n, long (*fn)(long));
 *       returns sum of fn(arr[i]) for i in [0, n); 0 when n <= 0.
 *   long count_if(const long *arr, long n, long (*pred)(long));
 *       returns the count of elements for which pred(arr[i]) is nonzero.
 *
 * The interesting part is ABI discipline ACROSS the call: live state (the
 * pointer, the counter, the callback, the accumulator) lives in callee-saved
 * registers so the C callback may clobber the caller-saved set freely, and the
 * stack is kept 16-byte aligned at each call site — the bugs this example is
 * built to catch. See test_callback.c for the C callbacks and assertions.
 */
#include "asm.h"

ASM_FUNC sum_map
#if defined(__x86_64__)
    pushq   %rbx                /* arr cursor       */
    pushq   %r12                /* remaining count  */
    pushq   %r13                /* callback pointer */
    pushq   %r14                /* accumulator      */
    pushq   %r15                /* align: 5 pushes -> rsp 16-aligned at call */
    movq    %rdi, %rbx
    movq    %rsi, %r12
    movq    %rdx, %r13
    xorq    %r14, %r14
1:
    testq   %r12, %r12
    jle     2f                  /* signed: n <= 0 ends the loop */
    movq    (%rbx), %rdi        /* marshal arr[i] into the 1st int arg */
    addq    $8, %rbx
    call    *%r13               /* clobbers caller-saved regs; ours are safe */
    addq    %rax, %r14
    decq    %r12
    jmp     1b
2:
    movq    %r14, %rax
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    ret
#elif defined(__aarch64__)
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    mov     x19, x0             /* arr cursor       */
    mov     x20, x1             /* remaining count  */
    mov     x21, x2             /* callback pointer */
    mov     x22, xzr            /* accumulator      */
1:
    cmp     x20, #0
    ble     2f
    ldr     x0, [x19], #8       /* load arr[i], post-increment cursor */
    blr     x21
    add     x22, x22, x0
    sub     x20, x20, #1
    b       1b
2:
    mov     x0, x22
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #48
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    addi    sp, sp, -48         /* 16-aligned frame */
    sd      ra, 0(sp)
    sd      s0, 8(sp)           /* arr cursor       */
    sd      s1, 16(sp)          /* remaining count  */
    sd      s2, 24(sp)          /* callback pointer */
    sd      s3, 32(sp)          /* accumulator      */
    mv      s0, a0
    mv      s1, a1
    mv      s2, a2
    li      s3, 0
1:
    blez    s1, 2f              /* signed: n <= 0 ends the loop */
    ld      a0, 0(s0)           /* marshal arr[i] into the 1st int arg */
    addi    s0, s0, 8
    jalr    ra, 0(s2)           /* clobbers caller-saved regs; ours are safe */
    add     s3, s3, a0
    addi    s1, s1, -1
    j       1b
2:
    mv      a0, s3
    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    addi    sp, sp, 48
    ret
#endif
ASM_ENDFUNC sum_map

ASM_FUNC count_if
#if defined(__x86_64__)
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    movq    %rdi, %rbx
    movq    %rsi, %r12
    movq    %rdx, %r13
    xorq    %r14, %r14
1:
    testq   %r12, %r12
    jle     2f
    movq    (%rbx), %rdi
    addq    $8, %rbx
    call    *%r13
    testq   %rax, %rax          /* predicate nonzero? */
    jz      3f
    incq    %r14
3:
    decq    %r12
    jmp     1b
2:
    movq    %r14, %rax
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    ret
#elif defined(__aarch64__)
    stp     x29, x30, [sp, #-48]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    mov     x19, x0
    mov     x20, x1
    mov     x21, x2
    mov     x22, xzr
1:
    cmp     x20, #0
    ble     2f
    ldr     x0, [x19], #8
    blr     x21
    cbz     x0, 3f              /* predicate zero -> skip */
    add     x22, x22, #1
3:
    sub     x20, x20, #1
    b       1b
2:
    mov     x0, x22
    ldp     x21, x22, [sp, #32]
    ldp     x19, x20, [sp, #16]
    ldp     x29, x30, [sp], #48
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    addi    sp, sp, -48
    sd      ra, 0(sp)
    sd      s0, 8(sp)
    sd      s1, 16(sp)
    sd      s2, 24(sp)
    sd      s3, 32(sp)
    mv      s0, a0
    mv      s1, a1
    mv      s2, a2
    li      s3, 0
1:
    blez    s1, 2f
    ld      a0, 0(s0)
    addi    s0, s0, 8
    jalr    ra, 0(s2)
    beqz    a0, 3f              /* predicate zero -> skip */
    addi    s3, s3, 1
3:
    addi    s1, s1, -1
    j       1b
2:
    mv      a0, s3
    ld      ra, 0(sp)
    ld      s0, 8(sp)
    ld      s1, 16(sp)
    ld      s2, 24(sp)
    ld      s3, 32(sp)
    addi    sp, sp, 48
    ret
#endif
ASM_ENDFUNC count_if
