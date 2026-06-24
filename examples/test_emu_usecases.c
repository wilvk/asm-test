/*
 * test_emu_usecases.c — two UNUSUAL USES of the emulator tier.
 *
 * (1) The emulator as a security sandbox. On real hardware a buffer over-read or
 *     over-write either silently returns garbage or crashes the whole runner.
 *     Inside the virtual CPU it becomes a *precise, reported* fault: we map
 *     exactly one page, drive a counted loop one element past it, and assert the
 *     fault kind (READ vs WRITE) and the exact byte address of the overrun.
 *
 * (2) The emulator as a cross-ISA equivalence checker. The same algorithm
 *     (a + b + c) is run as native x86-64 code AND as raw AArch64, RISC-V, and
 *     ARM32 machine code — each on its own guest CPU, regardless of this host —
 *     and all four results are asserted equal. "One algorithm, four ISAs,
 *     proven to agree."
 *
 * Build/run with `make usecases-emu` (requires libunicorn).
 */
#include "asmtest.h"
#include "asmtest_emu.h"

#include <string.h>

extern long sum_longs(const long *p, long n);  /* counted load loop  */
extern void fill_longs(long *p, long n);        /* counted store loop */
extern long add3(long a, long b, long c);       /* a + b + c          */

#define CODE_WINDOW 64

/* A caller-mapped guest page and the first address just past it. With 8-byte
 * longs, indices 0..511 fit in the page and index 512 lands exactly on PAGE_END. */
#define PAGE_BASE 0x00300000UL
#define PAGE_SIZE 0x1000UL
#define PAGE_END (PAGE_BASE + PAGE_SIZE)
#define LONGS_PER_PAGE (PAGE_SIZE / sizeof(long)) /* 512 */

/* ---------------- (1) the emulator as a security sandbox ----------------- */

static emu_t *E;

SETUP(emu_sandbox) {
    E = emu_open();
    emu_map(E, PAGE_BASE, PAGE_SIZE);
}
TEARDOWN(emu_sandbox) {
    emu_close(E);
    E = NULL;
}

TEST(emu_sandbox, in_bounds_read_is_clean) {
    long data[8];
    long want = 0;
    for (int i = 0; i < 8; i++) {
        data[i] = (i + 1) * 10; /* 10,20,...,80 */
        want += data[i];
    }
    ASSERT_TRUE(emu_write(E, PAGE_BASE, data, sizeof data));

    emu_result_t r;
    long args[] = {(long)PAGE_BASE, 8};
    ASSERT_TRUE(emu_call(E, (void *)sum_longs, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_NO_FAULT(&r);
    ASSERT_EQ(r.regs.rax, want); /* sum landed in rax, nothing read past 8 */
}

TEST(emu_sandbox, over_read_faults_at_exact_page_boundary) {
    /* Read one long past the page: the loop walks 0..512, and index 512 is the
     * first address outside the mapping — the emulator pins the fault there. */
    emu_result_t r;
    long args[] = {(long)PAGE_BASE, (long)(LONGS_PER_PAGE + 1)};
    bool ok = emu_call(E, (void *)sum_longs, CODE_WINDOW, args, 2, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_FAULT_AT(&r, EMU_FAULT_READ, PAGE_END);
}

TEST(emu_sandbox, in_bounds_write_is_observed) {
    /* Pre-fill the page with a marker, zero the first 4 longs via the routine,
     * then read the page back out: the writes are visible and bounded. */
    long marker[8];
    for (int i = 0; i < 8; i++)
        marker[i] = 0x7777;
    ASSERT_TRUE(emu_write(E, PAGE_BASE, marker, sizeof marker));

    emu_result_t r;
    long args[] = {(long)PAGE_BASE, 4};
    ASSERT_TRUE(emu_call(E, (void *)fill_longs, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_NO_FAULT(&r);

    long got[8];
    ASSERT_TRUE(emu_read(E, PAGE_BASE, got, sizeof got));
    for (int i = 0; i < 4; i++)
        ASSERT_EQ(got[i], 0); /* zeroed */
    for (int i = 4; i < 8; i++)
        ASSERT_EQ(got[i], 0x7777); /* untouched */
}

TEST(emu_sandbox, over_write_faults_at_exact_page_boundary) {
    emu_result_t r;
    long args[] = {(long)PAGE_BASE, (long)(LONGS_PER_PAGE + 1)};
    bool ok = emu_call(E, (void *)fill_longs, CODE_WINDOW, args, 2, 0, &r);
    ASSERT_FALSE(ok);
    ASSERT_FAULT_AT(&r, EMU_FAULT_WRITE, PAGE_END);
}

/* ------------- (2) cross-ISA equivalence: one algorithm, four CPUs -------- */

/* Raw `a + b + c; return` machine code for each guest (little-endian), the same
 * encodings the emulator suite uses elsewhere:
 *   AArch64: add x0,x0,x1 ; add x0,x0,x2 ; ret
 *   RISC-V : add a0,a0,a1 ; add a0,a0,a2 ; ret
 *   ARM32  : add r0,r0,r1 ; add r0,r0,r2 ; bx lr
 */
static const unsigned char A64_ADD3[] = {0x00, 0x00, 0x01, 0x8b, 0x00, 0x00,
                                         0x02, 0x8b, 0xc0, 0x03, 0x5f, 0xd6};
static const unsigned char RV_ADD3[] = {0x33, 0x05, 0xb5, 0x00, 0x33, 0x05,
                                        0xc5, 0x00, 0x67, 0x80, 0x00, 0x00};
static const unsigned char ARM_ADD3[] = {0x01, 0x00, 0x80, 0xe0, 0x02, 0x00,
                                         0x80, 0xe0, 0x1e, 0xff, 0x2f, 0xe1};

TEST(emu_crossisa, add3_agrees_across_four_isas) {
    long a = 11, b = 22, c = 33;
    long want = a + b + c; /* 66, the C reference */
    long args[] = {a, b, c};

    /* x86-64: copied from the natively-built routine. */
    emu_t *x = emu_open();
    emu_result_t rx;
    ASSERT_TRUE(emu_call(x, (void *)add3, CODE_WINDOW, args, 3, 0, &rx));
    ASSERT_NO_FAULT(&rx);
    ASSERT_EQ(rx.regs.rax, want);
    emu_close(x);

    /* AArch64 guest. */
    emu_arm64_t *e64 = emu_arm64_open();
    emu_arm64_result_t r64;
    ASSERT_TRUE(
        emu_arm64_call(e64, A64_ADD3, sizeof A64_ADD3, args, 3, 0, &r64));
    ASSERT_NO_FAULT(&r64);
    ASSERT_EQ(r64.regs.x[0], want);
    emu_arm64_close(e64);

    /* RISC-V (RV64) guest: the result lands in a0 == x[10]. */
    emu_riscv_t *erv = emu_riscv_open();
    emu_riscv_result_t rrv;
    ASSERT_TRUE(emu_riscv_call(erv, RV_ADD3, sizeof RV_ADD3, args, 3, 0, &rrv));
    ASSERT_NO_FAULT(&rrv);
    ASSERT_EQ(rrv.regs.x[10], want);
    emu_riscv_close(erv);

    /* ARM32 guest: the result lands in r0. */
    emu_arm_t *ea = emu_arm_open();
    emu_arm_result_t ra;
    ASSERT_TRUE(emu_arm_call(ea, ARM_ADD3, sizeof ARM_ADD3, args, 3, 0, &ra));
    ASSERT_NO_FAULT(&ra);
    ASSERT_EQ(ra.regs.r[0], want);
    emu_arm_close(ea);

    /* All four guests, and the C model, computed the same value. */
    ASSERT_EQ((long)rx.regs.rax, (long)r64.regs.x[0]);
    ASSERT_EQ((long)r64.regs.x[0], (long)rrv.regs.x[10]);
    ASSERT_EQ((long)rrv.regs.x[10], (long)ra.regs.r[0]);
}
