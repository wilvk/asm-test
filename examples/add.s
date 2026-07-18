/*
 * add.s — example routine under test. Portable across x86-64 and AArch64,
 * Linux and macOS (ASM_FUNC handles symbol decoration; #if selects the body).
 *
 * long add_signed(long a, long b);
 *   x86-64: a -> %rdi, b -> %rsi, result -> %rax
 *   AArch64: a -> x0,  b -> x1,   result -> x0
 */
#include "asm.h"

ASM_FUNC add_signed
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    add     a0, a0, a1
    ret
#endif
ASM_ENDFUNC add_signed
