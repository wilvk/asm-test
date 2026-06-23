/*
 * mem.s — example memory routine under test (x86-64, System V AMD64 ABI, GAS).
 *
 * void fill_bytes(void *buf, long val, long n);
 *   buf -> %rdi, val -> %rsi (low byte used), n -> %rdx
 */
#include "asm.h"

ASM_FUNC(fill_bytes)
    testq   %rdx, %rdx          /* n == 0 ? nothing to do  */
    je      2f
    xorq    %rcx, %rcx          /* i = 0                   */
1:
    movb    %sil, (%rdi,%rcx,1) /* buf[i] = (byte)val      */
    incq    %rcx                /* i++                     */
    cmpq    %rdx, %rcx          /* i < n ?                 */
    jb      1b
2:
    ret
ASM_ENDFUNC(fill_bytes)
