/*
 * flags.s — example routines exercising the carry flag and the ABI
 * (portable x86-64 / AArch64). The "rbx" routine names reflect the x86 case;
 * the AArch64 bodies use x19, the analogous callee-saved register.
 */
#include "asm.h"

/* The carry-flag routines have NO rv64 analog: RISC-V has no condition-flags
 * register (the asmtest.h rv64 branch defines ASMTEST_NO_FLAGS). Omit them there
 * — asm.h is not asmtest.h, so this .s cannot see ASMTEST_NO_FLAGS; the
 * equivalent arch condition guards the same code, and test_capture.c gates the
 * callers behind ASMTEST_NO_FLAGS itself. */
#if !(defined(__riscv) && __riscv_xlen == 64)
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
#endif /* !rv64: no condition-flags register */

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
#elif defined(__riscv) && __riscv_xlen == 64
    addi    sp, sp, -16
    sd      s1, 0(sp)           /* save the callee-saved scratch reg */
    add     s1, a0, a1
    mv      a0, s1
    ld      s1, 0(sp)           /* restore it -> ABI-compliant */
    addi    sp, sp, 16
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
#elif defined(__riscv) && __riscv_xlen == 64
    add     s1, a0, a1          /* trash a callee-saved reg, never restore it */
    mv      a0, s1
    ret
#endif
ASM_ENDFUNC clobbers_rbx

#if defined(__riscv) && __riscv_xlen == 64
/*
 * T3: FP callee-saved (fs0-fs11) preservation demo — rv64 only (RISC-V has no
 * 128-bit vector path, so ASSERT_ABI_PRESERVED_VEC rides the _fp trampoline's
 * fs seed/capture). Both take/return a double.
 *   double preserves_fs(double x);  uses fs2 as scratch but saves/restores it
 *                                   (ABI-compliant) — passes the check.
 *   double clobbers_fs2(double x);  trashes fs2 and never restores it
 *                                   (violation) — the check reports "fs2 not
 *                                   restored" (mirrors clobbers_rbx).
 */
ASM_FUNC preserves_fs
    addi    sp, sp, -16
    fsd     fs2, 0(sp)          /* save the callee-saved FP scratch reg */
    fadd.d  fs2, fa0, fa0       /* fs2 = 2*x */
    fmv.d   fa0, fs2
    fld     fs2, 0(sp)          /* restore it -> ABI-compliant */
    addi    sp, sp, 16
    ret
ASM_ENDFUNC preserves_fs

ASM_FUNC clobbers_fs2
    fadd.d  fs2, fa0, fa0       /* trash fs2 (callee-saved), never restore it */
    fmv.d   fa0, fs2
    ret
ASM_ENDFUNC clobbers_fs2
#endif
