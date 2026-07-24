/*
 * ct_eq.s — constant-time buffer equality, and its leaky control
 * (portable x86-64 / AArch64).
 *
 * The pair exists so the constant-time property can be TESTED rather than
 * asserted in prose (docs/internal/gui/06-doors-and-learning.md T1). Correctness
 * is the easy half; the property that matters is that no basic block depends on
 * the secret bytes. `test_ct_eq.c` proves it by unioning emulator block coverage
 * across secret-differing inputs — the union must not grow for `ct_eq`, and must
 * grow for `leaky_eq`, or the assertion has no teeth.
 *
 * long ct_eq(const void *a, const void *b, long n);
 *   1 if the first n bytes match, else 0. The loop bound is the PUBLIC length n,
 *   so the only branch is on n. Bytes are XOR-accumulated and collapsed
 *   branchlessly.
 *
 * long leaky_eq(const void *a, const void *b, long n);
 *   The naive early-exit compare — the negative control. Its exit branch depends
 *   on WHERE the buffers first differ, which is exactly the secret-dependent
 *   control flow ct_eq avoids.
 */
#include "asm.h"

/* No rv64 or ARM32 bodies: the emulator-side CT proof drives HOST-compiled
 * bytes, and the suite's proof tests already gate on an x86-64 host. The two
 * bodies below are the two hosts the project's example assembly targets. */

ASM_FUNC ct_eq
#if defined(__x86_64__)
    xorl    %eax, %eax              /* acc = 0                              */
    xorl    %ecx, %ecx              /* i = 0                                */
1:
    cmpq    %rdx, %rcx              /* i vs n — the PUBLIC length           */
    jge     2f
    movzbl  (%rdi,%rcx,1), %r8d
    movzbl  (%rsi,%rcx,1), %r9d
    xorl    %r9d, %r8d
    orl     %r8d, %eax              /* acc |= a[i] ^ b[i]                   */
    incq    %rcx
    jmp     1b
2:
    /* Branchless collapse: acc == 0  ->  1, else 0.
     * cmp $1,%eax sets CF iff eax == 0; sbb %eax,%eax then yields 0 or -1;
     * neg turns -1 into 1. No branch on acc, which is the secret. */
    cmpl    $1, %eax
    sbbl    %eax, %eax
    negl    %eax
    cltq                            /* sign-extend eax into rax (0 or 1)    */
    ret
#elif defined(__aarch64__)
    mov     w4, #0                  /* acc = 0                              */
    mov     x3, #0                  /* i = 0                                */
1:
    cmp     x3, x2
    b.ge    2f
    ldrb    w5, [x0, x3]
    ldrb    w6, [x1, x3]
    eor     w5, w5, w6
    orr     w4, w4, w5
    add     x3, x3, #1
    b       1b
2:
    cmp     w4, #0
    cset    x0, eq                  /* branchless: 1 iff acc == 0           */
    ret
#endif
ASM_ENDFUNC ct_eq

ASM_FUNC leaky_eq
#if defined(__x86_64__)
    xorl    %ecx, %ecx
1:
    cmpq    %rdx, %rcx
    jge     3f
    movzbl  (%rdi,%rcx,1), %r8d
    movzbl  (%rsi,%rcx,1), %r9d
    cmpl    %r9d, %r8d
    jne     2f                      /* EARLY EXIT — depends on the data     */
    incq    %rcx
    jmp     1b
2:
    xorl    %eax, %eax
    ret
3:
    movl    $1, %eax
    ret
#elif defined(__aarch64__)
    mov     x3, #0
1:
    cmp     x3, x2
    b.ge    3f
    ldrb    w5, [x0, x3]
    ldrb    w6, [x1, x3]
    cmp     w5, w6
    b.ne    2f                      /* EARLY EXIT — depends on the data     */
    add     x3, x3, #1
    b       1b
2:
    mov     x0, #0
    ret
3:
    mov     x0, #1
    ret
#endif
ASM_ENDFUNC leaky_eq
