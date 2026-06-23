/*
 * flags.s — example routines exercising the carry flag and the ABI
 * (portable x86-64 / AArch64). The "rbx" routine names reflect the x86 case;
 * the AArch64 bodies use x19, the analogous callee-saved register.
 */
#include "asm.h"

/* long set_carry(void); sets the carry flag, returns 0. */
ASM_FUNC set_carry
#if defined(__x86_64__)
    stc
    movq    $0, %rax
    ret
#elif defined(__aarch64__)
    mov     x0, #0
    cmp     x0, x0              /* 0 - 0: no borrow => C = 1 */
    ret
#endif
ASM_ENDFUNC set_carry

/* long clear_carry(void); clears the carry flag, returns 0. */
ASM_FUNC clear_carry
#if defined(__x86_64__)
    clc
    movq    $0, %rax
    ret
#elif defined(__aarch64__)
    mov     x0, #0
    cmp     x0, #1              /* 0 - 1: borrow => C = 0 */
    ret
#endif
ASM_ENDFUNC clear_carry

/*
 * long sum_via_rbx(long a, long b);
 * Uses a callee-saved register as scratch but saves/restores it — ABI-compliant.
 */
ASM_FUNC sum_via_rbx
#if defined(__x86_64__)
    pushq   %rbx
    movq    %rdi, %rbx
    addq    %rsi, %rbx
    movq    %rbx, %rax
    popq    %rbx
    ret
#elif defined(__aarch64__)
    str     x19, [sp, #-16]!
    add     x19, x0, x1
    mov     x0, x19
    ldr     x19, [sp], #16
    ret
#endif
ASM_ENDFUNC sum_via_rbx

/*
 * long clobbers_rbx(long a, long b);
 * Trashes a callee-saved register and never restores it — violates the ABI
 * (for the failure demo).
 */
ASM_FUNC clobbers_rbx
#if defined(__x86_64__)
    movq    %rdi, %rbx
    addq    %rsi, %rbx
    movq    %rbx, %rax
    ret
#elif defined(__aarch64__)
    add     x19, x0, x1
    mov     x0, x19
    ret
#endif
ASM_ENDFUNC clobbers_rbx
