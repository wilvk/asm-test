/*
 * tests/positive.c — the framework's own self-tests (success paths).
 *
 * Track A of docs/internal/plans/expansion-plan.md. These cases exercise the *passing*
 * behavior of every assertion family plus SKIP and SETUP/TEARDOWN. Run directly
 * (the binary must exit 0); tests/expect.sh also drives this binary to check
 * runner behavior (--list, --filter, --shuffle, --format=junit).
 *
 * Pure C: register/flag/vector assertions take a regs_t built by hand here, so
 * no routine-under-test or capture trampoline is needed.
 */
#include "asmtest.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <string.h>

/* A regs_t whose callee-saved fields all hold their ABI sentinels, i.e. a
 * routine that correctly preserved every callee-saved register. */
static regs_t preserved_regs(void) {
    regs_t r;
    memset(&r, 0, sizeof r);
#if defined(__x86_64__)
    r.rbx = ASMTEST_SENTINEL_RBX;
    r.rbp = ASMTEST_SENTINEL_RBP;
    r.r12 = ASMTEST_SENTINEL_R12;
    r.r13 = ASMTEST_SENTINEL_R13;
    r.r14 = ASMTEST_SENTINEL_R14;
    r.r15 = ASMTEST_SENTINEL_R15;
#elif defined(__aarch64__)
    r.x19 = ASMTEST_SENTINEL_X19;
    r.x20 = ASMTEST_SENTINEL_X20;
    r.x21 = ASMTEST_SENTINEL_X21;
    r.x22 = ASMTEST_SENTINEL_X22;
    r.x23 = ASMTEST_SENTINEL_X23;
    r.x24 = ASMTEST_SENTINEL_X24;
    r.x25 = ASMTEST_SENTINEL_X25;
    r.x26 = ASMTEST_SENTINEL_X26;
    r.x27 = ASMTEST_SENTINEL_X27;
    r.x28 = ASMTEST_SENTINEL_X28;
    r.x29 = ASMTEST_SENTINEL_X29;
#elif defined(__riscv) && __riscv_xlen == 64
    r.s0 = ASMTEST_SENTINEL_S0;
    r.s1 = ASMTEST_SENTINEL_S1;
    r.s2 = ASMTEST_SENTINEL_S2;
    r.s3 = ASMTEST_SENTINEL_S3;
    r.s4 = ASMTEST_SENTINEL_S4;
    r.s5 = ASMTEST_SENTINEL_S5;
    r.s6 = ASMTEST_SENTINEL_S6;
    r.s7 = ASMTEST_SENTINEL_S7;
    r.s8 = ASMTEST_SENTINEL_S8;
    r.s9 = ASMTEST_SENTINEL_S9;
    r.s10 = ASMTEST_SENTINEL_S10;
    r.s11 = ASMTEST_SENTINEL_S11;
    /* fs0-fs11 (f8/f9, f18-f27) sentinels for ASSERT_ABI_PRESERVED_VEC: the _fp
     * trampoline stores each fs's f-register number into vec[reg#].u64[0]. */
    r.vec[8].u64[0] = 8;
    r.vec[9].u64[0] = 9;
    for (unsigned i = 18; i <= 27; i++)
        r.vec[i].u64[0] = i;
#endif
    return r;
}

/* ---- value comparisons ---- */

TEST(posit, eq) { ASSERT_EQ(2 + 3, 5); }
TEST(posit, ne) { ASSERT_NE(5, 6); }
TEST(posit, lt) { ASSERT_LT(4, 5); }
TEST(posit, le) {
    ASSERT_LE(5, 5);
    ASSERT_LE(4, 5);
}
TEST(posit, gt) { ASSERT_GT(6, 5); }
TEST(posit, ge) {
    ASSERT_GE(5, 5);
    ASSERT_GE(6, 5);
}
TEST(posit, truefalse) {
    ASSERT_TRUE(1);
    ASSERT_FALSE(0);
}

/* ---- unsigned comparisons: the high bit must order as unsigned ---- */

TEST(posit, ueq) { ASSERT_UEQ(0xFFFFFFFFFFFFFFFFUL, ~0UL); }
TEST(posit, unsigned_orders_high_bit) {
    ASSERT_LT(-1L, 1L);                  /* signed: -1 < 1            */
    ASSERT_UGT((unsigned long)-1L, 1UL); /* unsigned: 0xffff... > 1   */
    ASSERT_ULT(1UL, (unsigned long)-1L);
}

/* ---- strings & memory ---- */

TEST(posit, streq) { ASSERT_STREQ("abc", "abc"); }
TEST(posit, mem_eq) {
    unsigned char a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_MEM_EQ(a, b, sizeof a);
}

/* ---- register / flag / vector assertions over a hand-built regs_t ---- */

TEST(posit, abi_preserved) {
    regs_t r = preserved_regs();
    ASSERT_ABI_PRESERVED(&r);
}
#if defined(__riscv) && __riscv_xlen == 64
/* rv64 FP callee-saved (fs0-fs11) check over a hand-built regs_t: all sentinels
 * correct -> ASSERT_ABI_PRESERVED_VEC passes. */
TEST(posit, abi_preserved_vec) {
    regs_t r = preserved_regs();
    ASSERT_ABI_PRESERVED_VEC(&r);
}
#endif
#if !defined(ASMTEST_NO_FLAGS)
TEST(posit, flag_set) {
    regs_t r = preserved_regs();
    r.flags = ASMTEST_CF | ASMTEST_ZF;
    ASSERT_FLAG_SET(&r, CF);
    ASSERT_FLAG_SET(&r, ZF);
}
TEST(posit, flag_clear) {
    regs_t r = preserved_regs();
    r.flags = 0;
    ASSERT_FLAG_CLEAR(&r, CF);
    ASSERT_FLAG_CLEAR(&r, ZF);
}
#endif /* rv64 has no condition-flags register (ASMTEST_NO_FLAGS) */
TEST(posit, reg_eq) {
    regs_t r = preserved_regs();
    r.ret = 42;
    ASSERT_REG_EQ(&r, ret, 42);
}
TEST(posit, vec_eq) {
    regs_t r = preserved_regs();
    unsigned char expect[16];
    for (int i = 0; i < 16; i++)
        r.vec[0].u8[i] = expect[i] = (unsigned char)(i * 7 + 1);
    ASSERT_VEC_EQ(&r, 0, expect);
}

/* ---- floating point ---- */

TEST(posit, fp_eq) { ASSERT_DEQ(1.5, 1.5); }
TEST(posit, fp_near) {
    double a = 1.0, b = nextafter(1.0, 2.0); /* one ULP apart */
    ASSERT_DNEAR(a, b, 1);
}
TEST(posit, feq) { ASSERT_FEQ(2.25f, 2.25f); }
TEST(posit, fnear) {
    float a = 1.0f, b = nextafterf(1.0f, 2.0f);
    ASSERT_FNEAR(a, b, 1);
}
/* Far-apart operands: the ULP distance across the whole representable range
 * (DBL_MAX vs -DBL_MAX ~= 1.84e19 apart) must be COMPUTED without signed-overflow
 * UB. The gap exceeds INT64_MAX, so the old signed subtraction tripped this repo's
 * own `make sanitize` UBSan lane (halt_on_error=1); the assert passes with a
 * ULONG_MAX tolerance — the point is that evaluating the distance is well-defined. */
TEST(posit, near_full_range_no_overflow) {
    ASSERT_DNEAR(DBL_MAX, -DBL_MAX, ULONG_MAX);
    ASSERT_FNEAR(FLT_MAX, -FLT_MAX, ULONG_MAX);
    /* Signed zeros still collapse to a zero-ULP distance (the transform's intent). */
    ASSERT_DNEAR(0.0, -0.0, 0);
    ASSERT_FNEAR(0.0f, -0.0f, 0);
}

/* ---- RNG ---- */

/* asmtest_rng_range over a range wider than LONG_MAX must not divide by zero.
 * `[LONG_MIN, LONG_MAX]` made the old `(uint64_t)(hi-lo)+1` wrap to 0, so `% span`
 * was a division by zero (SIGFPE) that crashed the runner; a merely-wide range hit
 * the signed `hi-lo` overflow first. The unsigned span + full-width special case
 * fixes both. Also spot-check that the common narrow path still stays in bounds. */
TEST(posit, rng_range_full_width_no_sigfpe) {
    asmtest_rng_t rng = {.s = 0x1234567890abcdefULL};
    for (int i = 0; i < 1000; i++) {
        long v = asmtest_rng_range(&rng, LONG_MIN,
                                   LONG_MAX); /* full width: span wraps to 0 */
        ASSERT_TRUE(v >= LONG_MIN && v <= LONG_MAX);
    }
}
TEST(posit, rng_range_wide_in_bounds) {
    asmtest_rng_t rng = {.s = 0xfeedfacecafebeefULL};
    for (int i = 0; i < 1000; i++) {
        long v = asmtest_rng_range(&rng, LONG_MIN, 0); /* wider than LONG_MAX */
        ASSERT_TRUE(v >= LONG_MIN && v <= 0);
    }
}
TEST(posit, rng_range_narrow_in_bounds) {
    asmtest_rng_t rng = {.s = 42};
    int seen_lo = 0, seen_hi = 0;
    for (int i = 0; i < 1000; i++) {
        long v = asmtest_rng_range(&rng, 5, 10);
        ASSERT_TRUE(v >= 5 && v <= 10);
        seen_lo |= (v == 5);
        seen_hi |= (v == 10); /* endpoints are inclusive */
    }
    ASSERT_TRUE(seen_lo && seen_hi);
}

/* ---- Guard-page allocator ---- */

/* A size within a page of SIZE_MAX makes round_up_page's `n + pg - 1` wrap;
 * pre-fix this collapsed to a small, successful 2-page mmap and handed back a
 * pointer inside the PROT_NONE guard page instead of failing (any dereference
 * faults). Post-fix the allocators reject it outright. */
TEST(posit, guarded_alloc_rejects_size_overflow) {
    ASSERT_TRUE(asmtest_guarded_alloc((size_t)-1) == NULL);
    ASSERT_TRUE(asmtest_guarded_alloc((size_t)-4096) == NULL);
    ASSERT_TRUE(asmtest_guarded_alloc_under((size_t)-1) == NULL);
}

/* ---- SKIP ---- */

TEST(posit, skip_reports) {
    SKIP("intentional skip");
    ASSERT_TRUE(0); /* unreachable: SKIP longjmps out */
}

/* ---- SETUP / TEARDOWN ---- */

static int g_fixture;

SETUP(fix) { g_fixture = 7; }
TEARDOWN(fix) { g_fixture = 0; }

TEST(fix, sees_setup) { ASSERT_EQ(g_fixture, 7); }

/* TEARDOWN actually runs — observed across tests. SETUP/TEARDOWN increment
 * counters; under fork each test is isolated (a later test can't see an earlier
 * teardown's effect on a global), but under --no-fork the tests share state, so a
 * second test can prove the first test's TEARDOWN executed. `g_setups > 1`
 * detects the --no-fork case (state persisted). tests/expect.sh runs the suite
 * --no-fork so this fires. */
static int g_setups, g_teardowns;

SETUP(lifecycle) { g_setups++; }
TEARDOWN(lifecycle) { g_teardowns++; }

/* At entry, my SETUP ran but not my TEARDOWN, and every prior lifecycle test's
 * SETUP+TEARDOWN both ran — so setups is exactly one ahead of teardowns. */
TEST(lifecycle, first) { ASSERT_EQ(g_setups - g_teardowns, 1); }
TEST(lifecycle, observes_prior_teardown) {
    if (g_setups > 1) /* --no-fork: `first` already ran */
        ASSERT_EQ(g_teardowns, g_setups - 1); /* ...and its TEARDOWN executed */
}
