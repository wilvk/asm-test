/*
 * test_emu.c — exercises the Unicorn-backed emulator tier (x86-64 guest).
 * Build/run via `make emu-test` (requires libunicorn).
 *
 * These use the same TEST/ASSERT framework; emu_call returns full CPU state in
 * an emu_result_t, so ordinary assertions inspect any register or fault.
 */
#include "asmtest.h"
#include "asmtest_emu.h"

#include <string.h>

extern long add_signed(long a, long b);
extern long sum_via_rbx(long a, long b);
extern long clobbers_rbx(long a, long b);
extern long load_long(void *p);
extern long classify(long x); /* -1/0/+1; three branch paths */
extern void fill_bytes(void *buf, long val, long n); /* a counted loop  */

/* 64 bytes comfortably covers each of these tiny routines; emulation stops at
 * the routine's own `ret`, so copying past the function end is harmless. */
#define CODE_WINDOW 64

/* The x86-64 `emu` suite drives the REAL example routines (add_signed,
 * classify, fill_bytes, ...). Those `extern` symbols are the HOST's compiled
 * machine code, which is valid x86-64 input for the engine only when the host
 * itself is x86-64; on another host (e.g. the aarch64 CI runner) the bytes are
 * a different ISA and the x86-64 engine decodes garbage. Skip those cases
 * off-x86-64. The hand-assembled-byte tests in this file (the FP/SIMD and
 * Win64 cases below, plus the AArch64 / RISC-V / ARM32 guests) are real machine
 * code regardless of host, so they keep running and the wrapper is still
 * exercised on every host. */
#if defined(__x86_64__)
#define REQUIRE_X86_HOST() ((void)0)
#else
#define REQUIRE_X86_HOST()                                                     \
    SKIP("x86-64 emu suite drives host-compiled routines; host is not x86-64")
#endif

static emu_t *E;

SETUP(emu) { E = emu_open(); }

TEARDOWN(emu) {
    emu_close(E);
    E = NULL;
}

TEST(emu, runs_routine_in_isolation) {
    REQUIRE_X86_HOST();
    ASSERT_TRUE(E != NULL);
    emu_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_REG_EQ(&r.regs, rax, 42);
}

TEST(emu, full_state_reveals_clobbered_callee_saved) {
    /* On real hardware ASM_CALLn can only check rbx was restored; the emulator
     * shows the value the routine left in it mid-flight. */
    REQUIRE_X86_HOST();
    emu_result_t r;
    long args[] = {3, 4};
    emu_call(E, (void *)clobbers_rbx, CODE_WINDOW, args, 2, 0, &r);
    ASSERT_EQ(r.regs.rax, 7);
    ASSERT_EQ(r.regs.rbx, 7); /* clobbers_rbx leaves a+b in rbx */
}

TEST(emu, compliant_routine_restores_rbx) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    long args[] = {3, 4};
    /* sum_via_rbx pushes rbx then pops it; rbx starts at 0, ends at 0. */
    emu_call(E, (void *)sum_via_rbx, CODE_WINDOW, args, 2, 0, &r);
    ASSERT_EQ(r.regs.rax, 7);
    ASSERT_EQ(r.regs.rbx, 0);
}

TEST(emu, mid_routine_single_step) {
    /* Run just the first instruction of add_signed (mov rax, rdi) and inspect
     * the intermediate state, before the `add`. */
    REQUIRE_X86_HOST();
    emu_result_t r;
    long args[] = {5, 9};
    emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, /*max_insns=*/1, &r);
    ASSERT_EQ(r.regs.rax, 5); /* rax = rdi; the add has not run yet */
}

TEST(emu, fault_injection_catches_bad_load) {
    /* load_long dereferences its pointer arg; aim it at unmapped memory. */
    REQUIRE_X86_HOST();
    emu_result_t r;
    long args[] = {(long)0xdead0000UL};
    bool ok = emu_call(E, (void *)load_long, CODE_WINDOW, args, 1, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(r.faulted);
    ASSERT_UEQ(r.fault_addr, 0xdead0000UL);
    ASSERT_EQ(r.fault_kind, EMU_FAULT_READ);
}

TEST(emu, reads_preloaded_guest_memory) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    uint64_t addr = 0x00300000UL;
    long value = 0x1234;
    ASSERT_TRUE(emu_map(E, addr, 0x1000));
    ASSERT_TRUE(emu_write(E, addr, &value, sizeof value));
    long args[] = {(long)addr};
    ASSERT_TRUE(emu_call(E, (void *)load_long, CODE_WINDOW, args, 1, 0, &r));
    ASSERT_FALSE(r.faulted);
    ASSERT_EQ(r.regs.rax, 0x1234);
}

/* -------------------------------------------------------------------------
 * Phase 10: instruction tracing & basic-block coverage. classify() has three
 * return paths; no single input walks them all, so the UNION of basic blocks
 * across inputs answers "did the tests exercise every branch?".
 * ------------------------------------------------------------------------- */
TEST(emu, trace_records_instruction_stream) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    uint64_t insns[32], blocks[16];
    emu_trace_t t = {0};
    t.insns = insns;
    t.insns_cap = 32;
    t.blocks = blocks;
    t.blocks_cap = 16;

    long args[] = {7};
    ASSERT_TRUE(
        emu_call_traced(E, (void *)classify, CODE_WINDOW, args, 1, 0, &r, &t));
    ASSERT_EQ(r.regs.rax, 1);
    ASSERT_TRUE(t.insns_total > 0);
    ASSERT_EQ(t.insns_len, t.insns_total); /* big buffer: nothing dropped */
    ASSERT_FALSE(t.truncated);
    ASSERT_UEQ(t.insns[0], 0); /* execution starts at the routine's entry */
    ASSERT_TRUE(t.blocks_len >= 1);
    ASSERT_UEQ(t.blocks[0], 0); /* first basic block is the entry block     */
}

TEST(emu, coverage_union_exceeds_single_input) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    uint64_t blocks[16];
    emu_trace_t one = {0};
    one.blocks = blocks;
    one.blocks_cap = 16;

    long pos[] = {7};
    emu_call_traced(E, (void *)classify, CODE_WINDOW, pos, 1, 0, &r, &one);
    size_t single = one.blocks_len; /* blocks the positive path reaches */
    ASSERT_TRUE(single >= 1);

    /* Accumulate all three paths into one fresh trace; the union is larger. */
    uint64_t ublocks[16];
    emu_trace_t all = {0};
    all.blocks = ublocks;
    all.blocks_cap = 16;
    long inputs[] = {-5, 0, 7};
    for (int i = 0; i < 3; i++) {
        long a[] = {inputs[i]};
        emu_call_traced(E, (void *)classify, CODE_WINDOW, a, 1, 0, &r, &all);
    }
    ASSERT_TRUE(all.blocks_len > single); /* extra branches now covered */
    ASSERT_UEQ(all.blocks[0], 0);
}

TEST(emu, loop_reenters_block_and_trace_truncates) {
    /* fill_bytes loops n times: the loop-body block is re-entered each pass
     * (blocks_total > distinct blocks), and a deliberately tiny insns buffer
     * truncates (insns_total > insns_len, truncated flag set). */
    REQUIRE_X86_HOST();
    uint64_t addr = 0x00300000UL;
    ASSERT_TRUE(emu_map(E, addr, 0x1000));
    emu_result_t r;
    uint64_t insns[2], blocks[16];
    emu_trace_t t = {0};
    t.insns = insns;
    t.insns_cap = 2;
    t.blocks = blocks;
    t.blocks_cap = 16;

    long args[] = {(long)addr, 0xAB, 5};
    ASSERT_TRUE(emu_call_traced(E, (void *)fill_bytes, CODE_WINDOW, args, 3, 0,
                                &r, &t));
    ASSERT_TRUE(t.truncated);
    ASSERT_EQ(t.insns_len, (size_t)2);
    ASSERT_TRUE(t.insns_total > t.insns_len);
    ASSERT_TRUE(t.blocks_total > t.blocks_len); /* loop re-entry */
}

/* -------------------------------------------------------------------------
 * Track C: FP / SIMD argument marshalling + capture, the emulator assertion
 * macros, and coverage reporting (x86-64 guest; reuses the `emu` suite's E).
 * The host toolchain emits System V calls, so the FP/SIMD routines are
 * hand-assembled bytes:
 *   addsd xmm0, xmm1 -> F2 0F 58 C1   ret -> C3   (scalar double add)
 *   paddd xmm0, xmm1 -> 66 0F FE C1   ret -> C3   (packed add of 4x int32)
 * ------------------------------------------------------------------------- */
TEST(emu, double_arg_returns_in_xmm0) {
    static const unsigned char ADDSD[] = {0xF2, 0x0F, 0x58, 0xC1, 0xc3};
    emu_result_t r;
    long iargs[1] = {0};
    double fargs[] = {1.5, 2.25}; /* xmm0, xmm1 */
    ASSERT_TRUE(emu_call_fp(E, ADDSD, sizeof ADDSD, iargs, 0, fargs, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    ASSERT_EMU_FP_EQ(&r,
                     3.75); /* result in xmm0.f64[0], no manual struct read */
}

TEST(emu, vector_arg_captures_xmm_file) {
    static const unsigned char PADDD[] = {0x66, 0x0F, 0xFE, 0xC1, 0xc3};
    emu_result_t r;
    emu_vec128_t a = {0}, b = {0};
    for (int i = 0; i < 4; i++) {
        a.u32[i] = (uint32_t)(i + 1);        /* 1 2 3 4   */
        b.u32[i] = (uint32_t)(10 * (i + 1)); /* 10 20 30 40 */
    }
    emu_vec128_t vargs[2] = {a, b};
    long iargs[1] = {0};
    ASSERT_TRUE(
        emu_call_vec(E, PADDD, sizeof PADDD, iargs, 0, vargs, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    emu_vec128_t expect = {0};
    for (int i = 0; i < 4; i++)
        expect.u32[i] = a.u32[i] + b.u32[i]; /* 11 22 33 44 */
    ASSERT_EMU_VEC_EQ(&r, 0, expect.u8);
    ASSERT_EQ(r.regs.xmm[0].u32[0], 11u);
    ASSERT_EQ(r.regs.xmm[0].u32[3], 44u);
}

TEST(emu, assertion_macros_reg_and_fault) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    ASSERT_EMU_REG_EQ(&r, rax, 42);
    /* A bad load faults at a known address and kind. */
    long bad[] = {(long)0xdead0000UL};
    emu_call(E, (void *)load_long, CODE_WINDOW, bad, 1, 0, &r);
    ASSERT_FAULT(&r);
    ASSERT_FAULT_AT(&r, EMU_FAULT_READ, 0xdead0000UL);
}

TEST(emu, coverage_report_lists_uncovered_blocks) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    /* universe = union of all three classify paths. */
    uint64_t ublocks[16];
    emu_trace_t universe = {0};
    universe.blocks = ublocks;
    universe.blocks_cap = 16;
    long inputs[] = {-5, 0, 7};
    for (int i = 0; i < 3; i++) {
        long a[] = {inputs[i]};
        emu_call_traced(E, (void *)classify, CODE_WINDOW, a, 1, 0, &r,
                        &universe);
    }
    /* covered = just the positive path. */
    uint64_t cblocks[16];
    emu_trace_t covered = {0};
    covered.blocks = cblocks;
    covered.blocks_cap = 16;
    long pos[] = {7};
    emu_call_traced(E, (void *)classify, CODE_WINDOW, pos, 1, 0, &r, &covered);

    /* The union exceeds any single input — expressed as one macro. */
    ASSERT_BLOCKS_AT_LEAST(&universe, covered.blocks_len + 1);
    ASSERT_BLOCK_COVERED(&universe, 0); /* entry block always reached */
    ASSERT_BLOCK_COVERED(&covered, 0);

    /* The report names the blocks a single input did not reach. */
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    size_t nmiss = emu_coverage_uncovered(&covered, &universe, f);
    ASSERT_TRUE(nmiss > 0);
    char buf[512] = {0};
    rewind(f);
    (void)!fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    ASSERT_TRUE(strstr(buf, "blocks covered") != NULL);
    ASSERT_TRUE(strstr(buf, "uncovered:") != NULL);
}

TEST(emu, lcov_export_emits_records) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    uint64_t blocks[16];
    emu_trace_t t = {0};
    t.blocks = blocks;
    t.blocks_cap = 16;
    long a[] = {7};
    emu_call_traced(E, (void *)classify, CODE_WINDOW, a, 1, 0, &r, &t);

    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    emu_trace_lcov(&t, "classify", f);
    char buf[512] = {0};
    rewind(f);
    (void)!fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    ASSERT_TRUE(strstr(buf, "SF:classify") != NULL);
    ASSERT_TRUE(strstr(buf, "DA:0,1") != NULL); /* entry block at offset 0 */
    ASSERT_TRUE(strstr(buf, "end_of_record") != NULL);
}

/* Source-line coverage mapping (Track C leftover): a caller-supplied
 * (offset -> line) table turns block-offset coverage into source-line coverage,
 * with hit AND missed lines reported. The map and trace are synthetic so the
 * asserted line numbers don't depend on any routine's codegen. */
TEST(emu, source_line_mapping) {
    /* offsets [0,4) -> line 10, [4,8) -> line 11, [8,..) -> line 12. */
    static const emu_line_entry_t rows[] = {{0, 10}, {4, 11}, {8, 12}};
    emu_line_map_t map = {rows, 3};

    /* lookup resolves an offset to the row whose range contains it; the last
     * row extends to the end. */
    ASSERT_EQ(emu_line_lookup(&map, 0)->line, 10u);
    ASSERT_EQ(emu_line_lookup(&map, 3)->line, 10u);
    ASSERT_EQ(emu_line_lookup(&map, 4)->line, 11u);
    ASSERT_EQ(emu_line_lookup(&map, 7)->line, 11u);
    ASSERT_EQ(emu_line_lookup(&map, 8)->line, 12u);
    ASSERT_EQ(emu_line_lookup(&map, 999)->line, 12u);
    ASSERT_TRUE(emu_line_lookup(NULL, 0) == NULL);

    /* An offset before the first row maps to nothing. */
    static const emu_line_entry_t rows2[] = {{4, 11}, {8, 12}};
    emu_line_map_t map2 = {rows2, 2};
    ASSERT_TRUE(emu_line_lookup(&map2, 0) == NULL);
    ASSERT_TRUE(emu_line_lookup(&map2, 3) == NULL);

    /* A covered trace that entered the blocks at offsets 0 and 8 (lines 10 and
     * 12); line 11 is never reached. */
    uint64_t cblocks[] = {0, 8};
    emu_trace_t covered = {0};
    covered.blocks = cblocks;
    covered.blocks_cap = 2;
    covered.blocks_len = 2;

    /* The report names the one uncovered line (11). */
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    size_t miss = emu_trace_source_report(&covered, &map, f);
    ASSERT_EQ(miss, (size_t)1);
    char buf[256] = {0};
    rewind(f);
    (void)!fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    ASSERT_TRUE(strstr(buf, "2/3 lines covered") != NULL);
    ASSERT_TRUE(strstr(buf, "uncovered lines: 11") != NULL);

    /* lcov_source emits one DA per source line — hit (1) AND missed (0). */
    FILE *g = tmpfile();
    ASSERT_TRUE(g != NULL);
    emu_trace_lcov_source(&covered, &map, "classify.s", g);
    char lbuf[256] = {0};
    rewind(g);
    (void)!fread(lbuf, 1, sizeof lbuf - 1, g);
    fclose(g);
    ASSERT_TRUE(strstr(lbuf, "SF:classify.s") != NULL);
    ASSERT_TRUE(strstr(lbuf, "DA:10,1") != NULL);
    ASSERT_TRUE(strstr(lbuf, "DA:11,0") != NULL); /* missed line surfaced */
    ASSERT_TRUE(strstr(lbuf, "DA:12,1") != NULL);
    ASSERT_TRUE(strstr(lbuf, "LF:3") != NULL);
    ASSERT_TRUE(strstr(lbuf, "LH:2") != NULL);
}

/* ---- Disassembly in diagnostics (Capstone, optional) --------------------
 * Hand-assembled x86-64 byte literals, so these run on every host (the x86-64
 * engine decodes them regardless of the host ISA) and exercise the disassembly
 * path in BOTH build configs: with Capstone the diagnostics carry instruction
 * text; without it they degrade to offsets — asserted by branching on
 * emu_disas_available(). */

TEST(emu, disas_decodes_known_instructions) {
    static const uint8_t code[] = {0x48, 0x31, 0xc0,
                                   0xc3}; /* xor rax,rax;ret */
    char buf[64];
    size_t n = emu_disas(EMU_ARCH_X86_64, code, sizeof code, EMU_CODE_BASE, 0,
                         buf, sizeof buf);
    if (emu_disas_available()) {
        ASSERT_UEQ(n, 3); /* xor rax, rax is 3 bytes */
        ASSERT_TRUE(strstr(buf, "xor") != NULL);
        n = emu_disas(EMU_ARCH_X86_64, code, sizeof code, EMU_CODE_BASE, 3, buf,
                      sizeof buf);
        ASSERT_UEQ(n, 1); /* ret */
        ASSERT_STREQ(buf, "ret");
    } else {
        ASSERT_UEQ(n, 0);      /* no Capstone -> no decode */
        ASSERT_STREQ(buf, ""); /* buf cleared so the caller falls back to off */
    }
}

TEST(emu, fault_describe_names_offending_instruction) {
    static const uint8_t code[] = {0x48, 0x8b, 0x07,
                                   0xc3}; /* mov rax,[rdi];ret */
    emu_result_t r;
    long args[] = {(long)0xdead0000UL}; /* unmapped pointer */
    emu_call_traced(E, code, sizeof code, args, 1, 0, &r, NULL);
    ASSERT_FAULT_AT(&r, EMU_FAULT_READ, 0xdead0000UL);

    char buf[160];
    emu_fault_describe(&r, EMU_ARCH_X86_64, code, sizeof code, EMU_CODE_BASE,
                       buf, sizeof buf);
    ASSERT_TRUE(strstr(buf, "read fault") != NULL);
    ASSERT_TRUE(strstr(buf, "0xdead0000") != NULL);
    ASSERT_TRUE(strstr(buf, "@0x0") != NULL); /* faulting insn at offset 0 */
    if (emu_disas_available())
        ASSERT_TRUE(strstr(buf, "mov") != NULL); /* names the load */
}

TEST(emu, disas_report_annotates_blocks) {
    /* Synthetic trace over known bytes: offsets 0 (xor) and 3 (ret). */
    static const uint8_t code[] = {0x48, 0x31, 0xc0, 0xc3};
    uint64_t offs[] = {0, 3};
    emu_trace_t t = {0};
    t.blocks = offs;
    t.blocks_cap = t.blocks_len = t.blocks_total = 2;
    t.insns = offs;
    t.insns_cap = t.insns_len = t.insns_total = 2;

    /* Coverage summary, annotated. */
    FILE *f = tmpfile();
    ASSERT_TRUE(f != NULL);
    emu_trace_report_disasm(&t, EMU_ARCH_X86_64, code, sizeof code, f);
    char buf[512] = {0};
    rewind(f);
    (void)!fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    ASSERT_TRUE(strstr(buf, "distinct blocks") != NULL);
    ASSERT_TRUE(strstr(buf, "0x3") != NULL); /* the block offset */
    if (emu_disas_available())
        ASSERT_TRUE(strstr(buf, "ret") != NULL); /* annotated instruction */

    /* Ordered instruction trace, annotated. */
    FILE *g = tmpfile();
    ASSERT_TRUE(g != NULL);
    emu_trace_disasm(&t, EMU_ARCH_X86_64, code, sizeof code, g);
    char tbuf[512] = {0};
    rewind(g);
    (void)!fread(tbuf, 1, sizeof tbuf - 1, g);
    fclose(g);
    ASSERT_TRUE(strstr(tbuf, "trace: 2 instructions") != NULL);
    if (emu_disas_available())
        ASSERT_TRUE(strstr(tbuf, "xor") != NULL); /* first insn annotated */
}

/* ---- Mid-execution guards (Track F) -------------------------------------
 * Hand-assembled x86-64, host-independent. The guarded writes land in MAPPED
 * memory, so they do not fault — the watchpoint catches a *logical* violation
 * that ABI-boundary or guard-page checks cannot see, and names the store. */

/* mov [rdi], rax ; mov [rdi+0x800], rax ; ret */
static const uint8_t TWO_WRITES[] = {0x48, 0x89, 0x07, 0x48, 0x89, 0x87,
                                     0x00, 0x08, 0x00, 0x00, 0xc3};
#define WATCH_BASE 0x400000UL

TEST(emu, watchpoint_only_flags_escaping_write) {
    ASSERT_TRUE(emu_map(E, WATCH_BASE, 0x1000)); /* both writes land in it */
    emu_result_t r;
    long args[] = {(long)WATCH_BASE};

    emu_watch_t w;
    emu_watch_writes(E, WATCH_BASE, 8, EMU_WATCH_ONLY,
                     &w); /* confine to [base,+8) */
    emu_call(E, TWO_WRITES, sizeof TWO_WRITES, args, 1, 0, &r);
    emu_watch_clear(E);

    ASSERT_NO_FAULT(&r);        /* region is mapped: no fault... */
    ASSERT_WRITE_VIOLATION(&w); /* ...but the 2nd store escaped [base,+8) */
    ASSERT_UEQ(w.addr, WATCH_BASE + 0x800);
    ASSERT_UEQ(w.rip_off, 3); /* offset of the escaping store */

    char buf[160];
    emu_watch_describe(&w, EMU_ARCH_X86_64, TWO_WRITES, sizeof TWO_WRITES,
                       EMU_CODE_BASE, buf, sizeof buf);
    ASSERT_TRUE(strstr(buf, "0x400800") != NULL);
    if (emu_disas_available())
        ASSERT_TRUE(strstr(buf, "mov") != NULL); /* names the store */
}

TEST(emu, watchpoint_never_flags_forbidden_zone_then_clears) {
    ASSERT_TRUE(emu_map(E, WATCH_BASE, 0x1000));
    emu_result_t r;
    long args[] = {(long)WATCH_BASE};

    /* NEVER: writing into the guard band at base+0x800 is forbidden. */
    emu_watch_t w;
    emu_watch_writes(E, WATCH_BASE + 0x800, 8, EMU_WATCH_NEVER, &w);
    emu_call(E, TWO_WRITES, sizeof TWO_WRITES, args, 1, 0, &r);
    ASSERT_WRITE_VIOLATION(&w);
    ASSERT_UEQ(w.addr, WATCH_BASE + 0x800);

    /* Cleared, then re-armed over the whole region: nothing escapes. */
    emu_watch_clear(E);
    emu_watch_t w2;
    emu_watch_writes(E, WATCH_BASE, 0x1000, EMU_WATCH_ONLY, &w2);
    emu_call(E, TWO_WRITES, sizeof TWO_WRITES, args, 1, 0, &r);
    emu_watch_clear(E);
    ASSERT_NO_WRITE_VIOLATION(&w2);
}

/* mov rbx,0x99 ; jmp +0 ; ret — clobbers rbx, then the jump target (a fresh
 * basic block, the ret) sees the clobbered value. A return-time ABI check could
 * miss this; the block-entry invariant does not. */
static const uint8_t CLOBBER_RBX[] = {0x48, 0xc7, 0xc3, 0x99, 0x00,
                                      0x00, 0x00, 0xeb, 0x00, 0xc3};

TEST(emu, reg_invariant_catches_clobber_and_holds) {
    emu_result_t r;
    long args[] = {0};

    /* rbx starts at 0 (the engine zeros the register file at the start of every
     * call), so a routine that never touches rbx keeps the invariant across both
     * of its blocks — order-independent of the clobber test below. */
    static const uint8_t XOR_JMP_RET[] = {0x31, 0xc0, 0xeb, 0x00, 0xc3};
    emu_reg_guard_t g2;
    ASSERT_TRUE(emu_guard_reg(E, "rbx", 0, &g2));
    emu_call(E, XOR_JMP_RET, sizeof XOR_JMP_RET, args, 0, 0, &r);
    emu_guard_reg_clear(E);
    ASSERT_REG_INVARIANT(&g2);

    /* rbx must stay 0 — broken at the ret's block entry (rbx == 0x99 there),
     * which a return-time ABI check, seeing rbx restored or not, can miss. */
    emu_reg_guard_t g;
    ASSERT_TRUE(emu_guard_reg(E, "rbx", 0, &g));
    emu_call(E, CLOBBER_RBX, sizeof CLOBBER_RBX, args, 0, 0, &r);
    emu_guard_reg_clear(E);
    ASSERT_TRUE(g.violated);
    ASSERT_UEQ(g.got, 0x99);
    ASSERT_UEQ(g.rip_off, 9); /* the ret's block, after the clobber */

    /* An unknown register name is rejected. */
    emu_reg_guard_t g3;
    ASSERT_FALSE(emu_guard_reg(E, "nope", 0, &g3));
}

/* mov rax, rbx ; ret — returns whatever rbx held at entry. rbx is callee-saved,
 * so the ABI never sets it from arguments: a routine reading it needs a preload. */
static const uint8_t MOV_RAX_RBX[] = {0x48, 0x89, 0xd8, 0xc3};

TEST(emu, preloaded_register_is_visible_to_the_routine) {
    REQUIRE_X86_HOST();
    emu_result_t r;
    long noargs[1] = {0};
    ASSERT_TRUE(emu_set_reg(E, "rbx", 0x1234));
    ASSERT_TRUE(emu_call(E, MOV_RAX_RBX, sizeof MOV_RAX_RBX, noargs, 0, 0, &r));
    ASSERT_UEQ(r.regs.rax, 0x1234); /* the preload reached the routine */
    /* Re-arming updates it; clearing returns to the deterministic zero. */
    emu_set_reg(E, "rbx", 0x99);
    emu_call(E, MOV_RAX_RBX, sizeof MOV_RAX_RBX, noargs, 0, 0, &r);
    ASSERT_UEQ(r.regs.rax, 0x99);
    emu_clear_regs(E);
    emu_call(E, MOV_RAX_RBX, sizeof MOV_RAX_RBX, noargs, 0, 0, &r);
    ASSERT_UEQ(r.regs.rax, 0);
    ASSERT_FALSE(emu_set_reg(E, "nope", 1)); /* unknown name rejected */
}

TEST(emu, read_watchpoint_flags_a_forbidden_read) {
    REQUIRE_X86_HOST();
    ASSERT_TRUE(emu_map(E, WATCH_BASE, 0x1000));
    emu_result_t r;
    long args[] = {(long)WATCH_BASE};
    /* NEVER-read the low 8 bytes; load_long reads exactly [rdi]. */
    emu_watch_t w;
    emu_watch_reads(E, WATCH_BASE, 8, EMU_WATCH_NEVER, &w);
    emu_call(E, (void *)load_long, CODE_WINDOW, args, 1, 0, &r);
    emu_watch_clear(E);
    ASSERT_NO_FAULT(&r);        /* mapped: the read itself succeeds... */
    ASSERT_WRITE_VIOLATION(&w); /* ...but the read watch flagged it */
    ASSERT_UEQ(w.addr, WATCH_BASE);
    /* A write watch would miss it — load_long only reads. */
    emu_watch_t w2;
    emu_watch_writes(E, WATCH_BASE, 8, EMU_WATCH_NEVER, &w2);
    emu_call(E, (void *)load_long, CODE_WINDOW, args, 1, 0, &r);
    emu_watch_clear(E);
    ASSERT_NO_WRITE_VIOLATION(&w2);
}

/* ---- Coverage-guided fuzzing & mutation testing (Track E) ---------------
 * A hand-assembled classify(x) -> {-1,0,+1} with three branch paths, run as
 * raw x86-64 bytes (host-independent). Three blocks/paths: negative (the js
 * target at 0x12), zero (the jz target at 0x11), positive (fall-through). */
static const uint8_t CLASSIFY3[] = {
    0x31, 0xc0,                   /* 0x00  xor eax, eax       */
    0x48, 0x85, 0xff,             /* 0x02  test rdi, rdi      */
    0x78, 0x0b,                   /* 0x05  js   0x12 (neg)    */
    0x48, 0x85, 0xff,             /* 0x07  test rdi, rdi      */
    0x74, 0x05,                   /* 0x0a  jz   0x11 (zero)   */
    0xb8, 0x01, 0x00, 0x00, 0x00, /* 0x0c  mov eax, 1  (pos)  */
    0xc3,                         /* 0x11  ret                */
    0xb8, 0xff, 0xff, 0xff, 0xff, /* 0x12  mov eax, -1 (neg)  */
    0xc3,                         /* 0x17  ret                */
};

TEST(emu, fuzz_coverage_beats_fixed_vector) {
    emu_result_t r;
    /* A single fixed vector (x = 5, the positive path) reaches some blocks. */
    uint64_t fb[16];
    emu_trace_t fixed = {0};
    fixed.blocks = fb;
    fixed.blocks_cap = 16;
    long pos[] = {5};
    emu_call_traced(E, CLASSIFY3, sizeof CLASSIFY3, pos, 1, 0, &r, &fixed);

    /* A coverage-guided search over [-50,50] reaches strictly more (it finds
     * the negative path a single positive vector never executes). */
    uint64_t gb[16];
    emu_trace_t uni = {0};
    uni.blocks = gb;
    uni.blocks_cap = 16;
    emu_fuzz_stat_t st;
    ASSERT_TRUE(emu_fuzz_cover1(E, CLASSIFY3, sizeof CLASSIFY3, -50, 50, 2000,
                                0xC0FFEEULL, &uni, &st));
    ASSERT_UEQ(st.blocks_reached, uni.blocks_len);
    ASSERT_TRUE(uni.blocks_len > fixed.blocks_len); /* guided > fixed */
    ASSERT_TRUE(st.corpus_len >= 2); /* several coverage-expanding inputs */
}

TEST(emu, fuzz_corpus_is_retrievable_and_feeds_mutation) {
    REQUIRE_X86_HOST();
    uint64_t blocks[16];
    emu_trace_t uni = {0};
    uni.blocks = blocks;
    uni.blocks_cap = 16;
    emu_fuzz_stat_t st;
    ASSERT_TRUE(emu_fuzz_cover1(E, CLASSIFY3, sizeof CLASSIFY3, -50, 50, 2000,
                                0xC0FFEEULL, &uni, &st));
    /* The kept inputs are now readable off the handle (were discarded before). */
    const long *corpus = emu_fuzz_corpus(E);
    size_t n = emu_fuzz_corpus_len(E);
    ASSERT_TRUE(n >= 2);
    ASSERT_TRUE(corpus != NULL);
    ASSERT_UEQ((uint64_t)n, (uint64_t)st.corpus_len); /* handle vs stat agree */
    /* Feed the corpus straight into mutation testing — its inputs are exactly
     * this set — and confirm it distinguishes at least some mutants. */
    emu_mutation_stat_t ms;
    emu_mutation_test1(E, CLASSIFY3, sizeof CLASSIFY3, corpus, n, 0, 0xABCDULL,
                       &ms);
    ASSERT_TRUE(ms.killed + ms.survived > 0);
}

TEST(emu, mutation_strong_suite_kills_more) {
    /* A weak suite (only the positive path) lets mutations to the other paths
     * survive; a strong suite that covers all three kills more of them. The
     * same full single-bit-flip mutant set is run against both. */
    long weak[] = {5};
    long strong[] = {-7, 0, 9};
    emu_mutation_stat_t ws, ss;
    size_t weak_surv = emu_mutation_test1(E, CLASSIFY3, sizeof CLASSIFY3, weak,
                                          1, 0, 0xABCDULL, &ws);
    size_t strong_surv = emu_mutation_test1(E, CLASSIFY3, sizeof CLASSIFY3,
                                            strong, 3, 0, 0xABCDULL, &ss);

    ASSERT_UEQ(ws.mutants, ss.mutants);   /* identical mutant set */
    ASSERT_TRUE(weak_surv > 0);           /* the weak suite misses some */
    ASSERT_TRUE(strong_surv < weak_surv); /* the strong suite kills more */
    ASSERT_TRUE(ss.killed > ws.killed);
}

/* -------------------------------------------------------------------------
 * AArch64 guest: raw machine code run on whatever host this is (Unicorn
 * emulates AArch64 even on an x86-64 host). Bytes assembled from:
 *   add x0, x0, x1  -> 8b010000      ldr x0, [x0] -> f9400000
 *   add x0, x0, x2  -> 8b020000      ret          -> d65f03c0
 * stored little-endian.
 * ------------------------------------------------------------------------- */
static const unsigned char A64_ADD[] = {0x00, 0x00, 0x01, 0x8b,
                                        0xc0, 0x03, 0x5f, 0xd6};
static const unsigned char A64_ADD3[] = {0x00, 0x00, 0x01, 0x8b, 0x00, 0x00,
                                         0x02, 0x8b, 0xc0, 0x03, 0x5f, 0xd6};
static const unsigned char A64_LOAD[] = {0x00, 0x00, 0x40, 0xf9,
                                         0xc0, 0x03, 0x5f, 0xd6};

TEST(emu_arm64, runs_routine_in_isolation) {
    emu_arm64_t *e = emu_arm64_open();
    ASSERT_TRUE(e != NULL);
    emu_arm64_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_arm64_call(e, A64_ADD, sizeof A64_ADD, args, 2, 0, &r));
    ASSERT_REG_EQ(&r.regs, x[0], 42);
    emu_arm64_close(e);
}

TEST(emu_arm64, mid_routine_single_step) {
    emu_arm64_t *e = emu_arm64_open();
    emu_arm64_result_t r;
    long args[] = {1, 2, 100};
    /* one instruction only: x0 = x0 + x1 = 3; the +x2 has not run yet. */
    emu_arm64_call(e, A64_ADD3, sizeof A64_ADD3, args, 3, 1, &r);
    ASSERT_EQ(r.regs.x[0], 3);
    emu_arm64_close(e);
}

TEST(emu_arm64, fault_injection_catches_bad_load) {
    emu_arm64_t *e = emu_arm64_open();
    emu_arm64_result_t r;
    long args[] = {(long)0xdead0000UL};
    bool ok = emu_arm64_call(e, A64_LOAD, sizeof A64_LOAD, args, 1, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(r.faulted);
    ASSERT_UEQ(r.fault_addr, 0xdead0000UL);
    ASSERT_EQ(r.fault_kind, EMU_FAULT_READ);
    emu_arm64_close(e);
}

TEST(emu_arm64, reads_preloaded_guest_memory) {
    emu_arm64_t *e = emu_arm64_open();
    emu_arm64_result_t r;
    uint64_t addr = 0x00300000UL;
    long value = 0x4321;
    ASSERT_TRUE(emu_arm64_map(e, addr, 0x1000));
    ASSERT_TRUE(emu_arm64_write(e, addr, &value, sizeof value));
    long args[] = {(long)addr};
    ASSERT_TRUE(emu_arm64_call(e, A64_LOAD, sizeof A64_LOAD, args, 1, 0, &r));
    ASSERT_FALSE(r.faulted);
    ASSERT_EQ(r.regs.x[0], 0x4321);
    emu_arm64_close(e);
}

TEST(emu_arm64, trace_records_instruction_stream) {
    emu_arm64_t *e = emu_arm64_open();
    emu_arm64_result_t r;
    uint64_t insns[8], blocks[8];
    emu_trace_t t = {0};
    t.insns = insns;
    t.insns_cap = 8;
    t.blocks = blocks;
    t.blocks_cap = 8;

    long args[] = {1, 2, 100};
    ASSERT_TRUE(emu_arm64_call_traced(e, A64_ADD3, sizeof A64_ADD3, args, 3, 0,
                                      &r, &t));
    ASSERT_EQ(r.regs.x[0], 103); /* (1 + 2) + 100 */
    /* add, add, ret — straight line: 3 instructions at offsets 0/4/8, one
     * basic block (4-byte fixed-width instructions make offsets exact). */
    ASSERT_EQ(t.insns_total, (uint64_t)3);
    ASSERT_EQ(t.insns_len, (size_t)3);
    ASSERT_UEQ(t.insns[0], 0);
    ASSERT_UEQ(t.insns[1], 4);
    ASSERT_UEQ(t.insns[2], 8);
    ASSERT_EQ(t.blocks_total, (uint64_t)1);
    ASSERT_EQ(t.blocks_len, (size_t)1);
    ASSERT_UEQ(t.blocks[0], 0);
    emu_arm64_close(e);
}

/* AArch64 FP/SIMD (Track C follow-up). Bytes (little-endian):
 *   fadd d0, d0, d1        -> 1E612800   ret -> D65F03C0
 *   add  v0.4s, v0.4s, v1.4s -> 4EA18400 ret -> D65F03C0 (packed add of 4x s32)
 */
TEST(emu_arm64, double_arg_returns_in_d0) {
    static const unsigned char FADD[] = {0x00, 0x28, 0x61, 0x1E,
                                         0xC0, 0x03, 0x5F, 0xD6};
    emu_arm64_t *e = emu_arm64_open();
    emu_arm64_result_t r;
    long iargs[1] = {0};
    double fargs[] = {1.5, 2.25}; /* d0, d1 */
    ASSERT_TRUE(
        emu_arm64_call_fp(e, FADD, sizeof FADD, iargs, 0, fargs, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    ASSERT_DEQ(r.regs.v[0].f64[0], 3.75); /* double return in d0 = v[0] */
    emu_arm64_close(e);
}

TEST(emu_arm64, vector_arg_captures_v_file) {
    static const unsigned char VADD4S[] = {0x00, 0x84, 0xA1, 0x4E,
                                           0xC0, 0x03, 0x5F, 0xD6};
    emu_arm64_t *e = emu_arm64_open();
    emu_arm64_result_t r;
    emu_vec128_t a = {0}, b = {0};
    for (int i = 0; i < 4; i++) {
        a.u32[i] = (uint32_t)(i + 1);
        b.u32[i] = (uint32_t)(10 * (i + 1));
    }
    emu_vec128_t vargs[2] = {a, b};
    long iargs[1] = {0};
    ASSERT_TRUE(emu_arm64_call_vec(e, VADD4S, sizeof VADD4S, iargs, 0, vargs, 2,
                                   0, &r));
    ASSERT_NO_FAULT(&r);
    emu_vec128_t expect = {0};
    for (int i = 0; i < 4; i++)
        expect.u32[i] = a.u32[i] + b.u32[i]; /* 11 22 33 44 */
    ASSERT_EMU_VEC128_EQ(&r.regs.v[0], expect.u8);
    ASSERT_EQ(r.regs.v[0].u32[3], 44u);
    emu_arm64_close(e);
}

/* -------------------------------------------------------------------------
 * RISC-V (RV64) guest: raw machine code run on whatever host this is (Unicorn
 * emulates RISC-V even on an x86-64 host). Integer args arrive in a0..a7
 * (x10..x17); the return value is in a0 (x10). Bytes (little-endian) from:
 *   add  a0, a0, a1  -> 00b50533      ld   a0, 0(a0) -> 00053503
 *   add  a0, a0, a2  -> 00c50533      ret            -> 00008067
 * ------------------------------------------------------------------------- */
static const unsigned char RV_ADD[] = {0x33, 0x05, 0xb5, 0x00,
                                       0x67, 0x80, 0x00, 0x00};
static const unsigned char RV_ADD3[] = {0x33, 0x05, 0xb5, 0x00, 0x33, 0x05,
                                        0xc5, 0x00, 0x67, 0x80, 0x00, 0x00};
static const unsigned char RV_LOAD[] = {0x03, 0x35, 0x05, 0x00,
                                        0x67, 0x80, 0x00, 0x00};

TEST(emu_riscv, runs_routine_in_isolation) {
    emu_riscv_t *e = emu_riscv_open();
    ASSERT_TRUE(e != NULL);
    emu_riscv_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_riscv_call(e, RV_ADD, sizeof RV_ADD, args, 2, 0, &r));
    ASSERT_REG_EQ(&r.regs, x[10], 42); /* a0 == x10 holds the return value */
    emu_riscv_close(e);
}

TEST(emu_riscv, mid_routine_single_step) {
    emu_riscv_t *e = emu_riscv_open();
    emu_riscv_result_t r;
    long args[] = {1, 2, 100};
    /* one instruction only: a0 = a0 + a1 = 3; the +a2 has not run yet. */
    emu_riscv_call(e, RV_ADD3, sizeof RV_ADD3, args, 3, 1, &r);
    ASSERT_EQ(r.regs.x[10], 3);
    emu_riscv_close(e);
}

TEST(emu_riscv, fault_injection_catches_bad_load) {
    emu_riscv_t *e = emu_riscv_open();
    emu_riscv_result_t r;
    long args[] = {(long)0xdead0000UL};
    bool ok = emu_riscv_call(e, RV_LOAD, sizeof RV_LOAD, args, 1, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(r.faulted);
    ASSERT_UEQ(r.fault_addr, 0xdead0000UL);
    ASSERT_EQ(r.fault_kind, EMU_FAULT_READ);
    emu_riscv_close(e);
}

TEST(emu_riscv, reads_preloaded_guest_memory) {
    emu_riscv_t *e = emu_riscv_open();
    emu_riscv_result_t r;
    uint64_t addr = 0x00300000UL;
    long value = 0x5678;
    ASSERT_TRUE(emu_riscv_map(e, addr, 0x1000));
    ASSERT_TRUE(emu_riscv_write(e, addr, &value, sizeof value));
    long args[] = {(long)addr};
    ASSERT_TRUE(emu_riscv_call(e, RV_LOAD, sizeof RV_LOAD, args, 1, 0, &r));
    ASSERT_FALSE(r.faulted);
    ASSERT_EQ(r.regs.x[10], 0x5678);
    emu_riscv_close(e);
}

TEST(emu_riscv, trace_records_instruction_stream) {
    emu_riscv_t *e = emu_riscv_open();
    emu_riscv_result_t r;
    uint64_t insns[8], blocks[8];
    emu_trace_t t = {0};
    t.insns = insns;
    t.insns_cap = 8;
    t.blocks = blocks;
    t.blocks_cap = 8;

    long args[] = {1, 2, 100};
    ASSERT_TRUE(
        emu_riscv_call_traced(e, RV_ADD3, sizeof RV_ADD3, args, 3, 0, &r, &t));
    ASSERT_EQ(r.regs.x[10], 103); /* (1 + 2) + 100 */
    /* add, add, ret — straight line: 3 instructions at offsets 0/4/8, one
     * basic block (base RV64 instructions are 4 bytes, so offsets are exact). */
    ASSERT_EQ(t.insns_total, (uint64_t)3);
    ASSERT_EQ(t.insns_len, (size_t)3);
    ASSERT_UEQ(t.insns[0], 0);
    ASSERT_UEQ(t.insns[1], 4);
    ASSERT_UEQ(t.insns[2], 8);
    ASSERT_EQ(t.blocks_total, (uint64_t)1);
    ASSERT_EQ(t.blocks_len, (size_t)1);
    ASSERT_UEQ(t.blocks[0], 0);
    emu_riscv_close(e);
}

/* RISC-V D-extension FP (Track C follow-up). The FP unit is enabled at
 * emu_riscv_open (mstatus.FS). Bytes (little-endian):
 *   fadd.d fa0, fa0, fa1 -> 02B50553   ret -> 00008067
 */
TEST(emu_riscv, double_arg_returns_in_fa0) {
    static const unsigned char FADDD[] = {0x53, 0x05, 0xB5, 0x02,
                                          0x67, 0x80, 0x00, 0x00};
    emu_riscv_t *e = emu_riscv_open();
    emu_riscv_result_t r;
    long iargs[1] = {0};
    double fargs[] = {1.5, 2.25}; /* fa0, fa1 */
    ASSERT_TRUE(
        emu_riscv_call_fp(e, FADDD, sizeof FADDD, iargs, 0, fargs, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    ASSERT_DEQ(r.regs.f[10].f64[0], 3.75); /* fa0 == f10 */
    emu_riscv_close(e);
}

/* -------------------------------------------------------------------------
 * ARM32 (A32) guest: raw machine code run on whatever host this is (Unicorn
 * emulates ARM32 even on an x86-64 host). Integer args arrive in r0..r3; the
 * return value is in r0. Bytes (little-endian) from:
 *   add  r0, r0, r1  -> e0800001      ldr  r0, [r0]  -> e5900000
 *   add  r0, r0, r2  -> e0800002      bx   lr        -> e12fff1e
 * ------------------------------------------------------------------------- */
static const unsigned char ARM_ADD[] = {0x01, 0x00, 0x80, 0xe0,
                                        0x1e, 0xff, 0x2f, 0xe1};
static const unsigned char ARM_ADD3[] = {0x01, 0x00, 0x80, 0xe0, 0x02, 0x00,
                                         0x80, 0xe0, 0x1e, 0xff, 0x2f, 0xe1};
static const unsigned char ARM_LOAD[] = {0x00, 0x00, 0x90, 0xe5,
                                         0x1e, 0xff, 0x2f, 0xe1};

TEST(emu_arm, runs_routine_in_isolation) {
    emu_arm_t *e = emu_arm_open();
    ASSERT_TRUE(e != NULL);
    emu_arm_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_arm_call(e, ARM_ADD, sizeof ARM_ADD, args, 2, 0, &r));
    ASSERT_REG_EQ(&r.regs, r[0], 42); /* r0 holds the return value */
    emu_arm_close(e);
}

TEST(emu_arm, mid_routine_single_step) {
    emu_arm_t *e = emu_arm_open();
    emu_arm_result_t r;
    long args[] = {1, 2, 100};
    /* one instruction only: r0 = r0 + r1 = 3; the +r2 has not run yet. */
    emu_arm_call(e, ARM_ADD3, sizeof ARM_ADD3, args, 3, 1, &r);
    ASSERT_EQ(r.regs.r[0], 3);
    emu_arm_close(e);
}

TEST(emu_arm, fault_injection_catches_bad_load) {
    emu_arm_t *e = emu_arm_open();
    emu_arm_result_t r;
    long args[] = {(long)0xdead0000UL};
    bool ok = emu_arm_call(e, ARM_LOAD, sizeof ARM_LOAD, args, 1, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(r.faulted);
    ASSERT_UEQ(r.fault_addr, 0xdead0000UL);
    ASSERT_EQ(r.fault_kind, EMU_FAULT_READ);
    emu_arm_close(e);
}

TEST(emu_arm, reads_preloaded_guest_memory) {
    emu_arm_t *e = emu_arm_open();
    emu_arm_result_t r;
    uint64_t addr = 0x00300000UL;
    uint32_t value = 0x2468;
    ASSERT_TRUE(emu_arm_map(e, addr, 0x1000));
    ASSERT_TRUE(emu_arm_write(e, addr, &value, sizeof value));
    long args[] = {(long)addr};
    ASSERT_TRUE(emu_arm_call(e, ARM_LOAD, sizeof ARM_LOAD, args, 1, 0, &r));
    ASSERT_FALSE(r.faulted);
    ASSERT_EQ(r.regs.r[0], 0x2468);
    emu_arm_close(e);
}

TEST(emu_arm, trace_records_instruction_stream) {
    emu_arm_t *e = emu_arm_open();
    emu_arm_result_t r;
    uint64_t insns[8], blocks[8];
    emu_trace_t t = {0};
    t.insns = insns;
    t.insns_cap = 8;
    t.blocks = blocks;
    t.blocks_cap = 8;

    long args[] = {1, 2, 100};
    ASSERT_TRUE(
        emu_arm_call_traced(e, ARM_ADD3, sizeof ARM_ADD3, args, 3, 0, &r, &t));
    ASSERT_EQ(r.regs.r[0], 103); /* (1 + 2) + 100 */
    /* add, add, bx lr — straight line: 3 instructions at offsets 0/4/8, one
     * basic block (A32 instructions are 4 bytes, so offsets are exact). */
    ASSERT_EQ(t.insns_total, (uint64_t)3);
    ASSERT_EQ(t.insns_len, (size_t)3);
    ASSERT_UEQ(t.insns[0], 0);
    ASSERT_UEQ(t.insns[1], 4);
    ASSERT_UEQ(t.insns[2], 8);
    ASSERT_EQ(t.blocks_total, (uint64_t)1);
    ASSERT_EQ(t.blocks_len, (size_t)1);
    ASSERT_UEQ(t.blocks[0], 0);
    emu_arm_close(e);
}

/* ARM32 VFP (Track C follow-up). The VFP unit is enabled at emu_arm_open
 * (CPACR + FPEXC). Bytes (little-endian):
 *   vadd.f64 d0, d0, d1 -> EE300B01   bx lr -> E12FFF1E
 */
TEST(emu_arm, double_arg_returns_in_d0) {
    static const unsigned char VADDF64[] = {0x01, 0x0B, 0x30, 0xEE,
                                            0x1E, 0xFF, 0x2F, 0xE1};
    emu_arm_t *e = emu_arm_open();
    emu_arm_result_t r;
    long iargs[1] = {0};
    double fargs[] = {1.5, 2.25}; /* d0, d1 */
    ASSERT_TRUE(
        emu_arm_call_fp(e, VADDF64, sizeof VADDF64, iargs, 0, fargs, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    ASSERT_DEQ(r.regs.q[0].f64[0], 3.75); /* d0 == q[0].f64[0] */
    emu_arm_close(e);
}

/* ARM32 NEON vector args (Track C leftover): brings ARM32 to x86/AArch64 vector
 * parity. vadd.i32 q0, q0, q1 adds four packed s32 lanes; bytes (little-endian):
 *   vadd.i32 q0, q0, q1 -> F2200842   bx lr -> E12FFF1E
 * Vector args land in q0..q3 (AAPCS-VFP); the whole q0..q15 file is captured. */
TEST(emu_arm, vector_arg_captures_q_file) {
    static const unsigned char VADD4S[] = {0x42, 0x08, 0x20, 0xF2,
                                           0x1E, 0xFF, 0x2F, 0xE1};
    emu_arm_t *e = emu_arm_open();
    emu_arm_result_t r;
    emu_vec128_t a = {0}, b = {0};
    for (int i = 0; i < 4; i++) {
        a.u32[i] = (uint32_t)(i + 1);
        b.u32[i] = (uint32_t)(10 * (i + 1));
    }
    emu_vec128_t vargs[2] = {a, b};
    long iargs[1] = {0};
    ASSERT_TRUE(
        emu_arm_call_vec(e, VADD4S, sizeof VADD4S, iargs, 0, vargs, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    emu_vec128_t expect = {0};
    for (int i = 0; i < 4; i++)
        expect.u32[i] = a.u32[i] + b.u32[i]; /* 11 22 33 44 */
    ASSERT_EMU_VEC128_EQ(&r.regs.q[0], expect.u8);
    ASSERT_EQ(r.regs.q[0].u32[3], 44u);
    emu_arm_close(e);
}

/* -------------------------------------------------------------------------
 * Windows x64 ("Win64") calling convention on the x86-64 emulator engine.
 * Same guest CPU as the `emu` suite, but emu_call_win64 marshals args per the
 * Microsoft x64 ABI: integer args in rcx, rdx, r8, r9, then on the stack above
 * 32 bytes of shadow space; return in rax. Lets Win64 routines be exercised on
 * a System V host. Raw x86-64 machine code (the host toolchain emits System V
 * calls, so the routines are hand-assembled bytes):
 *   mov rax, rcx -> 48 89 c8     mov rax, [rsp+40] -> 48 8b 44 24 28
 *   add rax, rdx -> 48 01 d0     mov rax, [rcx]    -> 48 8b 01
 *   ret          -> c3
 * ------------------------------------------------------------------------- */
TEST(emu_win64, runs_routine_in_isolation) {
    /* arg0 + arg1, taken from rcx and rdx under Win64. */
    static const unsigned char ADD[] = {0x48, 0x89, 0xc8, /* mov rax, rcx */
                                        0x48, 0x01, 0xd0, /* add rax, rdx */
                                        0xc3};            /* ret          */
    emu_t *e = emu_open();
    ASSERT_TRUE(e != NULL);
    emu_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_call_win64(e, ADD, sizeof ADD, args, 2, 0, &r));
    ASSERT_REG_EQ(&r.regs, rax, 42);
    emu_close(e);
}

TEST(emu_win64, convention_selects_argument_registers) {
    /* `mov rax, rcx; ret` returns whatever is in rcx. Under Win64 rcx is the
     * 1st argument; under System V it is the 4th. Same bytes, same args — the
     * result diverges purely by calling convention. */
    static const unsigned char RET_RCX[] = {0x48, 0x89, 0xc8, 0xc3};
    emu_t *e = emu_open();
    emu_result_t w, s;
    long args[] = {7, 0, 0, 0};
    ASSERT_TRUE(emu_call_win64(e, RET_RCX, sizeof RET_RCX, args, 4, 0, &w));
    ASSERT_EQ(w.regs.rax, 7); /* Win64: rcx = arg0 */
    ASSERT_TRUE(emu_call(e, RET_RCX, sizeof RET_RCX, args, 4, 0, &s));
    ASSERT_EQ(s.regs.rax, 0); /* System V: rcx = arg3 = 0 */
    emu_close(e);
}

TEST(emu_win64, fifth_arg_sits_above_shadow_space) {
    /* `mov rax, [rsp+40]; ret`: the 5th integer arg lands just above the
     * 32-byte shadow space ([rsp]=retaddr, [rsp+8..39]=shadow, [rsp+40]=arg5),
     * not at [rsp+8] — proving the caller reserved the home space. */
    static const unsigned char ARG5[] = {0x48, 0x8b, 0x44, 0x24, 0x28, 0xc3};
    emu_t *e = emu_open();
    emu_result_t r;
    long args[] = {1, 2, 3, 4, 0x55};
    ASSERT_TRUE(emu_call_win64(e, ARG5, sizeof ARG5, args, 5, 0, &r));
    ASSERT_EQ(r.regs.rax, 0x55);
    emu_close(e);
}

TEST(emu_win64, fault_injection_catches_bad_load) {
    /* `mov rax, [rcx]; ret`: dereference arg0 (in rcx); aim it at unmapped. */
    static const unsigned char LOAD[] = {0x48, 0x8b, 0x01, 0xc3};
    emu_t *e = emu_open();
    emu_result_t r;
    long args[] = {(long)0xdead0000UL};
    bool ok = emu_call_win64(e, LOAD, sizeof LOAD, args, 1, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(r.faulted);
    ASSERT_UEQ(r.fault_addr, 0xdead0000UL);
    ASSERT_EQ(r.fault_kind, EMU_FAULT_READ);
    emu_close(e);
}
