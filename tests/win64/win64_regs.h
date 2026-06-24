/* win64_regs.h — Win64 (Microsoft x64) capture layout, Phase 1 slice.
 *
 * Mirror of the System V `regs_t` for the Microsoft x64 ABI. Promoted into
 * include/asmtest.h as the third `regs_t` branch in Phase 2 (with _Static_assert
 * offset pins, the manifest, and the conformance corpus). The offsets here MUST
 * match the stores in src/capture_win64.asm.
 *
 * Deltas vs. System V: the callee-saved set adds rdi and rsi (argument registers
 * on SysV, preserved on Win64). xmm6-xmm15 are also callee-saved on Win64; their
 * capture lands in a later FP/vector slice. Windows is LLP64, so the 64-bit
 * fields are uint64_t, not `long`.
 */
#ifndef ASMTEST_WIN64_REGS_H
#define ASMTEST_WIN64_REGS_H

#include <stdint.h>

/* A 128-bit vector lane (xmm register), captured low/high qword. */
typedef struct {
    uint64_t lo;
    uint64_t hi;
} win64_vec128_t;

typedef struct {
    uint64_t ret;          /* 0  rax (return value)       */
    uint64_t rdx;          /* 8  second return register   */
    uint64_t rbx;          /* 16 callee-saved             */
    uint64_t rbp;          /* 24 callee-saved             */
    uint64_t rdi;          /* 32 callee-saved (Win64)     */
    uint64_t rsi;          /* 40 callee-saved (Win64)     */
    uint64_t r12;          /* 48 callee-saved             */
    uint64_t r13;          /* 56 callee-saved             */
    uint64_t r14;          /* 64 callee-saved             */
    uint64_t r15;          /* 72 callee-saved             */
    uint64_t flags;        /* 80 RFLAGS                   */
    double   fret;         /* 88 xmm0 (FP return); valid after _fp/_vec    */
    win64_vec128_t vec[16];/* 96 xmm0..15; valid after _vec (vec[0] = ret) */
} win64_regs_t;

/* xmm6..xmm15 are callee-saved on Win64. The _vec trampoline seeds each with a
 * recognisable sentinel (xmm(6+k) lanes both = 6+k) so the preservation check is
 * "vec[i].lo == i && vec[i].hi == i" for i in 6..15. */
#define WIN64_XMM_CALLEE_LO 6
#define WIN64_XMM_CALLEE_HI 15

/* Sentinels seeded into the callee-saved set before the call: an intact value
 * afterwards means the routine preserved that register (the ABI check). */
#define WIN64_SENTINEL_RBX 0x1111111111111111ULL
#define WIN64_SENTINEL_RBP 0x2222222222222222ULL
#define WIN64_SENTINEL_RDI 0xDDDDDDDDDDDDDDDDULL
#define WIN64_SENTINEL_RSI 0xCCCCCCCCCCCCCCCCULL
#define WIN64_SENTINEL_R12 0x3333333333333333ULL
#define WIN64_SENTINEL_R13 0x4444444444444444ULL
#define WIN64_SENTINEL_R14 0x5555555555555555ULL
#define WIN64_SENTINEL_R15 0x6666666666666666ULL

/* RFLAGS bit masks (identical to System V x86-64). */
#define WIN64_CF (1ULL << 0)  /* carry    */
#define WIN64_PF (1ULL << 2)  /* parity   */
#define WIN64_ZF (1ULL << 6)  /* zero     */
#define WIN64_SF (1ULL << 7)  /* sign     */
#define WIN64_OF (1ULL << 11) /* overflow */

#endif /* ASMTEST_WIN64_REGS_H */
