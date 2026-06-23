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
