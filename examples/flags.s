/*
 * flags.s — example routines exercising flags and the ABI (x86-64, SysV, GAS).
 */
#include "asm.h"

/* long set_carry(void); sets CF, returns 0. (mov does not disturb flags) */
ASM_FUNC(set_carry)
    stc
    movq    $0, %rax
    ret
ASM_ENDFUNC(set_carry)

/* long clear_carry(void); clears CF, returns 0. */
ASM_FUNC(clear_carry)
    clc
    movq    $0, %rax
    ret
ASM_ENDFUNC(clear_carry)

/*
 * long sum_via_rbx(long a, long b);
 * Uses rbx as scratch but saves/restores it — ABI-compliant.
 */
ASM_FUNC(sum_via_rbx)
    pushq   %rbx
    movq    %rdi, %rbx
    addq    %rsi, %rbx
    movq    %rbx, %rax
    popq    %rbx
    ret
ASM_ENDFUNC(sum_via_rbx)

/*
 * long clobbers_rbx(long a, long b);
 * Trashes rbx and never restores it — violates the ABI (for the demo).
 */
ASM_FUNC(clobbers_rbx)
    movq    %rdi, %rbx
    addq    %rsi, %rbx
    movq    %rbx, %rax
    ret
ASM_ENDFUNC(clobbers_rbx)
