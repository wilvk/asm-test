/*
 * test_emu.c — exercises the Unicorn-backed emulator tier (x86-64 guest).
 * Build/run via `make emu-test` (requires libunicorn).
 *
 * These use the same TEST/ASSERT framework; emu_call returns full CPU state in
 * an emu_result_t, so ordinary assertions inspect any register or fault.
 */
#include "asmtest.h"
#include "asmtest_emu.h"

extern long add_signed(long a, long b);
extern long sum_via_rbx(long a, long b);
extern long clobbers_rbx(long a, long b);
extern long load_long(void *p);
extern long classify(long x);             /* -1/0/+1; three branch paths */
extern void fill_bytes(void *buf, long val, long n); /* a counted loop  */

/* 64 bytes comfortably covers each of these tiny routines; emulation stops at
 * the routine's own `ret`, so copying past the function end is harmless. */
#define CODE_WINDOW 64

static emu_t *E;

SETUP(emu) {
    E = emu_open();
}

TEARDOWN(emu) {
    emu_close(E);
    E = NULL;
}

TEST(emu, runs_routine_in_isolation) {
    ASSERT_TRUE(E != NULL);
    emu_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_REG_EQ(&r.regs, rax, 42);
}

TEST(emu, full_state_reveals_clobbered_callee_saved) {
    /* On real hardware ASM_CALLn can only check rbx was restored; the emulator
     * shows the value the routine left in it mid-flight. */
    emu_result_t r;
    long args[] = {3, 4};
    emu_call(E, (void *)clobbers_rbx, CODE_WINDOW, args, 2, 0, &r);
    ASSERT_EQ(r.regs.rax, 7);
    ASSERT_EQ(r.regs.rbx, 7); /* clobbers_rbx leaves a+b in rbx */
}

TEST(emu, compliant_routine_restores_rbx) {
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
    emu_result_t r;
    long args[] = {5, 9};
    emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, /*max_insns=*/1, &r);
    ASSERT_EQ(r.regs.rax, 5); /* rax = rdi; the add has not run yet */
}

TEST(emu, fault_injection_catches_bad_load) {
    /* load_long dereferences its pointer arg; aim it at unmapped memory. */
    emu_result_t r;
    long args[] = {(long)0xdead0000UL};
    bool ok = emu_call(E, (void *)load_long, CODE_WINDOW, args, 1, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_TRUE(r.faulted);
    ASSERT_UEQ(r.fault_addr, 0xdead0000UL);
    ASSERT_EQ(r.fault_kind, EMU_FAULT_READ);
}

TEST(emu, reads_preloaded_guest_memory) {
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
    emu_result_t r;
    uint64_t insns[32], blocks[16];
    emu_trace_t t = {0};
    t.insns = insns;
    t.insns_cap = 32;
    t.blocks = blocks;
    t.blocks_cap = 16;

    long args[] = {7};
    ASSERT_TRUE(emu_call_traced(E, (void *)classify, CODE_WINDOW, args, 1, 0,
                                &r, &t));
    ASSERT_EQ(r.regs.rax, 1);
    ASSERT_TRUE(t.insns_total > 0);
    ASSERT_EQ(t.insns_len, t.insns_total); /* big buffer: nothing dropped */
    ASSERT_FALSE(t.truncated);
    ASSERT_UEQ(t.insns[0], 0);  /* execution starts at the routine's entry */
    ASSERT_TRUE(t.blocks_len >= 1);
    ASSERT_UEQ(t.blocks[0], 0); /* first basic block is the entry block     */
}

TEST(emu, coverage_union_exceeds_single_input) {
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
    ASSERT_TRUE(emu_riscv_call_traced(e, RV_ADD3, sizeof RV_ADD3, args, 3, 0,
                                      &r, &t));
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
