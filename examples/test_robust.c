/*
 * test_robust.c — Phase 8 robustness demo (intentionally not part of `make
 * test`). Shows that a hang or a crash in the routine under test becomes a
 * reported failure while the rest of the run continues.
 *
 *   make demo-robust              # fork-isolated, short timeout
 *
 * Run with a small --timeout so the spin is caught quickly, e.g.
 *   ./build/test_robust --timeout=2
 * The `survives_*` tests pass, proving the runner kept going after the
 * timeout/crash. Add --no-fork to see the in-process model still catch the
 * timeout and SIGSEGV (but a SIGABRT-class corruption would take it down —
 * which is exactly what the default fork isolation prevents).
 */
#include "asmtest.h"

extern long spin_forever(void);
extern long crash_null(void);

TEST(robust, hang_is_reported_as_timeout) {
    regs_t r;
    ASM_CALL0(&r, spin_forever); /* never returns; alarm() -> timeout failure */
    ASSERT_EQ((long)r.ret, 0);   /* unreachable */
}

TEST(robust, survives_the_timeout) {
    ASSERT_EQ(1 + 1, 2); /* runs only because the hang was contained */
}

TEST(robust, crash_is_reported_as_failure) {
    regs_t r;
    ASM_CALL0(&r, crash_null); /* SIGSEGV -> reported failure */
    ASSERT_EQ((long)r.ret, 0); /* unreachable */
}

TEST(robust, survives_the_crash) {
    ASSERT_EQ(2 * 2, 4); /* runs only because the crash was contained */
}
