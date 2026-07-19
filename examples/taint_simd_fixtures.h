/*
 * taint_simd_fixtures.h — the shared XMM/SSE SIMD seed/sink fixtures for the DynamoRIO
 * taint tier (Increment 8) and its libdft64 differential oracle's NAMED-SKIP arm.
 *
 * libdft64's SSE/AVX taint is "basic" and its upstream soundness note says the rules "may
 * be wrong", so the SIMD subset is cross-checked INFORMATIONALLY under the oracle
 * (libdft-partial-sse-avx, T6) — the delta is reported, never asserted. These bytes are
 * extracted VERBATIM from examples/dr_taint_simd.c so the DR side and the oracle run the
 * byte-identical XMM inputs; `make dr-taint-simd-test` is the proof no byte changed.
 *
 * <stdint.h>-only. The offset is exposed as SIMD_BRANCH_OFF (distinct from taint_fixtures.h's
 * SINK_OFF) so a TU can include both headers.
 */
#ifndef ASMTEST_TAINT_SIMD_FIXTURES_H
#define ASMTEST_TAINT_SIMD_FIXTURES_H

#include <stdint.h>

/* simd_copy: taint through an XMM register AND an SSE 16-byte vectorized copy. */
static const uint8_t simd_copy[] = {
    0xf3, 0x0f, 0x6f, 0x07, /* 0x00 movdqu xmm0, [rdi]  (SEED load, 16B) */
    0x66, 0x0f, 0x6f, 0xc8, /* 0x04 movdqa xmm1, xmm0   (XMM reg copy)   */
    0xf3, 0x0f, 0x7f, 0x0e, /* 0x08 movdqu [rsi], xmm1  (16B store->buf2)*/
    0x48, 0x8b, 0x06,       /* 0x0c mov    rax, [rsi]   (reload copy)    */
    0xc3,                   /* 0x0f ret                                  */
};

/* simd_sink: the seeded XMM lane reaches a GP register then a branch flag (SINK). */
static const uint8_t simd_sink[] = {
    0xf3, 0x0f, 0x6f, 0x07,       /* 0x00 movdqu xmm0, [rdi]  (SEED load) */
    0x66, 0x0f, 0x6f, 0xc8,       /* 0x04 movdqa xmm1, xmm0               */
    0x66, 0x48, 0x0f, 0x7e, 0xc9, /* 0x08 movq   rcx, xmm1   (XMM->GP)    */
    0x48, 0x85, 0xc9,             /* 0x0d test   rcx, rcx    (taint ZF)   */
    0x74, 0x03,                   /* 0x10 jz 0x15            (SINK)       */
    0x48, 0x89, 0xc8,             /* 0x12 mov    rax, rcx                 */
    0xc3,                         /* 0x15 ret                            */
};
#define SIMD_BRANCH_OFF 0x10 /* the jz instruction's region offset */

/* The 16 seed bytes; the low 8 (little-endian) reload into rax. */
static const uint8_t SEED16[16] = {0x88, 0x77, 0x66, 0x55, 0x44, 0x33,
                                   0x22, 0x11, 0x00, 0xff, 0xee, 0xdd,
                                   0xcc, 0xbb, 0xaa, 0x99};
#define SEED_LO64 0x1122334455667788ULL

#endif /* ASMTEST_TAINT_SIMD_FIXTURES_H */
