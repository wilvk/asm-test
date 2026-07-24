/*
 * test_ct_eq.c — the constant-time equality suite
 * (docs/internal/gui/06-doors-and-learning.md T1).
 *
 * Build/run via `make ct-eq-test` (needs libunicorn); `make docker-emu` runs it
 * beside `emu-test`.
 *
 * Two kinds of test live here and they prove different things:
 *
 *  - NATIVE correctness runs the routines directly and needs no engine. It runs
 *    on every host the example assembly targets.
 *  - The CT PROOF drives the host-compiled routine bytes through the emulator,
 *    so the `extern` symbols must be x86-64 machine code — the same host-ISA
 *    gate `test_emu.c:34` carries, for the same reason.
 *
 * THE ORACLE, and its documented limit. emu_call_traced ACCUMULATES into a
 * re-used trace struct (include/asmtest_emu.h:172), so running secret-differing
 * inputs into ONE trace makes the block set a union. If the union never grows
 * after the first run, no basic block depended on the differing bytes. That is
 * a statement about CONTROL FLOW only: it says nothing about data-dependent
 * memory addressing (a table lookup indexed by a secret keeps one block set and
 * still leaks through the cache). Address-level CT verdicts need the Wave-1
 * `mem[]` stream; the block-union oracle is the shipped v1 property and this
 * comment is the boundary.
 *
 * `leaky_eq` is the control. Without it the union assertion could pass for a
 * routine that never ran at all.
 */
#include "asmtest.h"
#include "asmtest_emu.h"
#include "asmtest_rec.h"

#include <string.h>

extern long ct_eq(const void *a, const void *b, long n);
extern long leaky_eq(const void *a, const void *b, long n);

/* 128 bytes covers both routines' bodies; emulation stops at the routine's own
 * `ret`, so copying past the function end is harmless. */
#define CODE_WINDOW 128

/* Guest data page for the two buffers: A at BUF_A, B at BUF_B, 16 bytes each.
 * NOT 0x00200000 (which the doc suggested): that is the emulator's own STACK
 * base (src/emu.c:15), and emu_map refuses to overlap an internal region. */
#define DATA_PAGE 0x00300000UL
#define DATA_SIZE 0x1000
#define BUF_A     (DATA_PAGE + 0x000)
#define BUF_B     (DATA_PAGE + 0x100)
#define BUF_LEN   16

#if defined(__x86_64__)
#define REQUIRE_X86_HOST() ((void)0)
#else
#define REQUIRE_X86_HOST()                                                     \
    SKIP("the CT proof drives host-compiled routine bytes; host is not "       \
         "x86-64")
#endif

static emu_t *E;

SETUP(ct_eq) { E = emu_open(); }

TEARDOWN(ct_eq) {
    emu_close(E);
    E = NULL;
}

/* ------------------------------------------------------------------ */
/* Native correctness — no engine, every host                          */
/* ------------------------------------------------------------------ */

TEST(ct_eq, correct_on_native) {
    unsigned char a[BUF_LEN], b[BUF_LEN];
    memset(a, 0xa5, sizeof a);
    memcpy(b, a, sizeof b);
    ASSERT_EQ(ct_eq(a, b, BUF_LEN), 1);
    ASSERT_EQ(leaky_eq(a, b, BUF_LEN), 1);

    b[0] ^= 0x01; /* differs at the FIRST byte */
    ASSERT_EQ(ct_eq(a, b, BUF_LEN), 0);
    ASSERT_EQ(leaky_eq(a, b, BUF_LEN), 0);

    memcpy(b, a, sizeof b);
    b[BUF_LEN - 1] ^= 0x80; /* differs at the LAST byte */
    ASSERT_EQ(ct_eq(a, b, BUF_LEN), 0);
    ASSERT_EQ(leaky_eq(a, b, BUF_LEN), 0);

    /* A zero-length compare is equal by definition — and is the one branch
     * ct_eq legitimately has (on the PUBLIC length). */
    ASSERT_EQ(ct_eq(a, b, 0), 1);
}

/* ------------------------------------------------------------------ */
/* The CT proof — block-coverage union across secret-differing inputs  */
/* ------------------------------------------------------------------ */

/* Write the variant's two buffers into the guest. variant 0 = equal,
 * 1 = differ at byte 0, 2 = differ at byte 15. */
static void write_variant(emu_t *e, int variant) {
    unsigned char a[BUF_LEN], b[BUF_LEN];
    memset(a, 0xa5, sizeof a);
    memcpy(b, a, sizeof b);
    if (variant == 1)
        b[0] ^= 0x01;
    else if (variant == 2)
        b[BUF_LEN - 1] ^= 0x80;
    emu_write(e, BUF_A, a, sizeof a);
    emu_write(e, BUF_B, b, sizeof b);
}

/* Run all three variants into ONE accumulating trace and report the block-set
 * size after run 1 (`*baseline`) and after run 3 (the return). */
static size_t union_over_variants(emu_t *e, const void *fn, size_t *baseline) {
    uint64_t blocks[64];
    emu_trace_t tr;
    memset(&tr, 0, sizeof tr);
    tr.blocks = blocks;
    tr.blocks_cap = sizeof blocks / sizeof blocks[0];

    emu_result_t r;
    memset(&r, 0, sizeof r);
    *baseline = 0;
    for (int variant = 0; variant < 3; variant++) {
        long args[3] = {(long)BUF_A, (long)BUF_B, BUF_LEN};
        write_variant(e, variant);
        emu_call_traced(e, fn, CODE_WINDOW, args, 3, 0, &r, &tr);
        if (variant == 0)
            *baseline = tr.blocks_len;
    }
    /* Record mode's first adopter (T7): a no-op unless --record-dir armed one,
     * so this call is unconditional. What lands is the UNION trace — which is
     * the thing the assertion below is about, so the recording is evidence for
     * the verdict rather than a sample of it. */
    asmtest_rec_emu(&tr, &r, fn, CODE_WINDOW);

    /* A filled block buffer would make "the union did not grow" vacuously true,
     * so the caller must see the overflow rather than a clean number. */
    ASSERT_FALSE(tr.truncated);
    return tr.blocks_len;
}

TEST(ct_eq, no_secret_dependent_branch) {
    REQUIRE_X86_HOST();
    ASSERT_TRUE(E != NULL);
    ASSERT_TRUE(emu_map(E, DATA_PAGE, DATA_SIZE));

    size_t baseline = 0;
    size_t after = union_over_variants(E, (const void *)ct_eq, &baseline);
    ASSERT_TRUE(baseline > 0); /* the routine really ran */
    /* The union did not grow: no basic block depended on the differing bytes. */
    ASSERT_EQ(after, baseline);
}

TEST(ct_eq, leaky_eq_control_grows_coverage) {
    REQUIRE_X86_HOST();
    ASSERT_TRUE(E != NULL);
    ASSERT_TRUE(emu_map(E, DATA_PAGE, DATA_SIZE));

    size_t baseline = 0;
    size_t after = union_over_variants(E, (const void *)leaky_eq, &baseline);
    ASSERT_TRUE(baseline > 0);
    /* The control MUST fail the CT property — otherwise the assertion above is
     * measuring nothing. */
    ASSERT_TRUE(after > baseline);
}
