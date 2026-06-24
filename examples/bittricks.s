/*
 * bittricks.s — branchless "bit-hack" routines under test (portable x86-64 /
 * AArch64). These are the kind of routine where a typo in a shift amount or a
 * mask constant produces an answer that is right for *most* inputs and wrong at
 * a power-of-two boundary or the all-zero/all-ones case — exactly the bugs the
 * differential / property-testing engine is built to surface. See
 * test_bittricks.c for the C reference models the framework fuzzes them against.
 *
 *   unsigned long popcount64(unsigned long x);  count set bits (SWAR, pure ALU)
 *   unsigned long next_pow2(unsigned long x);   smallest power of two >= x (x>=1)
 *   unsigned long reverse_byte(unsigned long x); reverse the low 8 bits
 */
#include "asm.h"

/* SWAR (SIMD-within-a-register) population count: no popcnt instruction, so the
 * same arithmetic runs identically on every target — a clean differential
 * target whose magic constants are easy to mistype. */
ASM_FUNC popcount64
#if defined(__x86_64__)
    movq    %rdi, %rax
    movq    %rdi, %rcx
    shrq    $1, %rcx
    movabsq $0x5555555555555555, %rdx
    andq    %rdx, %rcx
    subq    %rcx, %rax              /* x - ((x>>1) & 0x5555...) */
    movabsq $0x3333333333333333, %rdx
    movq    %rax, %rcx
    shrq    $2, %rcx
    andq    %rdx, %rax
    andq    %rdx, %rcx
    addq    %rcx, %rax             /* (x & m2) + ((x>>2) & m2)  */
    movq    %rax, %rcx
    shrq    $4, %rcx
    addq    %rcx, %rax
    movabsq $0x0F0F0F0F0F0F0F0F, %rdx
    andq    %rdx, %rax            /* (x + (x>>4)) & 0x0F0F...   */
    movabsq $0x0101010101010101, %rdx
    imulq   %rdx, %rax           /* sum bytes into the top byte */
    shrq    $56, %rax
    ret
#elif defined(__aarch64__)
    fmov    d0, x0               /* move bits into the NEON file  */
    cnt     v0.8b, v0.8b         /* per-byte population count     */
    addv    b0, v0.8b            /* horizontal sum of the 8 bytes */
    fmov    w0, s0               /* result (0..64) back to x0     */
    ret
#endif
ASM_ENDFUNC popcount64

/* Round up to the next power of two by smearing the highest set bit down across
 * all lower positions, then incrementing. Defined for x >= 1 (the tests fuzz a
 * positive range); exact powers must return themselves, the classic off-by-one. */
ASM_FUNC next_pow2
#if defined(__x86_64__)
    movq    %rdi, %rax
    decq    %rax
    movq    %rax, %rcx
    shrq    $1, %rcx
    orq     %rcx, %rax
    movq    %rax, %rcx
    shrq    $2, %rcx
    orq     %rcx, %rax
    movq    %rax, %rcx
    shrq    $4, %rcx
    orq     %rcx, %rax
    movq    %rax, %rcx
    shrq    $8, %rcx
    orq     %rcx, %rax
    movq    %rax, %rcx
    shrq    $16, %rcx
    orq     %rcx, %rax
    movq    %rax, %rcx
    shrq    $32, %rcx
    orq     %rcx, %rax
    incq    %rax
    ret
#elif defined(__aarch64__)
    sub     x0, x0, #1
    orr     x0, x0, x0, lsr #1
    orr     x0, x0, x0, lsr #2
    orr     x0, x0, x0, lsr #4
    orr     x0, x0, x0, lsr #8
    orr     x0, x0, x0, lsr #16
    orr     x0, x0, x0, lsr #32
    add     x0, x0, #1
    ret
#endif
ASM_ENDFUNC next_pow2

/* Reverse the low 8 bits of x (upper bits cleared): 0x01 -> 0x80, 0xAA -> 0x55.
 * x86-64 does the 3-step adjacent/pair/nibble swap; AArch64 has a single rbit. */
ASM_FUNC reverse_byte
#if defined(__x86_64__)
    movzbl  %dil, %eax           /* keep only the low byte */
    movq    %rax, %rcx
    shrq    $1, %rcx
    andq    $0x55, %rcx
    andq    $0x55, %rax
    shlq    $1, %rax
    orq     %rcx, %rax           /* swap adjacent bits     */
    movq    %rax, %rcx
    shrq    $2, %rcx
    andq    $0x33, %rcx
    andq    $0x33, %rax
    shlq    $2, %rax
    orq     %rcx, %rax           /* swap bit pairs         */
    movq    %rax, %rcx
    shrq    $4, %rcx
    andq    $0x0F, %rcx
    andq    $0x0F, %rax
    shlq    $4, %rax
    orq     %rcx, %rax           /* swap nibbles           */
    ret
#elif defined(__aarch64__)
    and     x0, x0, #0xFF
    rbit    x0, x0               /* reverse all 64 bits ... */
    lsr     x0, x0, #56          /* ... then take the byte  */
    ret
#endif
ASM_ENDFUNC reverse_byte
