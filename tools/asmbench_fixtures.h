/*
 * asmbench_fixtures.h — cross-ISA benchmark fixtures.
 *
 * The SAME small algorithms authored per guest ISA as raw machine code, so the
 * deterministic instruction-count metric (asmtest_trace_t.insns_total) can be
 * compared across architectures. Raw bytes rather than assembly strings so the
 * fixtures need no assembler (Keystone) at build time — the AArch64 / RV64 /
 * ARM32 `add3` bodies are the tested fixtures from examples/test_emu.c.
 *
 * `add3(a,b,c) = a+b+c` is straight-line: it exposes the natural cross-ISA
 * difference (three-operand adds on the RISC ISAs need no initial move, so they
 * retire one fewer instruction than the x86 two-operand form). `sum_to_n` is a
 * counted loop whose instruction count scales with the trip count — used both as
 * an emulated-count benchmark and, on an x86-64 host, as the routine the
 * host-native trace-completeness probe runs (a loop is what makes a fixed-window
 * hardware backend truncate).
 */
#ifndef ASMBENCH_FIXTURES_H
#define ASMBENCH_FIXTURES_H

#include <stddef.h>

/* add3, System V x86-64 (args rdi,rsi,rdx): mov rax,rdi; add rax,rsi; add rax,rdx; ret */
static const unsigned char FIX_X86_ADD3[] = {0x48, 0x89, 0xf8, 0x48, 0x01,
                                             0xf0, 0x48, 0x01, 0xd0, 0xc3};
/* add3, Win64 (args rcx,rdx,r8): mov rax,rcx; add rax,rdx; add rax,r8; ret */
static const unsigned char FIX_WIN64_ADD3[] = {0x48, 0x89, 0xc8, 0x48, 0x01,
                                               0xd0, 0x4c, 0x01, 0xc0, 0xc3};
/* add3, AArch64: add x0,x0,x1; add x0,x0,x2; ret */
static const unsigned char FIX_A64_ADD3[] = {
    0x00, 0x00, 0x01, 0x8b, 0x00, 0x00, 0x02, 0x8b, 0xc0, 0x03, 0x5f, 0xd6};
/* add3, RV64: add a0,a0,a1; add a0,a0,a2; ret (jalr x0,0(ra)) */
static const unsigned char FIX_RV_ADD3[] = {0x33, 0x05, 0xb5, 0x00, 0x33, 0x05,
                                            0xc5, 0x00, 0x67, 0x80, 0x00, 0x00};
/* add3, ARM32 (A32): add r0,r0,r1; add r0,r0,r2; bx lr */
static const unsigned char FIX_A32_ADD3[] = {
    0x01, 0x00, 0x80, 0xe0, 0x02, 0x00, 0x80, 0xe0, 0x1e, 0xff, 0x2f, 0xe1};

/* sum_to_n(n) x86-64 counted loop (arg n in rdi), returns n+(n-1)+...+1:
 *   xor eax,eax;  L: add rax,rdi; dec rdi; jnz L;  ret
 * insns executed for n>0 = 1 + 3*n + 1. */
static const unsigned char FIX_X86_SUMTON[] = {
    0x31, 0xc0, 0x48, 0x01, 0xf8, 0x48, 0xff, 0xcf, 0x75, 0xf8, 0xc3};

/* tri(n) = n + tri(n-1), tri(0) = 0 — the same triangular sum by RECURSION (x86-64
 * SysV, arg n in edi), returns n(n+1)/2:
 *   test edi,edi; jne .rec; xor eax,eax; ret;
 *   .rec: push rdi; dec edi; call tri; pop rdi; add rax,rdi; ret
 * Unlike sum_to_n's counted loop it nests CALL/RET n deep, so a fixed-window
 * backend truncates on call-STACK depth (AMD LBR's 16-deep return stack) — the
 * orthogonal capture axis the loop ladder does not touch. insns executed = 8*n+4
 * (8 per recursive frame + 4 for the base case). Bytes verified against clang. */
static const unsigned char FIX_X86_TRI[] = {
    0x85, 0xff, 0x75, 0x03, 0x31, 0xc0, 0xc3, 0x57, 0xff, 0xcf,
    0xe8, 0xf1, 0xff, 0xff, 0xff, 0x5f, 0x48, 0x01, 0xf8, 0xc3};

#endif /* ASMBENCH_FIXTURES_H */
