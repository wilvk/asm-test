/*
 * capture.s — register/flags capture trampoline.
 *
 *   void asm_call_capture(regs_t *out, void *fn, const long args[6]);
 *
 * Seeds the callee-saved registers with known sentinels, marshals up to six
 * integer args into the ABI argument registers, calls fn, then snapshots the
 * return register, the callee-saved registers (to verify the routine restored
 * them), and the condition flags into *out.
 *
 * Sentinels and struct offsets MUST match asmtest.h.
 */
#include "asm.h"

ASM_FUNC asm_call_capture

#if defined(__x86_64__)
/* --- System V AMD64 ABI ---------------------------------------------------
 * out -> %rdi, fn -> %rsi, args -> %rdx
 * args[0..5] -> rdi,rsi,rdx,rcx,r8,r9. regs_t offsets:
 *   ret 0, rdx 8, rbx 16, rbp 24, r12 32, r13 40, r14 48, r15 56, flags 64
 */
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15
    /* 48 bytes pushed; reserve 24 more so rsp is 16-aligned before `call`. */
    subq    $24, %rsp
    movq    %rdi, 0(%rsp)           /* stash out across the call */
    movq    %rsi, 8(%rsp)           /* stash fn  across the call */

    /* Marshal args[0..5] into the integer argument registers. */
    movq    %rdx, %rax              /* rax = &args (scratch) */
    movq    0(%rax),  %rdi
    movq    8(%rax),  %rsi
    movq    24(%rax), %rcx
    movq    32(%rax), %r8
    movq    40(%rax), %r9
    movq    16(%rax), %rdx          /* load rdx last */

    /* Seed callee-saved registers with sentinels. */
    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    8(%rsp), %r11
    xorl    %eax, %eax              /* variadic-safe: 0 vector regs */
    call    *%r11

    /* Capture results. MOV/loads below do not disturb RFLAGS. */
    movq    0(%rsp), %r11           /* reload out */
    movq    %rax, 0(%r11)
    movq    %rdx, 8(%r11)
    movq    %rbx, 16(%r11)
    movq    %rbp, 24(%r11)
    movq    %r12, 32(%r11)
    movq    %r13, 40(%r11)
    movq    %r14, 48(%r11)
    movq    %r15, 56(%r11)
    pushfq
    popq    %rax
    movq    %rax, 64(%r11)

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret

#elif defined(__aarch64__)
/* --- AAPCS64 --------------------------------------------------------------
 * out -> x0, fn -> x1, args -> x2
 * args[0..5] -> x0..x5. regs_t offsets:
 *   ret 0, x19 8, x20 16, ... x28 80, x29 88, flags 96
 */
    sub     sp, sp, #112
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]     /* save frame pointer and link register */
    str     x0, [sp, #96]           /* stash out across the call */
    str     x1, [sp, #104]          /* stash fn  across the call */

    /* Marshal args[0..5] into x0..x5. */
    mov     x9, x2
    ldp     x0, x1, [x9, #0]
    ldp     x2, x3, [x9, #16]
    ldp     x4, x5, [x9, #32]

    /* Seed callee-saved registers (x19-x29) with sentinels. */
    ldr     x19, =0x1111111111111111
    ldr     x20, =0x2222222222222222
    ldr     x21, =0x3333333333333333
    ldr     x22, =0x4444444444444444
    ldr     x23, =0x5555555555555555
    ldr     x24, =0x6666666666666666
    ldr     x25, =0x7777777777777777
    ldr     x26, =0x8888888888888888
    ldr     x27, =0x9999999999999999
    ldr     x28, =0xAAAAAAAAAAAAAAAA
    ldr     x29, =0xBBBBBBBBBBBBBBBB

    ldr     x10, [sp, #104]         /* fn */
    blr     x10

    /* Capture results. Loads/stores below do not disturb NZCV. */
    ldr     x11, [sp, #96]          /* reload out */
    str     x0,  [x11, #0]          /* ret */
    str     x19, [x11, #8]
    str     x20, [x11, #16]
    str     x21, [x11, #24]
    str     x22, [x11, #32]
    str     x23, [x11, #40]
    str     x24, [x11, #48]
    str     x25, [x11, #56]
    str     x26, [x11, #64]
    str     x27, [x11, #72]
    str     x28, [x11, #80]
    str     x29, [x11, #88]
    mrs     x12, nzcv
    str     x12, [x11, #96]

    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #112
    ret

#else
#  error "capture.s supports x86-64 and AArch64 only"
#endif

ASM_ENDFUNC asm_call_capture
