/*
 * test_asm.c — exercises the Keystone-backed in-line assembler tier.
 * Build/run via `make asm-test` (requires libkeystone + libunicorn).
 *
 * Unlike test_emu.c's x86 suite, these never drive host-compiled routines:
 * Keystone produces real machine code for the named guest, so every case runs
 * on any host. Two layers are covered — the assembler core (asmtest_assemble,
 * deterministic byte output) and the emu bridge (emu_call_asm et al., which
 * assemble at EMU_CODE_BASE and run through the emulator).
 */
#include "asmtest.h"
#include "asmtest_assemble.h"

#include <string.h>

/* ---- Assembler core: deterministic, host-independent --------------------- */

TEST(assemble, x86_intel_known_encoding) {
    /* mov rax, rdi; add rax, rsi; ret — the add_signed corpus routine. */
    asm_result_t r;
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                 "mov rax, rdi; add rax, rsi; ret",
                                 EMU_CODE_BASE, &r));
    static const uint8_t expect[] = {0x48, 0x89, 0xf8,  /* mov rax, rdi */
                                     0x48, 0x01, 0xf0,  /* add rax, rsi */
                                     0xc3};             /* ret          */
    ASSERT_EQ(r.len, sizeof expect);
    ASSERT_EQ(r.stat_count, 3);
    ASSERT_MEM_EQ(r.bytes, expect, sizeof expect);
    asmtest_asm_free(&r);
}

TEST(assemble, x86_att_matches_intel) {
    /* The same routine in GAS/AT&T must assemble to identical bytes. */
    asm_result_t intel, att;
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                 "mov rax, rdi; add rax, rsi; ret",
                                 EMU_CODE_BASE, &intel));
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_ATT,
                                 "movq %rdi, %rax; addq %rsi, %rax; ret",
                                 EMU_CODE_BASE, &att));
    ASSERT_EQ(att.len, intel.len);
    ASSERT_MEM_EQ(att.bytes, intel.bytes, intel.len);
    asmtest_asm_free(&intel);
    asmtest_asm_free(&att);
}

TEST(assemble, bad_mnemonic_reported_as_data) {
    asm_result_t r;
    ASSERT_FALSE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL, "frobnicate rax",
                                  EMU_CODE_BASE, &r));
    ASSERT_TRUE(r.bytes == NULL);
    ASSERT_TRUE(r.err[0] != '\0'); /* carries the Keystone diagnostic */
    asmtest_asm_free(&r);          /* safe on a failed result */
}

/* ---- Emu bridge: assemble + run through the emulator tier ----------------- */

TEST(asm_run, x86_add_returns_42) {
    emu_t *e = emu_open();
    ASSERT_TRUE(e != NULL);
    emu_result_t out;
    long args[] = {40, 2};
    ASSERT_TRUE(emu_call_asm(e, "mov rax, rdi; add rax, rsi; ret", args, 2, 0,
                             &out));
    ASSERT_FALSE(out.faulted);
    ASSERT_EQ(out.regs.rax, 42);
    emu_close(e);
}

TEST(asm_run, x86_branch_resolves_at_load_base) {
    /* A taken forward branch only lands correctly if Keystone assembled at the
     * same address the emulator loads at (EMU_CODE_BASE). rdi != 0 -> 7. */
    emu_t *e = emu_open();
    emu_result_t out;
    long args[] = {1, 0};
    ASSERT_TRUE(emu_call_asm(
        e, "test rdi, rdi; jz zero; mov rax, 7; ret; zero: mov rax, 9; ret",
        args, 2, 0, &out));
    ASSERT_FALSE(out.faulted);
    ASSERT_EQ(out.regs.rax, 7);
    emu_close(e);
}

TEST(asm_run, bad_asm_fails_without_crashing) {
    /* A bad string is a reported failure (false + uc_err), not a crash. The
     * diagnostic goes to stderr; the run never reaches the emulator. */
    emu_t *e = emu_open();
    emu_result_t out;
    long args[] = {0, 0};
    ASSERT_FALSE(emu_call_asm(e, "frobnicate rax", args, 0, 0, &out));
    ASSERT_EQ(out.uc_err, -1);
    emu_close(e);
}

TEST(asm_run, arm64_add_returns_42) {
    emu_arm64_t *e = emu_arm64_open();
    ASSERT_TRUE(e != NULL);
    emu_arm64_result_t out;
    long args[] = {40, 2};
    ASSERT_TRUE(emu_arm64_call_asm(e, "add x0, x0, x1; ret", args, 2, 0, &out));
    ASSERT_FALSE(out.faulted);
    ASSERT_EQ(out.regs.x[0], 42);
    emu_arm64_close(e);
}

TEST(asm_run, arm32_add_returns_42) {
    emu_arm_t *e = emu_arm_open();
    ASSERT_TRUE(e != NULL);
    emu_arm_result_t out;
    long args[] = {40, 2};
    ASSERT_TRUE(emu_arm_call_asm(e, "add r0, r0, r1; bx lr", args, 2, 0, &out));
    ASSERT_FALSE(out.faulted);
    ASSERT_EQ(out.regs.r[0], 42);
    emu_arm_close(e);
}

TEST(asm_run, riscv_add_or_unsupported) {
    /* RISC-V depends on the linked Keystone supporting it (released builds do
     * not). Either it assembles and runs (a0 == 42), or it reports a clean
     * "unsupported" failure — both are correct; neither may crash. */
    asm_result_t probe;
    bool can = asmtest_assemble(ASM_RISCV64, ASM_SYNTAX_INTEL,
                                "add a0, a0, a1; ret", EMU_CODE_BASE, &probe);
    asmtest_asm_free(&probe);
    if (!can)
        SKIP("RISC-V not supported by this Keystone build");

    emu_riscv_t *e = emu_riscv_open();
    emu_riscv_result_t out;
    long args[] = {40, 2};
    ASSERT_TRUE(emu_riscv_call_asm(e, "add a0, a0, a1; ret", args, 2, 0, &out));
    ASSERT_FALSE(out.faulted);
    ASSERT_EQ(out.regs.x[10], 42); /* a0 == x10 */
    emu_riscv_close(e);
}
