/*
 * tests/negative.c — the framework's own self-tests (failure paths).
 *
 * Track A of docs/internal/plans/expansion-plan.md. Every case here is DESIGNED TO FAIL
 * (or crash/hang). tests/expect.sh runs each one filtered, asserting that the
 * binary exits nonzero and prints the expected diagnostic — i.e. that the
 * framework's assertions, crash handling, timeout, and fork-status synthesis
 * report failure when they should. Running the whole binary is expected to exit
 * nonzero; when the harness runs it unfiltered it always passes a bounded
 * --timeout so the neg.timeout spin loop can't stall the run.
 *
 * Pure C: register/flag/vector cases build a regs_t by hand, so no
 * routine-under-test or capture trampoline is needed.
 */
#include "asmtest.h"

#include <signal.h> /* raise(SIGILL/SIGFPE/SIGBUS) for the containment cases */

#include <stdlib.h>
#include <string.h>

/* All callee-saved sentinels correct EXCEPT one, modelling a routine that
 * clobbered a callee-saved register. */
static regs_t clobbered_regs(void) {
    regs_t r;
    memset(&r, 0, sizeof r);
#if defined(__x86_64__)
    r.rbx = ASMTEST_SENTINEL_RBX;
    r.rbp = ASMTEST_SENTINEL_RBP;
    r.r12 = ASMTEST_SENTINEL_R12;
    r.r13 = ASMTEST_SENTINEL_R13;
    r.r14 = ASMTEST_SENTINEL_R14;
    r.r15 = 0xDEAD; /* clobbered */
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
    r.x29 = 0xDEAD; /* clobbered */
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
    r.s11 = 0xDEAD; /* clobbered */
    /* fs0-fs11 sentinels correct EXCEPT fs2 (vec[18]) -> a routine that clobbered
     * one callee-saved FP register, for the ASSERT_ABI_PRESERVED_VEC failure. */
    r.vec[8].u64[0] = 8;
    r.vec[9].u64[0] = 9;
    for (unsigned i = 18; i <= 27; i++)
        r.vec[i].u64[0] = i;
    r.vec[18].u64[0] = 0xDEAD; /* fs2 clobbered */
#endif
    return r;
}

/* ---- value comparisons ---- */

TEST(neg, eq) { ASSERT_EQ(1, 2); }
TEST(neg, ne) { ASSERT_NE(5, 5); }
TEST(neg, lt) { ASSERT_LT(5, 5); }
TEST(neg, truefail) { ASSERT_TRUE(0); }
TEST(neg, falsefail) { ASSERT_FALSE(1); }

/* Unsigned compare must treat the high bit as a large magnitude: (unsigned)-1
 * is NOT < 1, so this fails. A wrongly-signed compare would pass it. */
TEST(neg, ult_ordering) { ASSERT_ULT((unsigned long)-1L, 1UL); }

/* ---- strings & memory ---- */

TEST(neg, streq) { ASSERT_STREQ("abc", "abd"); }
TEST(neg, mem_eq) {
    unsigned char a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char b[8] = {1, 2, 3, 9, 5, 6, 7, 8}; /* differs at byte 3 */
    ASSERT_MEM_EQ(a, b, sizeof a);
}

/* ---- register / flag / vector assertions ---- */

TEST(neg, abi) {
    regs_t r = clobbered_regs();
    ASSERT_ABI_PRESERVED(&r);
}
#if defined(__riscv) && __riscv_xlen == 64
/* rv64 FP callee-saved check: fs2 (vec[18]) was clobbered, so
 * ASSERT_ABI_PRESERVED_VEC must fail naming "fs2 not restored". */
TEST(neg, abi_vec) {
    regs_t r = clobbered_regs();
    ASSERT_ABI_PRESERVED_VEC(&r);
}
#endif
#if !defined(ASMTEST_NO_FLAGS)
TEST(neg, flag_set) {
    regs_t r = clobbered_regs();
    r.flags = 0; /* CF clear */
    ASSERT_FLAG_SET(&r, CF);
}
TEST(neg, flag_clear) {
    regs_t r = clobbered_regs();
    r.flags = ASMTEST_CF; /* CF set */
    ASSERT_FLAG_CLEAR(&r, CF);
}
#endif /* rv64 has no condition-flags register (ASMTEST_NO_FLAGS) */
TEST(neg, vec_eq) {
    regs_t r = clobbered_regs();
    unsigned char expect[16];
    for (int i = 0; i < 16; i++) {
        r.vec[0].u8[i] = (unsigned char)i;
        expect[i] = (unsigned char)i;
    }
    expect[5] ^= 0xFF; /* one differing byte */
    ASSERT_VEC_EQ(&r, 0, expect);
}

/* ---- floating point ---- */

TEST(neg, fp_eq) { ASSERT_DEQ(1.0, 2.0); }
TEST(neg, fp_near) {
    ASSERT_DNEAR(1.0, 2.0, 4);
} /* far more than 4 ULPs apart */
TEST(neg, feq) { ASSERT_FEQ(1.0f, 2.0f); }

/* ---- crash / timeout / abort containment ----
 *
 * These verify that a misbehaving test body is turned into a reported failure
 * rather than taking the runner down. The harness runs each filtered. */

/* SIGSEGV — caught by the in-process signal handler (works with or without
 * fork) and reported as a fatal-signal failure. */
TEST(neg, crash) {
    volatile int *p = (volatile int *)0;
    *p = 1;
}

/* Infinite loop — the per-test alarm() fires SIGALRM and reports a timeout.
 * The harness runs this with a short --timeout. The volatile guard keeps the
 * loop from being optimized away. */
TEST(neg, timeout) {
    volatile int spin = 1;
    while (spin) {
    }
}

/* abort() raises SIGABRT, which the in-process handler does NOT catch. Under
 * fork (the default) the child dies and the parent synthesizes the result from
 * the wait status — the path that makes fork the default. */
TEST(neg, aborts) { abort(); }

/* SIGILL / SIGFPE / SIGBUS — the runner installs handlers for these too (a bad
 * opcode, a divide fault, and a bus error are common real asm-routine crashes),
 * but only SIGSEGV/SIGABRT were self-tested. Delivered via raise() so they fire
 * identically on x86-64 and AArch64 (where integer div-by-zero does not trap and
 * unaligned access does not bus-fault). Each must surface as a contained failure
 * naming the signal. */
TEST(neg, illegal_instruction) { raise(SIGILL); }
TEST(neg, fp_exception) { raise(SIGFPE); }
TEST(neg, bus_error) { raise(SIGBUS); }

/* ---- headline safety features that only the demo exercised ----
 *
 * The guard-page fault and the differential-model mismatch were run only in
 * test_failure_demo (whose recipe prefixes `-` and asserts nothing), so a
 * regression — e.g. guarded_alloc degrading to malloc — stayed green. Pin them
 * as real, asserted failures here. */

/* One past the end of a guard-page buffer faults (SIGSEGV). */
TEST(neg, guard_page_overrun) {
    unsigned char *p = asmtest_guarded_alloc(8);
    p[8] = 0x41; /* the trailing guard page */
    asmtest_guarded_free(p, 8);
}
/* One before the start of an underrun-guarded buffer faults (SIGSEGV). */
TEST(neg, guard_page_underrun) {
    unsigned char *p = asmtest_guarded_alloc_under(8);
    p[-1] = 0x41; /* the leading guard page */
    asmtest_guarded_free_under(p, 8);
}

/* A routine that disagrees with its C model — differential testing reports the
 * first failing input. buggy_sum returns a-b where the model returns a+b, and
 * the generator always draws a nonzero b, so the very first draw disagrees. */
static long ref_sum(long a, long b) { return a + b; }
static long buggy_sum(long a, long b) { return a - b; }
static int gen_two(asmtest_rng_t *rng, long *out, int cap) {
    if (cap < 2)
        return 0;
    out[0] = (long)(asmtest_rng_u64(rng) % 100);
    out[1] = 1 + (long)(asmtest_rng_u64(rng) % 100); /* nonzero: a-b != a+b */
    return 2;
}
TEST(neg, ref_model_mismatch) {
    ASSERT_MATCHES_REF2(buggy_sum, ref_sum, gen_two, 1000);
}

/* The FP counterpart: buggy_fsum subtracts where the model adds, and the
 * generator draws b >= 1, so the very first trial exceeds 0 ulps. */
static double fref_sum(double a, double b) { return a + b; }
static double buggy_fsum(double a, double b) { return a - b; }
static int gen_ftwo(asmtest_rng_t *rng, double *out, int cap) {
    if (cap < 2)
        return 0;
    out[0] = (double)(asmtest_rng_u64(rng) % 100);
    out[1] = 1.0 + (double)(asmtest_rng_u64(rng) % 100);
    return 2;
}
TEST(neg, fref_model_mismatch) {
    ASSERT_MATCHES_FREF2(buggy_fsum, fref_sum, gen_ftwo, 1000, 0);
}

/* An assertion message full of XML metacharacters, so the JUnit output must
 * escape `< & > "` — the escaping was never validated (xmllint ran only on the
 * all-passing suite). tests/expect.sh xmllint-checks this suite's JUnit. */
TEST(neg, xml_special_chars) { ASSERT_STREQ("a<b&c>d", "w\"x'y"); }

/* Record mode (docs/internal/gui/06-doors-and-learning.md T7): a producer noted
 * a recording and a step, then the test failed. The runner must carry BOTH into
 * the TAP and JUnit failure reports. Deliberately ENGINE-FREE — nothing here
 * links an emulator, so the plumbing is tested even where no producer exists,
 * and the path is a fiction (nothing opens it) because what is under test is
 * the report, not the file. */
TEST(neg, records_a_recording) {
    asmtest_note_recording("fake.asmtrace", 7);
    ASSERT_EQ(1, 2);
}

/* A test that notes NOTHING must emit no `recording:` key at all — the honest
 * degrade for every suite with no producer glue. */
TEST(neg, records_nothing) { ASSERT_EQ(3, 4); }
