/*
 * test_dataflow.c — the PURE data-flow spine (Phase 0 sink + Phase 1 L1/L2),
 * validated over SYNTHETIC value traces built by hand. No Capstone, no Unicorn, no
 * hardware — these checks run and pass on EVERY CI host, exactly like test_ibs's
 * pure-decoder half.
 *
 *   Phase 0: asmtest_valtrace_append round-trips a fixture and truncates honestly
 *            when a buffer overflows; the wide[] side buffer round-trips.
 *   Phase 1: asmtest_defuse_build reconstructs a known def-use graph (a register
 *            move chain AND a load-after-store chain), and asmtest_slice_forward /
 *            _backward reproduce the hand-derived slices. A raw-address alias is
 *            asserted as a KNOWN limitation (pre GC-canonicalization).
 */
#include "asmtest_valtrace.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* Synthetic register ids (L1 keys on the id, so any distinct integers do). */
enum { RDI = 1, RSI = 2, RA = 10, RB = 11, RC = 12, RD = 13, RSP = 20 };
#define ADDR_A 0x4000ULL

/* Real Capstone x86 GP register ids (T3). Unlike the synthetic ids above —
 * whose only load-bearing property is mutual distinctness — the sub-register
 * alias fixtures below need the REAL ids, because reg_slice (src/dataflow.c)
 * canonicalizes by raw Capstone id. Literal mirrors of capstone/x86.h (pinned
 * Capstone 5.0.1, per CLAUDE.md's dependency rule), the same tactic
 * examples/test_dataflow_ptrace.c already uses for its own REG_* literals and
 * src/dataflow.c uses for reg_slice itself — this file stays Capstone-free. */
#define X86_REG_AH  1
#define X86_REG_AL  2
#define X86_REG_AX  3
#define X86_REG_EAX 19
#define X86_REG_RAX 35
#define X86_REG_R8  106
#define X86_REG_R8D 226

static at_val_rec_t reg(uint32_t id, bool w) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_REG;
    r.reg = id;
    r.is_write = w;
    return r;
}
static at_val_rec_t mem(uint64_t a, uint16_t sz, bool w) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_MEM_ABS;
    r.addr = a;
    r.size = sz;
    r.is_write = w;
    return r;
}

static int has_edge(const asmtest_defuse_t *g, uint32_t from, uint32_t to) {
    for (size_t i = 0; i < g->n; i++)
        if (g->edges[i].from_step == from && g->edges[i].to_step == to)
            return 1;
    return 0;
}

/* ---- Phase 0: sink round-trip + truncate --------------------------------- */
static void test_sink(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(8, 32, 64);
    CHECK(v != NULL, "sink: valtrace_new allocates");
    if (!v)
        return;

    at_val_rec_t s0[2] = {reg(RA, true), reg(RDI, false)};
    at_val_rec_t s1[2] = {mem(ADDR_A, 8, true), reg(RA, false)};
    asmtest_valtrace_append(v, 0x00, s0, 2);
    asmtest_valtrace_append(v, 0x10, s1, 2);

    CHECK(v->steps_len == 2 && v->steps_total == 2, "sink: two steps recorded");
    CHECK(v->recs_len == 4 && v->recs_total == 4,
          "sink: four records recorded");
    CHECK(v->insn_off[0] == 0x00 && v->insn_off[1] == 0x10,
          "sink: per-step offsets round-trip");
    CHECK(!v->truncated, "sink: not truncated within capacity");
    /* records carry their step stamp and survive verbatim */
    CHECK(v->recs[0].step == 0 && v->recs[0].reg == RA && v->recs[0].is_write,
          "sink: record 0 round-trips (step 0, write RA)");
    CHECK(v->recs[2].step == 1 && v->recs[2].kind == AT_LOC_MEM_ABS &&
              v->recs[2].addr == ADDR_A && v->recs[2].is_write,
          "sink: record 2 round-trips (step 1, mem write @A)");

    /* wide[] side buffer round-trips a >8B value */
    uint8_t ymm[32];
    for (int i = 0; i < 32; i++)
        ymm[i] = (uint8_t)(i + 1);
    size_t woff = asmtest_valtrace_stash_wide(v, ymm, sizeof ymm);
    CHECK(woff == 0 && v->wide_len == 32,
          "sink: wide value stashed at offset 0");
    CHECK(memcmp(v->wide + woff, ymm, 32) == 0, "sink: wide value round-trips");

    asmtest_valtrace_free(v);
}

static void test_truncate(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(2, 3, 0);
    if (!v) {
        CHECK(0, "truncate: valtrace_new");
        return;
    }
    at_val_rec_t pair[2] = {reg(RA, true), reg(RB, false)};
    asmtest_valtrace_append(v, 0, pair, 2);
    asmtest_valtrace_append(v, 1, pair, 2);
    asmtest_valtrace_append(v, 2, pair, 2); /* overflows steps AND recs */

    CHECK(v->steps_total == 3 && v->steps_len == 2,
          "truncate: totals honest, steps capped at capacity");
    CHECK(v->recs_len == 3 && v->recs_total == 6,
          "truncate: recs capped, total counts every record");
    CHECK(v->truncated, "truncate: overflow flag set");

    size_t bad = asmtest_valtrace_stash_wide(v, "xxxxx", 5);
    CHECK(bad == (size_t)-1, "truncate: stash_wide with no wide[] fails");
    asmtest_valtrace_free(v);
}

/* ---- Phase 1: def-use + slices over a known chain ------------------------ */
/*
 *  step0:  RA   <- RDI                 (mov)
 *  step1:  M[A] <- RA        (RSP base) (store)
 *  step2:  RC   <- M[A]      (RSP base) (load)
 *  step3:  RD   <- RC (, RSI)           (lea-like)
 *  step4:  RA   <- RD                   (mov)
 *
 * Hand-derived: backward(step4) = forward(step0) = {0,1,2,3,4}. RSI / RDI / RSP
 * have no in-trace writer, so they add no edges.
 */
static asmtest_valtrace_t *build_chain(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(8, 32, 0);
    if (!v)
        return NULL;
    at_val_rec_t s0[] = {reg(RA, true), reg(RDI, false)};
    at_val_rec_t s1[] = {mem(ADDR_A, 8, true), reg(RA, false), reg(RSP, false)};
    at_val_rec_t s2[] = {reg(RC, true), mem(ADDR_A, 8, false), reg(RSP, false)};
    at_val_rec_t s3[] = {reg(RD, true), reg(RC, false), reg(RSI, false)};
    at_val_rec_t s4[] = {reg(RA, true), reg(RD, false)};
    asmtest_valtrace_append(v, 0x00, s0, 2);
    asmtest_valtrace_append(v, 0x03, s1, 3);
    asmtest_valtrace_append(v, 0x08, s2, 3);
    asmtest_valtrace_append(v, 0x0d, s3, 3);
    asmtest_valtrace_append(v, 0x11, s4, 2);
    return v;
}

static void test_defuse_slice(void) {
    asmtest_valtrace_t *v = build_chain();
    if (!v) {
        CHECK(0, "defuse: build_chain");
        return;
    }
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL && g->nsteps == 5, "defuse: graph spans five steps");

    /* the register move chain and the load-after-store edge */
    CHECK(has_edge(g, 0, 1), "defuse: RA move edge step0 -> step1");
    CHECK(has_edge(g, 1, 2),
          "defuse: load-after-store edge step1 -> step2 (M[A])");
    CHECK(has_edge(g, 2, 3), "defuse: RC edge step2 -> step3");
    CHECK(has_edge(g, 3, 4), "defuse: RD edge step3 -> step4");
    CHECK(!has_edge(g, 4, 0), "defuse: no spurious back edge");

    at_val_rec_t seed = reg(0, false);
    seed.step = 0;
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && fwd->n == 5 && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 1) &&
              asmtest_slice_contains(fwd, 2) &&
              asmtest_slice_contains(fwd, 3) && asmtest_slice_contains(fwd, 4),
          "forward slice(step0) = {0,1,2,3,4} (what RDI influences)");

    at_val_rec_t sink = reg(0, false);
    sink.step = 4;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && bwd->n == 5 && asmtest_slice_contains(bwd, 0) &&
              asmtest_slice_contains(bwd, 4),
          "backward slice(step4) = {0,1,2,3,4} (what produced RA)");

    /* a mid-chain backward slice is a strict subset */
    at_val_rec_t mid = reg(0, false);
    mid.step = 2;
    asmtest_slice_t *bmid = asmtest_slice_backward(g, mid);
    CHECK(bmid && bmid->n == 3 && asmtest_slice_contains(bmid, 0) &&
              asmtest_slice_contains(bmid, 1) &&
              asmtest_slice_contains(bmid, 2) &&
              !asmtest_slice_contains(bmid, 3),
          "backward slice(step2) = {0,1,2} (stops before the consumer)");

    asmtest_slice_free(fwd);
    asmtest_slice_free(bwd);
    asmtest_slice_free(bmid);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

/*
 * T1 (dataflow-bindings-slice-codeimage.md): asmtest_slice_forward_seed /
 * _backward_seed (the by-pointer seed variants an FFI that cannot express a
 * by-value 72-byte aggregate needs) must be byte-for-byte equivalent to the
 * by-value slicers they wrap — both read only seed->step. A parity check
 * across every step index of a known graph is a complete proof over that
 * graph, not a sample.
 */
static void test_slice_seed_parity(void) {
    asmtest_valtrace_t *v = build_chain();
    if (!v) {
        CHECK(0, "slice_seed: build_chain");
        return;
    }
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    if (!g) {
        CHECK(0, "slice_seed: defuse_build");
        asmtest_valtrace_free(v);
        return;
    }

    int fwd_ok = 1, bwd_ok = 1;
    for (uint32_t k = 0; k < (uint32_t)g->nsteps; k++) {
        at_val_rec_t seed = reg(0, false);
        seed.step = k;

        asmtest_slice_t *fwd_val = asmtest_slice_forward(g, seed);
        asmtest_slice_t *fwd_ptr = asmtest_slice_forward_seed(g, &seed);
        if (!fwd_val || !fwd_ptr || fwd_val->n != fwd_ptr->n) {
            fwd_ok = 0;
        } else {
            for (uint32_t s = 0; s < (uint32_t)g->nsteps; s++)
                if (asmtest_slice_contains(fwd_val, s) !=
                    asmtest_slice_contains(fwd_ptr, s))
                    fwd_ok = 0;
        }
        asmtest_slice_free(fwd_val);
        asmtest_slice_free(fwd_ptr);

        asmtest_slice_t *bwd_val = asmtest_slice_backward(g, seed);
        asmtest_slice_t *bwd_ptr = asmtest_slice_backward_seed(g, &seed);
        if (!bwd_val || !bwd_ptr || bwd_val->n != bwd_ptr->n) {
            bwd_ok = 0;
        } else {
            for (uint32_t s = 0; s < (uint32_t)g->nsteps; s++)
                if (asmtest_slice_contains(bwd_val, s) !=
                    asmtest_slice_contains(bwd_ptr, s))
                    bwd_ok = 0;
        }
        asmtest_slice_free(bwd_val);
        asmtest_slice_free(bwd_ptr);
    }
    CHECK(fwd_ok,
          "slice_seed: asmtest_slice_forward_seed matches "
          "asmtest_slice_forward for every step index (element-for-element, "
          "via slice_contains)");
    CHECK(bwd_ok, "slice_seed: asmtest_slice_backward_seed matches "
                  "asmtest_slice_backward for every step index");

    /* Documented contract: a NULL seed is treated as step 0. */
    asmtest_slice_t *null_fwd = asmtest_slice_forward_seed(g, NULL);
    at_val_rec_t step0 = reg(0, false);
    step0.step = 0;
    asmtest_slice_t *step0_fwd = asmtest_slice_forward(g, step0);
    CHECK(null_fwd && step0_fwd && null_fwd->n == step0_fwd->n,
          "slice_seed: a NULL seed is treated as step 0");
    asmtest_slice_free(null_fwd);
    asmtest_slice_free(step0_fwd);

    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

/*
 * Documented KNOWN LIMITATION (plan Phase 1 exit criterion): two logically
 * distinct objects that occupy the SAME raw address (e.g. after a GC move, before
 * canonicalization) alias in the memory last-writer map, so a store to the first
 * and a load "of the second" are linked by a FALSE dependence edge. Asserting the
 * false positive pins the behaviour until the Phase 4 GC-canonicalization layer.
 */
static void test_alias_limitation(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(4, 8, 0);
    if (!v) {
        CHECK(0, "alias: valtrace_new");
        return;
    }
    at_val_rec_t st[] = {mem(ADDR_A, 8, true)};  /* object1.field = ... */
    at_val_rec_t ld[] = {mem(ADDR_A, 8, false)}; /* object2.field (aliased) */
    asmtest_valtrace_append(v, 0x00, st, 1);
    asmtest_valtrace_append(v, 0x08, ld, 1);
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(
        g && has_edge(g, 0, 1),
        "alias: raw-address collision links store->load (KNOWN false positive "
        "until GC canonicalization)");
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

/*
 * T3 — sub-register-aware last-writer resolution (src/dataflow.c's reg_slice).
 * Three synthetic traces, each built and torn down independently so a failure
 * in one can't shadow the others:
 *
 * (a) write eax -> read ax: raw-id keying (pre-T3) would MISS this edge (EAX
 *     and AX are distinct Capstone ids); reg_slice's container-byte folding
 *     must produce it.
 * (b) write ah -> read al: the DISCRIMINATOR. AH and AL are distinct
 *     Capstone ids AND distinct bytes of the same rax container (AH is byte
 *     offset 1, AL is byte offset 0). A container-collapsing implementation
 *     — one that folds by container alone and ignores the byte offset/width
 *     reg_slice computes — would wrongly link them; asserting NO edge here
 *     catches exactly that bug (proven by mutation below, not just asserted).
 *     A companion positive check (write ah -> read ah) confirms the AH write
 *     really was recorded, so the negative check isn't vacuously true because
 *     nothing was written at all.
 * (c) write r8d -> read r8: the r8-r15 family's twin of (a) (T1 fixed the
 *     PTRACE PRODUCER's value capture for this family; this is the SHARED
 *     BUILDER's independent edge-resolution path).
 * (d) write rax -> write eax -> read rax: the ZERO-EXTENSION discriminator.
 *     x86-64 defines a 32-bit GP write as implicitly clearing bits 32-63 of
 *     the 64-bit container — unlike a 16/8-bit write, which leaves the
 *     untouched bytes exactly as they were. A byte-range-only implementation
 *     that widens WRITES no further than reg_slice's 4-byte `len` for EAX
 *     would leave the container's upper half attributed to the STALE step0
 *     write, fabricating a phantom edge 0->2 alongside the real 1->2 one —
 *     exactly the class of bug this task's whole point is to eliminate,
 *     just relocated from "missing edge" to "extra fabricated edge".
 */
static void test_subreg_alias(void) {
    /* (a) write eax -> read ax produces the edge. */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(4, 4, 0);
        if (!v) {
            CHECK(0, "subreg: (a) valtrace_new");
        } else {
            at_val_rec_t s0[] = {reg(X86_REG_EAX, true)};
            at_val_rec_t s1[] = {reg(X86_REG_AX, false)};
            asmtest_valtrace_append(v, 0x00, s0, 1);
            asmtest_valtrace_append(v, 0x03, s1, 1);
            asmtest_defuse_t *g = asmtest_defuse_build(v);
            CHECK(g && has_edge(g, 0, 1),
                  "subreg(a): write eax -> read ax resolves the cross-alias "
                  "edge (raw-id keying would miss it: EAX != AX)");
            asmtest_defuse_free(g);
            asmtest_valtrace_free(v);
        }
    }

    /* (b) write ah -> read al produces NO edge (byte-slice precision). */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(4, 4, 0);
        if (!v) {
            CHECK(0, "subreg: (b) valtrace_new");
        } else {
            at_val_rec_t s0[] = {reg(X86_REG_AH, true)};
            at_val_rec_t s1[] = {reg(X86_REG_AH, false)};
            at_val_rec_t s2[] = {reg(X86_REG_AL, false)};
            asmtest_valtrace_append(v, 0x00, s0, 1);
            asmtest_valtrace_append(v, 0x02, s1, 1);
            asmtest_valtrace_append(v, 0x04, s2, 1);
            asmtest_defuse_t *g = asmtest_defuse_build(v);
            CHECK(g && has_edge(g, 0, 1),
                  "subreg(b) positive control: write ah -> read ah DOES "
                  "resolve (the write was really recorded)");
            CHECK(g && !has_edge(g, 0, 2),
                  "subreg(b) DISCRIMINATOR: write ah -> read al produces NO "
                  "edge (al is byte offset 0 of rax, ah is byte offset 1 -- "
                  "a container-collapsing implementation fails this)");
            asmtest_defuse_free(g);
            asmtest_valtrace_free(v);
        }
    }

    /* (c) write r8d -> read r8 produces the edge (the r8-r15 family twin of
     * (a); T1's fixture covers the ptrace producer's value capture for this
     * family, this covers the shared builder's edge resolution). */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(4, 4, 0);
        if (!v) {
            CHECK(0, "subreg: (c) valtrace_new");
        } else {
            at_val_rec_t s0[] = {reg(X86_REG_R8D, true)};
            at_val_rec_t s1[] = {reg(X86_REG_R8, false)};
            asmtest_valtrace_append(v, 0x00, s0, 1);
            asmtest_valtrace_append(v, 0x03, s1, 1);
            asmtest_defuse_t *g = asmtest_defuse_build(v);
            CHECK(g && has_edge(g, 0, 1),
                  "subreg(c): write r8d -> read r8 resolves the cross-alias "
                  "edge (the r8-r15 family's twin of (a))");
            asmtest_defuse_free(g);
            asmtest_valtrace_free(v);
        }
    }

    /* (d) write rax -> write eax -> read rax: only step1 (the eax write)
     * produces an edge; step0's write is architecturally stale for EVERY
     * byte of the container, including the upper half eax's own 4 bytes
     * never explicitly touch. */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(4, 4, 0);
        if (!v) {
            CHECK(0, "subreg: (d) valtrace_new");
        } else {
            at_val_rec_t s0[] = {reg(X86_REG_RAX, true)};
            at_val_rec_t s1[] = {reg(X86_REG_EAX, true)};
            at_val_rec_t s2[] = {reg(X86_REG_RAX, false)};
            asmtest_valtrace_append(v, 0x00, s0, 1);
            asmtest_valtrace_append(v, 0x03, s1, 1);
            asmtest_valtrace_append(v, 0x06, s2, 1);
            asmtest_defuse_t *g = asmtest_defuse_build(v);
            CHECK(g && has_edge(g, 1, 2),
                  "subreg(d): write eax -> read rax resolves (the "
                  "zero-extended write is a real producer of the FULL "
                  "container)");
            CHECK(g && !has_edge(g, 0, 2),
                  "subreg(d) DISCRIMINATOR: write rax -> write eax -> read "
                  "rax produces NO edge from the stale rax write -- a "
                  "4-byte-only write range for eax would fabricate this "
                  "edge from the upper half's zero-extension side effect");
            asmtest_defuse_free(g);
            asmtest_valtrace_free(v);
        }
    }
}

int main(void) {
    test_sink();
    test_truncate();
    test_defuse_slice();
    test_slice_seed_parity();
    test_alias_limitation();
    test_subreg_alias();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
