/*
 * mem.s — example memory routine under test (portable x86-64 / AArch64).
 *
 * void fill_bytes(void *buf, long val, long n);
 *   Writes the low byte of val into buf[0..n).
 *   x86-64: buf -> %rdi, val -> %rsi, n -> %rdx
 *   AArch64: buf -> x0,  val -> x1,   n -> x2
 */
#include "asm.h"

ASM_FUNC fill_bytes
#if defined(__x86_64__)
    testq   %rdx, %rdx          /* n == 0 ? nothing to do  */
    je      2f
    xorq    %rcx, %rcx          /* i = 0                   */
1:
    movb    %sil, (%rdi,%rcx,1) /* buf[i] = (byte)val      */
    incq    %rcx
    cmpq    %rdx, %rcx
    jb      1b
2:
    ret
#elif defined(__aarch64__)
    cbz     x2, 2f              /* n == 0 ? nothing to do  */
    mov     x3, #0             /* i = 0                   */
1:
    strb    w1, [x0, x3]        /* buf[i] = (byte)val      */
    add     x3, x3, #1
    cmp     x3, x2
    b.lo    1b
2:
    ret
#endif
ASM_ENDFUNC fill_bytes
