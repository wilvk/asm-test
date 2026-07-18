/*
 * bench.s — example routine for the Phase 9 benchmark demo (portable x86-64 /
 * AArch64). A simple counted loop, so its per-call cost is clearly above a
 * single-instruction routine like add_signed.
 *
 * long sum_to_n(long n);  returns n + (n-1) + ... + 1 (0 for n <= 0).
 *   x86-64: n -> %rdi, result -> %rax
 *   AArch64: n -> x0,  result -> x0
 */
#include "asm.h"

ASM_FUNC sum_to_n
#if defined(__x86_64__)
    xorq    %rax, %rax          /* acc = 0                 */
    testq   %rdi, %rdi          /* n <= 0 ? return 0       */
    jle     2f
1:
    addq    %rdi, %rax          /* acc += n                */
    decq    %rdi
    jnz     1b
2:
    ret
#elif defined(__aarch64__)
    mov     x1, x0              /* counter = n             */
    mov     x0, #0             /* acc = 0                 */
    cmp     x1, #0
    b.le    2f
1:
    add     x0, x0, x1          /* acc += counter          */
    subs    x1, x1, #1
    b.ne    1b
2:
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    mv      a1, a0              /* counter = n             */
    li      a0, 0              /* acc = 0                 */
    blez    a1, 2f
1:
    add     a0, a0, a1          /* acc += counter          */
    addi    a1, a1, -1
    bnez    a1, 1b
2:
    ret
#endif
ASM_ENDFUNC sum_to_n
