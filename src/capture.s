/*
 * capture.s — register/flags capture trampoline (x86-64, System V AMD64 ABI).
 *
 *   void asm_call_capture(regs_t *out, void *fn, const long args[6]);
 *     out  -> %rdi   (where to write the captured state)
 *     fn   -> %rsi   (routine under test)
 *     args -> %rdx   (six integer args: args[0..5] -> rdi,rsi,rdx,rcx,r8,r9)
 *
 * The trampoline seeds the callee-saved registers (rbx, rbp, r12-r15) with
 * known sentinels, marshals the six integer args into the ABI argument
 * registers, calls fn, then snapshots rax/rdx (return), the callee-saved
 * registers (to verify the routine restored them), and RFLAGS into *out.
 *
 * regs_t layout (see asmtest.h), byte offsets:
 *   rax 0, rdx 8, rbx 16, rbp 24, r12 32, r13 40, r14 48, r15 56, rflags 64
 *
 * Sentinels MUST match ASMTEST_SENTINEL_* in asmtest.h.
 */
#include "asm.h"

#if !defined(__x86_64__)
#  error "capture.s currently supports x86-64 only (AArch64 planned for a later phase)"
#endif

ASM_FUNC(asm_call_capture)
    /* Preserve this function's caller's callee-saved registers. */
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
    movq    0(%rax),  %rdi          /* args[0] -> rdi */
    movq    8(%rax),  %rsi          /* args[1] -> rsi */
    movq    24(%rax), %rcx          /* args[3] -> rcx */
    movq    32(%rax), %r8           /* args[4] -> r8  */
    movq    40(%rax), %r9           /* args[5] -> r9  */
    movq    16(%rax), %rdx          /* args[2] -> rdx (load last) */

    /* Seed callee-saved registers with sentinels for the ABI check. */
    movabsq $0x1111111111111111, %rbx
    movabsq $0x2222222222222222, %rbp
    movabsq $0x3333333333333333, %r12
    movabsq $0x4444444444444444, %r13
    movabsq $0x5555555555555555, %r14
    movabsq $0x6666666666666666, %r15

    movq    8(%rsp), %r11           /* fn (r11 is caller-saved) */
    xorl    %eax, %eax              /* variadic-safe: 0 vector regs */
    call    *%r11

    /* Capture results. MOV/loads below do not disturb RFLAGS. */
    movq    0(%rsp), %r11           /* reload out (fn may have clobbered regs) */
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
    movq    %rax, 64(%r11)          /* rflags as fn left them */

    addq    $24, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    ret
ASM_ENDFUNC(asm_call_capture)
