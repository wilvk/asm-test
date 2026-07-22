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

#include <stdio.h>
#include <string.h>

/* ---- Assembler core: deterministic, host-independent --------------------- */

TEST(assemble, x86_intel_known_encoding) {
    /* mov rax, rdi; add rax, rsi; ret — the add_signed corpus routine. */
    asm_result_t r;
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                 "mov rax, rdi; add rax, rsi; ret",
                                 EMU_CODE_BASE, &r));
    static const uint8_t expect[] = {0x48, 0x89, 0xf8, /* mov rax, rdi */
                                     0x48, 0x01, 0xf0, /* add rax, rsi */
                                     0xc3};            /* ret          */
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

TEST(assemble, x86_nasm_masm_gas_match_intel) {
    /* The same routine through each x86 dialect must yield identical bytes.
     * NASM treats ';' as a comment, so its statements are newline-separated;
     * MASM (Intel-family) and GAS (AT&T-family) accept ';' as before. */
    asm_result_t intel, nasm, masm, gas;
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                 "mov rax, rdi; add rax, rsi; ret",
                                 EMU_CODE_BASE, &intel));
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_NASM,
                                 "mov rax, rdi\nadd rax, rsi\nret",
                                 EMU_CODE_BASE, &nasm));
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_MASM,
                                 "mov rax, rdi; add rax, rsi; ret",
                                 EMU_CODE_BASE, &masm));
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_GAS,
                                 "movq %rdi, %rax; addq %rsi, %rax; ret",
                                 EMU_CODE_BASE, &gas));
    ASSERT_EQ(nasm.len, intel.len);
    ASSERT_MEM_EQ(nasm.bytes, intel.bytes, intel.len);
    ASSERT_EQ(masm.len, intel.len);
    ASSERT_MEM_EQ(masm.bytes, intel.bytes, intel.len);
    ASSERT_EQ(gas.len, intel.len);
    ASSERT_MEM_EQ(gas.bytes, intel.bytes, intel.len);
    asmtest_asm_free(&intel);
    asmtest_asm_free(&nasm);
    asmtest_asm_free(&masm);
    asmtest_asm_free(&gas);
}

TEST(assemble, rejects_dropped_statement) {
    /* Keystone drops a statement it cannot parse, leaves ks_errno at
     * KS_ERR_OK, and returns success — so without the statement-count guard
     * this call returns ok=true with `mov rbx, #2` silently missing from the
     * machine code. A wrong answer is worse than a missing one: fail instead. */
    asm_result_t r;
    ASSERT_FALSE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                  "mov rax, 1\nmov rbx, #2\nmov rcx, 3\nret\n",
                                  EMU_CODE_BASE, &r));
    ASSERT_TRUE(r.bytes == NULL);
    ASSERT_TRUE(r.err[0] != '\0');
    ASSERT_TRUE(strstr(r.err, "skipped") != NULL);
    asmtest_asm_free(&r);
}

TEST(assemble, rejects_att_source_in_intel_syntax) {
    /* The reachable trigger: a user pastes AT&T text and does not name a
     * syntax, so every binding passes the ASM_SYNTAX_INTEL default. Keystone
     * drops the AT&T `movq` and returns just {0xc3}, ok=true — an assertion
     * would then run against a bare `ret` the caller never wrote. */
    asm_result_t r;
    ASSERT_FALSE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                  "movq $42, %rax\nret\n", EMU_CODE_BASE, &r));
    ASSERT_TRUE(r.bytes == NULL);
    ASSERT_TRUE(r.err[0] != '\0');
    /* The diagnostic must point at the syntax argument — the dominant cause. */
    ASSERT_TRUE(strstr(r.err, "syntax") != NULL);
    /* Naming the right syntax assembles the same source cleanly. */
    asm_result_t att;
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_ATT,
                                 "movq $42, %rax\nret\n", EMU_CODE_BASE, &att));
    ASSERT_EQ(att.len, 8);
    asmtest_asm_free(&att);
    asmtest_asm_free(&r);
}

TEST(assemble, guard_admits_valid_separator_edge_cases) {
    /* The guard's own risk is the opposite failure: rejecting valid code. Each
     * source below puts a ';' somewhere a naive splitter would treat as a
     * statement separator — inside a string literal, a character literal, and
     * each comment form — and all of them are ONE statement to Keystone. */
    static const char *ok_srcs[] = {
        ".ascii \"a;b\"\nret\n",         /* ';' inside a string literal    */
        ".ascii \"a\\\";b\"\nret\n",     /* ...after an escaped quote      */
        ".byte ';'\nret\n",              /* ';' inside a char literal      */
        "mov rax, 1 // a;b\nret\n",      /* ';' inside a // comment        */
        "mov rax, 1 /* a;b */\nret\n",   /* ...inside a block comment      */
        "mov rax, 1 /* a;\nb */\nret\n", /* ...spanning a newline        */
        "mov rax, 1 # a;b\nret\n",       /* ...inside a # comment        */
        "mov rax, 1;;ret\n",             /* empty statement between ';'  */
        "\tmov rax, 1  \n\tret\t\n",     /* tabs and trailing whitespace */
        "mov rax, 1\r\n\r\nret\r\n",     /* CRLF and blank lines         */
        "l1: mov rax, 1; jmp l1\n",      /* label sharing an insn's line */
    };
    for (size_t i = 0; i < sizeof ok_srcs / sizeof ok_srcs[0]; i++) {
        asm_result_t r;
        bool ok = asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL, ok_srcs[i],
                                   EMU_CODE_BASE, &r);
        if (!ok)
            printf("# false rejection on source %zu: %s\n", i, r.err);
        ASSERT_TRUE(ok);
        asmtest_asm_free(&r);
    }

    /* NASM's ';' is a comment, not a separator: a source-side counter that
     * split on it unconditionally would demand 3 statements here and reject
     * a source Keystone assembles as 1. */
    asm_result_t nasm;
    ASSERT_TRUE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_NASM,
                                 "mov rax, 1; mov rbx, 2; ret\n", EMU_CODE_BASE,
                                 &nasm));
    ASSERT_EQ(nasm.stat_count, 1);
    asmtest_asm_free(&nasm);
}

TEST(assemble, arm_hash_immediates_are_not_comments) {
    /* '#' introduces a comment on x86 but is the immediate prefix on ARM, so
     * the counter must not truncate an ARM line there — `mov x0, #1; mov x1,
     * #2; ret` is three statements, and treating '#' as a comment would count
     * one and miss any drop after it. Both directions are checked: the valid
     * source assembles, and a dropped statement in the same shape is caught. */
    asm_result_t r;
    ASSERT_TRUE(asmtest_assemble(ASM_ARM64, ASM_SYNTAX_INTEL,
                                 "mov x0, #1; mov x1, #2; ret", EMU_CODE_BASE,
                                 &r));
    ASSERT_EQ(r.stat_count, 3);
    ASSERT_EQ(r.len, 12);
    asmtest_asm_free(&r);

    /* $2 is x86 immediate syntax: Keystone drops it and reports 2 of 3. */
    ASSERT_FALSE(asmtest_assemble(ASM_ARM64, ASM_SYNTAX_INTEL,
                                  "mov x0, #1\nmov x1, $2\nret", EMU_CODE_BASE,
                                  &r));
    ASSERT_TRUE(strstr(r.err, "skipped") != NULL);
    asmtest_asm_free(&r);

    ASSERT_TRUE(asmtest_assemble(ASM_ARM32, ASM_SYNTAX_INTEL,
                                 "mov r0, #1; mov r1, #2; bx lr", EMU_CODE_BASE,
                                 &r));
    ASSERT_EQ(r.stat_count, 3);
    asmtest_asm_free(&r);
}

TEST(assemble, bad_mnemonic_reported_as_data) {
    asm_result_t r;
    ASSERT_FALSE(asmtest_assemble(ASM_X86_64, ASM_SYNTAX_INTEL,
                                  "frobnicate rax", EMU_CODE_BASE, &r));
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
    ASSERT_TRUE(
        emu_call_asm(e, "mov rax, rdi; add rax, rsi; ret", args, 2, 0, &out));
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

/* ---- Opaque-handle FFI shims (the surface the language bindings drive) ----- */

TEST(ffi_shim, call_asm6_syntax_and_args) {
    emu_t *e = emu_open();
    emu_result_t out;

    /* Intel, two args (the back-compat path), through the widened shim. */
    ASSERT_TRUE(asmtest_emu_call_asm6(e, "mov rax, rdi; add rax, rsi; ret",
                                      ASM_SYNTAX_INTEL, 40, 2, 0, 0, 0, 0, 2, 0,
                                      &out));
    ASSERT_EQ(out.regs.rax, 42);

    /* AT&T syntax + a third arg: rdi + rsi + rdx. */
    ASSERT_TRUE(asmtest_emu_call_asm6(
        e, "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
        ASM_SYNTAX_ATT, 10, 20, 12, 0, 0, 0, 3, 0, &out));
    ASSERT_EQ(out.regs.rax, 42);

    /* max_insns caps execution mid-routine: stop after the first mov (rax=rdi)
     * before the add runs, so rax is the first arg, not the sum. */
    ASSERT_TRUE(asmtest_emu_call_asm6(e, "mov rax, rdi; add rax, rsi; ret",
                                      ASM_SYNTAX_INTEL, 40, 2, 0, 0, 0, 0, 2, 1,
                                      &out));
    ASSERT_EQ(out.regs.rax, 40);
    emu_close(e);
}

TEST(ffi_shim, last_error_set_then_cleared) {
    emu_t *e = emu_open();
    emu_result_t out;

    /* A bad string returns 0 and leaves a non-empty thread-local diagnostic. */
    ASSERT_FALSE(asmtest_emu_call_asm6(e, "frobnicate rax", ASM_SYNTAX_INTEL, 0,
                                       0, 0, 0, 0, 0, 0, 0, &out));
    ASSERT_TRUE(asmtest_asm_last_error()[0] != '\0');

    /* A subsequent success clears it back to "". */
    ASSERT_TRUE(asmtest_emu_call_asm6(e, "mov rax, rdi; ret", ASM_SYNTAX_INTEL,
                                      7, 0, 0, 0, 0, 0, 1, 0, &out));
    ASSERT_EQ(out.regs.rax, 7);
    ASSERT_TRUE(asmtest_asm_last_error()[0] == '\0');
    emu_close(e);
}

TEST(ffi_shim, asm_bytes_multi_arch) {
    uint8_t buf[16];

    /* x86-64 add_signed bytes, matching the assembler-core test above. */
    static const uint8_t x86[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0xc3};
    int n = asmtest_asm_bytes(ASM_X86_64, ASM_SYNTAX_INTEL,
                              "mov rax, rdi; add rax, rsi; ret", EMU_CODE_BASE,
                              buf, (int)sizeof buf);
    ASSERT_EQ((size_t)n, sizeof x86);
    ASSERT_MEM_EQ(buf, x86, sizeof x86);

    /* AArch64 `ret` is C0 03 5F D6 — a guest the x86 emulator can't run, but the
     * assemble-only shim still produces its bytes. */
    static const uint8_t a64[] = {0xc0, 0x03, 0x5f, 0xd6};
    n = asmtest_asm_bytes(ASM_ARM64, ASM_SYNTAX_INTEL, "ret", EMU_CODE_BASE,
                          buf, (int)sizeof buf);
    ASSERT_EQ((size_t)n, sizeof a64);
    ASSERT_MEM_EQ(buf, a64, sizeof a64);

    /* An out-of-range arch is a reported failure (0), not a crash. */
    ASSERT_EQ(
        asmtest_asm_bytes(99, ASM_SYNTAX_INTEL, "ret", 0, buf, (int)sizeof buf),
        0);
    ASSERT_TRUE(asmtest_asm_last_error()[0] != '\0');
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
