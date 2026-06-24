/*
 * emucases.s — routines exercised under the emulator tier by test_emu_usecases.c
 * (portable x86-64 / AArch64). The emulator copies a routine's bytes into a
 * virtual CPU and turns any invalid access into a precise fault, so these are
 * the buggable shapes: a counted read and a counted write whose length the test
 * deliberately drives past a mapped page, plus a 3-arg add used to check that
 * the same algorithm agrees across guest ISAs.
 *
 *   long sum_longs(const long *p, long n);   sum p[0..n)   (a counted load loop)
 *   void fill_longs(long *p, long n);        zero p[0..n)  (a counted store loop)
 *   long add3(long a, long b, long c);       a + b + c
 */
#include "asm.h"

ASM_FUNC sum_longs
#if defined(__x86_64__)
    xorq    %rax, %rax
    xorq    %rcx, %rcx
    testq   %rsi, %rsi
    jle     2f
1:
    addq    (%rdi,%rcx,8), %rax     /* read p[i] — faults if past the mapping */
    incq    %rcx
    cmpq    %rsi, %rcx
    jl      1b
2:
    ret
#elif defined(__aarch64__)
    mov     x2, xzr                 /* sum */
    mov     x3, xzr                 /* i   */
    cmp     x1, #0
    ble     2f
1:
    ldr     x4, [x0, x3, lsl #3]
    add     x2, x2, x4
    add     x3, x3, #1
    cmp     x3, x1
    b.lt    1b
2:
    mov     x0, x2
    ret
#endif
ASM_ENDFUNC sum_longs

ASM_FUNC fill_longs
#if defined(__x86_64__)
    xorq    %rcx, %rcx
    testq   %rsi, %rsi
    jle     2f
1:
    movq    $0, (%rdi,%rcx,8)       /* write p[i] — faults if past the mapping */
    incq    %rcx
    cmpq    %rsi, %rcx
    jl      1b
2:
    ret
#elif defined(__aarch64__)
    mov     x2, xzr                 /* i */
    cmp     x1, #0
    ble     2f
1:
    str     xzr, [x0, x2, lsl #3]
    add     x2, x2, #1
    cmp     x2, x1
    b.lt    1b
2:
    ret
#endif
ASM_ENDFUNC fill_longs

ASM_FUNC add3
#if defined(__x86_64__)
    movq    %rdi, %rax
    addq    %rsi, %rax
    addq    %rdx, %rax
    ret
#elif defined(__aarch64__)
    add     x0, x0, x1
    add     x0, x0, x2
    ret
#endif
ASM_ENDFUNC add3
