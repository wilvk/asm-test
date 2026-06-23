/*
 * add.s — example routine under test (x86-64, System V AMD64 ABI, GAS syntax).
 * Portable across Linux/macOS via ASM_FUNC (see include/asm.h).
 *
 * long add_signed(long a, long b);  a -> %rdi, b -> %rsi, result -> %rax
 */
#include "asm.h"

ASM_FUNC(add_signed)
    movq    %rdi, %rax          /* rax = a     */
    addq    %rsi, %rax          /* rax = a + b */
    ret
ASM_ENDFUNC(add_signed)
