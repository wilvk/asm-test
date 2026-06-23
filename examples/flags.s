/*
 * flags.s — example routines exercising flags and the ABI (x86-64, SysV, GAS).
 */
    .text

/* long set_carry(void); sets CF, returns 0. (mov does not disturb flags) */
    .globl _set_carry
_set_carry:
    stc
    movq    $0, %rax
    ret

/* long clear_carry(void); clears CF, returns 0. */
    .globl _clear_carry
_clear_carry:
    clc
    movq    $0, %rax
    ret

/*
 * long sum_via_rbx(long a, long b);
 * Uses rbx as scratch but saves/restores it — ABI-compliant.
 */
    .globl _sum_via_rbx
_sum_via_rbx:
    pushq   %rbx
    movq    %rdi, %rbx
    addq    %rsi, %rbx
    movq    %rbx, %rax
    popq    %rbx
    ret

/*
 * long clobbers_rbx(long a, long b);
 * Trashes rbx and never restores it — violates the ABI (for the demo).
 */
    .globl _clobbers_rbx
_clobbers_rbx:
    movq    %rdi, %rbx
    addq    %rsi, %rbx
    movq    %rbx, %rax
    ret
