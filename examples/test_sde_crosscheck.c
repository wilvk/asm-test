/*
 * test_sde_crosscheck.c — anchor SDE against an existing in-tree oracle.
 *
 * For baseline ISA that BOTH engines model (scalar GP + SSE), the same routine
 * executed natively (under SDE, on SDE's emulated CPU) and through the Unicorn
 * emulator tier must agree — register result, arithmetic flags, and every SSE
 * lane — in one binary. The sde-crosscheck-test sub-lane runs this twice: bare
 * (native-on-real-silicon vs Unicorn — valid on any x86-64 box) and under
 * `sde64 -future` (SDE vs Unicorn-inside-SDE — the lane's anchor). A bare failure
 * is an emulator-tier bug; an SDE-only failure is an SDE emulation divergence.
 *
 * Kept to SSE and below on purpose: Unicorn's vendored QEMU 5.0.1 has no AVX TCG
 * (the ceiling this whole lane routes around), so AVX belongs to sde-avx512-test's
 * one-sided assertion, not here.
 */
#include "asmtest.h"
#include "asmtest_emu.h"

extern long add_signed(long a, long b);
extern long sum_via_rbx(long a, long b);
extern void vec_add4f(void); /* vec128 vec_add4f(vec128 a, vec128 b) */

/* 64 bytes covers each tiny routine; emulation stops at the routine's own ret. */
#define CODE_WINDOW 64

/* The cross-check drives the HOST's compiled routine bytes through Unicorn, which
 * is valid x86-64 input only on an x86-64 host (mirrors test_emu.c). Off x86-64
 * the bytes are a different ISA; skip. The SDE lane only runs on x86-64 anyway. */
#if defined(__x86_64__)
#define REQUIRE_X86_HOST() ((void)0)
#else
#define REQUIRE_X86_HOST()                                                     \
    SKIP("x86-64 cross-check drives host-compiled routine bytes; host is not " \
         "x86-64")
#endif

static emu_t *E;

SETUP(crosscheck) { E = emu_open(); }

TEARDOWN(crosscheck) {
    emu_close(E);
    E = NULL;
}

TEST(crosscheck, gp_add) {
    REQUIRE_X86_HOST();
    ASSERT_TRUE(E != NULL);
    long native = add_signed(20, 22);
    emu_result_t r;
    long args[] = {20, 22};
    ASSERT_TRUE(emu_call(E, (void *)add_signed, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_EQ(native, (long)r.regs.rax); /* SDE/native == Unicorn */
    ASSERT_EQ(native, 42);
}

TEST(crosscheck, gp_flags) {
    REQUIRE_X86_HOST();
    /* Native capture (under SDE, this runs on SDE's emulated CPU). */
    regs_t cap;
    ASM_CALL2(&cap, sum_via_rbx, 1, 2);
    /* Same routine bytes through Unicorn. */
    emu_result_t r;
    long args[] = {1, 2};
    ASSERT_TRUE(emu_call(E, (void *)sum_via_rbx, CODE_WINDOW, args, 2, 0, &r));
    ASSERT_EQ((long)cap.ret, (long)r.regs.rax);
    /* The defined arithmetic flags agree (reserved/undefined bits are not
     * compared — only CF and ZF are architecturally meaningful here). */
    ASSERT_EQ((int)((cap.flags & ASMTEST_CF) != 0),
              (int)((r.regs.rflags & ASMTEST_CF) != 0));
    ASSERT_EQ((int)((cap.flags & ASMTEST_ZF) != 0),
              (int)((r.regs.rflags & ASMTEST_ZF) != 0));
}

TEST(crosscheck, sse_vec) {
    REQUIRE_X86_HOST();
    vec128_t a = {.f32 = {1.0f, 2.0f, 3.0f, 4.0f}};
    vec128_t b = {.f32 = {10.0f, 20.0f, 30.0f, 40.0f}};
    /* Native SSE capture (SDE's emulated CPU under the lane). */
    regs_t cap;
    ASM_VCALL2(&cap, vec_add4f, a, b);
    /* Same routine bytes through Unicorn. */
    emu_vec128_t ea = {0}, eb = {0};
    for (int i = 0; i < 4; i++) {
        ea.f32[i] = a.f32[i];
        eb.f32[i] = b.f32[i];
    }
    emu_vec128_t vargs[2] = {ea, eb};
    long iargs[1] = {0};
    emu_result_t r;
    ASSERT_TRUE(emu_call_vec(E, (void *)vec_add4f, CODE_WINDOW, iargs, 0, vargs,
                             2, 0, &r));
    for (int i = 0; i < 4; i++)
        ASSERT_FEQ(cap.vec[0].f32[i], r.regs.xmm[0].f32[i]);
}
