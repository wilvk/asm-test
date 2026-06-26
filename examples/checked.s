/*
 * checked.s — runtime / compiler-rt style checked addition (portable x86-64 /
 * AArch64). Returns the (wrapping) sum and leaves the overflow flag set exactly
 * when the signed addition overflowed — the signal a __builtin_add_overflow or
 * compiler-rt __addvdi3 lowering must produce, and which only register/flag
 * capture can observe (a plain return value cannot).
 *
 * long checked_add(long a, long b);
 *   rax / x0 = a + b (wrapping); OF (x86) / V (AArch64) = signed overflow.
 *   Nothing after the add may disturb the flags before ret — same discipline as
 *   set_carry in flags.s (mov-immediate and ret leave the flags alone).
 */
#include "asm.h"

ASM_FUNC checked_add
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax          /* sets OF on signed overflow; sum in rax */
    ret
#elif defined(__aarch64__)
    adds    x0, x0, x1          /* sets NZCV (V = signed overflow); sum in x0 */
    ret
#endif
ASM_ENDFUNC checked_add
