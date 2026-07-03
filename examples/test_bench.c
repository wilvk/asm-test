/*
 * test_bench.c — Phase 9 benchmark demo. Each BENCH body is one measured call;
 * the runner auto-calibrates a repeat count and reports min/median cycles per
 * call. Run with: ./build/test_bench --bench
 *
 * Calls into the asm routines under test cannot be optimized away, so no sink
 * is needed; BENCH_USE() is shown once for a value the compiler could discard.
 */
#include "asmtest.h"

extern long add_signed(long a, long b);
extern long sum_to_n(long n);

/* A single-instruction routine: cost is dominated by the call itself. */
BENCH(arith, add_signed) { add_signed(2, 3); }

/* A short counted loop: visibly costlier per call than add_signed. */
BENCH(arith, sum_to_100) { sum_to_n(100); }

/* A longer loop scales the per-call cost up further. */
BENCH(arith, sum_to_1000) { sum_to_n(1000); }

/* Routine reached through the capture trampoline (the real ABI path), with the
 * result funneled through the sink so nothing is elided. */
BENCH(arith, add_via_capture) {
    regs_t r;
    ASM_CALL2(&r, add_signed, 40, 2);
    BENCH_USE(r.ret);
}
