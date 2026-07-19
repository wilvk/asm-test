/*
 * test_apx_basic.c — Intel APX (r16-r31 / REX2 / NDD) register/flag/ABI
 * assertions. The routines in apx_basic.s use extended GP registers and the
 * new-data-destination form, which #UD on all shipping silicon today; every case
 * therefore GATES on asmtest_cpu_has_apx() and only executes under
 * `sde64 -future`, whose emulated CPUID reports APX_F. Run via the SDE lane
 * (`make sde-test SDE_HOME=... ` / `make docker-sde` -> sde-apx-test); this suite
 * is in SUITE_EXCLUDES so a host GAS that predates APX never has to assemble it in
 * `make test`.
 *
 * The gate is what keeps the fixture honest in BOTH directions: bare on real
 * pre-APX silicon every case reports `# SKIP` (proving it cannot rot into a
 * vacuous pass), while under SDE the gate opens and the assertions actually run.
 */
#include "asmtest.h"

extern long apx_sum4(long a, long b, long c, long d);
extern long apx_ndd_add(long a, long b);
extern long apx_egpr_carry(long a, long b);

/* Suite-local gate, modeled on test_emu.c's REQUIRE_X86_HOST: abort the case with
 * a named skip unless APX is available (i.e. unless we are under sde64 -future). */
#define REQUIRE_APX()                                                          \
    if (!asmtest_cpu_has_apx())                                                \
    SKIP("APX not available on this host (run under sde64 -future)")

TEST(apx, egpr_sum4_computes_through_r16_r19) {
    REQUIRE_APX();
    /* (1+2) + (3+4) = 10, routed entirely through r16-r19 (REX2 lea). */
    ASSERT_EQ(apx_sum4(1, 2, 3, 4), 10);
    ASSERT_EQ(apx_sum4(100, 20, 3, 0), 123);
}

TEST(apx, egpr_use_preserves_classic_callee_saved) {
    /* apx_sum4 clobbers r16-r19 (extra APX scratch state); the classic
     * callee-saved set (rbx/rbp/r12-r15) must still be intact on return. */
    REQUIRE_APX();
    regs_t r;
    ASM_CALL4(&r, apx_sum4, 4, 5, 6, 7);
    ASSERT_EQ(r.ret, 22);
    ASSERT_ABI_PRESERVED(&r);
}

TEST(apx, ndd_add_leaves_sources_and_returns_sum) {
    /* APX new-data-destination: rax = rdi + rsi, rdi/rsi untouched. */
    REQUIRE_APX();
    regs_t r;
    ASM_CALL2(&r, apx_ndd_add, 20, 22);
    ASSERT_EQ(r.ret, 42);
    ASSERT_ABI_PRESERVED(&r);
}

TEST(apx, egpr_add_carry_sets_cf) {
    /* -1 + 1 == 0 with an unsigned carry out of bit 63: the EGPR add sets CF, and
     * the captured flags prove SDE models APX flag effects (not just results). */
    REQUIRE_APX();
    regs_t r;
    ASM_CALL2(&r, apx_egpr_carry, -1, 1);
    ASSERT_EQ(r.ret, 0);
    ASSERT_FLAG_SET(&r, CF);
}
