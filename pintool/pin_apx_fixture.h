/*
 * pin_apx_fixture.h — the shared Intel APX (EGPR / REX2) parity fixture for T8.
 *
 * A short routine that touches r16/r17 via the REX2 prefix (0xD5): impossible to
 * encode or decode without APX, and squarely in the pinned DynamoRIO decoder's
 * blind spot (DR issue #6226 — 16 EGPRs, REX2, PUSH2/POP2, CCMP/CTEST). Intel's
 * XED (bundled in the Pin kit) decodes it fine — that decoder-currency gap is the
 * entire reason the Pin trace tier earns its keep.
 *
 * The bytes are hand-encoded (ubuntu 24.04's `as` predates APX) and their
 * APX-ness is PROVEN at runtime by examples/pin_apx_decode.c via xed_classify_apx()
 * — so a mis-encoding fails loudly rather than passing as not-really-APX.
 *
 * Disassembly (SysV; fn(a,b) computes a+b into r16, returns it):
 *   D5 18 89 F8   mov r16, rdi   (REX2.W, B4=1 for r16; reg=rdi)      off 0x0
 *   D5 18 01 F0   add r16, rsi   (REX2.W, B4=1 for r16; reg=rsi)      off 0x4
 *   D5 48 89 C0   mov rax, r16   (REX2.W, R4=1 for r16; rm=rax)       off 0x8
 *   C3            ret                                                 off 0xc
 * 4 instructions, 13 bytes; the first three are APX (xed_classify_apx == 1), the
 * ret is not. Straight-line, so a full trace records insns_total == 4, truncated 0.
 */
#ifndef ASMTEST_PIN_APX_FIXTURE_H
#define ASMTEST_PIN_APX_FIXTURE_H

#include <stdint.h>

static const uint8_t APX_ROUTINE[] = {
    0xD5, 0x18, 0x89, 0xF8, /* mov r16, rdi   (off 0x0) */
    0xD5, 0x18, 0x01, 0xF0, /* add r16, rsi   (off 0x4) */
    0xD5, 0x48, 0x89, 0xC0, /* mov rax, r16   (off 0x8) */
    0xC3                    /* ret            (off 0xc) */
};

/* Total instructions and the subset XED tags APX (the ret is plain). */
#define APX_INSN_COUNT     4
#define APX_APX_INSN_COUNT 3
#define APX_REGION_NAME    "apx"

#endif /* ASMTEST_PIN_APX_FIXTURE_H */
