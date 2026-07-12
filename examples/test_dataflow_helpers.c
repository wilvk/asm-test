/*
 * test_dataflow_helpers.c — Phase 4 (increment 3): runtime-helper SUMMARY EDGES,
 * validated over SYNTHETIC value traces + a synthetic method-map/helper-table
 * built by hand. PURE — no Capstone, no Unicorn, no runtime, no .NET SDK — so it
 * runs and passes on EVERY CI host, exactly like test_dataflow's pure spine. It
 * probes the plan's Phase 4 "runtime-helper edges" bullet: model a recognized
 * helper as a summary edge (input args -> output) instead of descending into it.
 *
 *   match:   asmtest_helper_match resolves exact + trailing-'*' prefix patterns
 *            and the built-in table recognizes the three canonical helper names.
 *   alloc:   a reg->reg alloc summary connects the caller's arg def to its return
 *            use ACROSS the helper — and the helper BODY steps are excluded from
 *            the slice (raw def-use threads them; the summary does not).
 *   barrier: a MEM_AT_REG write-barrier summary flows the stored reference value
 *            into the destination field in memory, so a later load of that field
 *            is connected — again without the body.
 *   unknown: an unrecognized call is NOT summarized (its body is descended), so
 *            the pass never fabricates a summary edge (conservative).
 *   guards:  NULL trace, and NULL/empty table degrading to a plain def-use.
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
enum { ARG = 1, RET = 2, PTR = 3, VAL = 4, SCRATCH = 5, RD = 6 };
#define FIELD_ADDR 0x7F0000ULL

static at_val_rec_t reg(uint32_t id, bool w) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_REG;
    r.reg = id;
    r.is_write = w;
    return r;
}
/* A register write carrying a captured value (drives MEM_AT_REG resolution). */
static at_val_rec_t reg_val(uint32_t id, uint64_t value) {
    at_val_rec_t r = reg(id, true);
    r.value = value;
    r.value_valid = true;
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
    if (g == NULL)
        return 0;
    for (size_t i = 0; i < g->n; i++)
        if (g->edges[i].from_step == from && g->edges[i].to_step == to)
            return 1;
    return 0;
}

/* A synthetic method-map: the caller Foo, plus the helper bodies at their own
 * addresses. Names are what the resolver hands the helper-table matcher. */
#define FOO_ADDR  0x1000ULL
#define FOO_SIZE  0x100ULL
#define HELP_ADDR 0x8000ULL
#define HELP_SIZE 0x0040ULL

/* ---- name matching: exact, prefix, and the built-in table ---------------- */
static void test_match(void) {
    static const asmtest_helper_loc_t in[] = {{AT_HELPER_REG, ARG, 0}};
    static const asmtest_helper_loc_t out[] = {{AT_HELPER_REG, RET, 0}};
    const asmtest_helper_t tbl[] = {
        {"Exact.Helper", in, 1, out, 1},
        {"CORINFO_HELP_NEW*", in, 1, out, 1},
    };
    CHECK(asmtest_helper_match(tbl, 2, "Exact.Helper") == 0,
          "match: exact name resolves to its entry");
    CHECK(asmtest_helper_match(tbl, 2, "Exact.Helperr") == -1,
          "match: a longer string is NOT an exact match");
    CHECK(asmtest_helper_match(tbl, 2, "CORINFO_HELP_NEWSFAST") == 1,
          "match: trailing '*' matches by prefix (NEWSFAST)");
    CHECK(asmtest_helper_match(tbl, 2, "CORINFO_HELP_NEWARR_1") == 1,
          "match: the same prefix matches another suffix (NEWARR_1)");
    CHECK(asmtest_helper_match(tbl, 2, "CORINFO_HELP_BOX") == -1,
          "match: a different suffixed helper does not match the NEW* prefix");
    CHECK(asmtest_helper_match(tbl, 2, "System.Object:.ctor()") == -1,
          "match: an ordinary managed method is not a helper");
    CHECK(asmtest_helper_match(tbl, 2, NULL) == -1, "match: NULL name -> -1");
    CHECK(asmtest_helper_match(NULL, 0, "Exact.Helper") == -1,
          "match: NULL table -> -1");

    /* the built-in representative table recognizes the three helper shapes */
    size_t n = 0;
    const asmtest_helper_t *def = asmtest_helper_default_table(&n);
    CHECK(def != NULL && n >= 3, "match: default table is non-empty");
    CHECK(asmtest_helper_match(def, n, "CORINFO_HELP_NEWSFAST") >= 0,
          "match: default table recognizes the allocation helper");
    CHECK(asmtest_helper_match(def, n, "JIT_WriteBarrier") >= 0,
          "match: default table recognizes the write-barrier");
    CHECK(asmtest_helper_match(def, n, "JIT_GenericHandleMethod") >= 0,
          "match: default table recognizes the generic-dictionary lookup");
    CHECK(asmtest_helper_match(def, n, "System.String:Concat(...)") == -1,
          "match: default table does not summarize an ordinary method");
    /* the alloc entry's contract: MethodTable arg (RDI=39) -> object in RAX=35 */
    int ai = asmtest_helper_match(def, n, "CORINFO_HELP_NEWFAST");
    CHECK(ai >= 0 && def[ai].n_in == 1 && def[ai].ins[0].reg == 39 &&
              def[ai].n_out == 1 && def[ai].outs[0].reg == 35,
          "match: default alloc contract is RDI -> RAX (SysV reg ids)");
}

/*
 * The alloc trace (one method-map: Foo the caller, AllocHelper the body):
 *   step0: Foo   ARG <- size                (caller computes the alloc arg)
 *   step1: Alloc reads ARG, writes SCRATCH  (helper body, internal)
 *   step2: Alloc writes RET, reads SCRATCH   (helper body, "mov rax, obj")
 *   step3: Alloc reads SCRATCH (ret path)    (helper body, internal)
 *   step4: Foo   RD <- RET                   (caller uses the new object)
 *
 * RAW def-use threads the caller through the body: 4<-2<-1<-0, so the backward
 * slice of step4 INCLUDES body step2. The alloc SUMMARY replaces the body with a
 * single node at step1 (read ARG, write RET): 0->1 (arg in) and 1->4 (object out),
 * so the caller connects ACROSS the helper and the body steps drop out of the slice.
 */
static const asmtest_method_t ALLOC_MAP[] = {
    {FOO_ADDR, FOO_SIZE, "Foo", 1},
    {HELP_ADDR, HELP_SIZE, "AllocHelper", 1},
};
enum { ALLOC_NMAP = (int)(sizeof ALLOC_MAP / sizeof ALLOC_MAP[0]) };

static asmtest_valtrace_t *build_alloc_trace(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(8, 32, 0);
    if (!v)
        return NULL;
    at_val_rec_t s0[] = {reg(ARG, true)};
    at_val_rec_t s1[] = {reg(ARG, false), reg(SCRATCH, true)};
    at_val_rec_t s2[] = {reg(RET, true), reg(SCRATCH, false)};
    at_val_rec_t s3[] = {reg(SCRATCH, false)};
    at_val_rec_t s4[] = {reg(RD, true), reg(RET, false)};
    asmtest_valtrace_append(v, FOO_ADDR + 0x00, s0, 1);
    asmtest_valtrace_append(v, HELP_ADDR + 0x00, s1, 2);
    asmtest_valtrace_append(v, HELP_ADDR + 0x10, s2, 2);
    asmtest_valtrace_append(v, HELP_ADDR + 0x20, s3, 1);
    asmtest_valtrace_append(v, FOO_ADDR + 0x10, s4, 2);
    return v;
}

static void test_alloc_summary(void) {
    static const asmtest_helper_loc_t in[] = {{AT_HELPER_REG, ARG, 0}};
    static const asmtest_helper_loc_t out[] = {{AT_HELPER_REG, RET, 0}};
    const asmtest_helper_t helpers[] = {{"AllocHelper", in, 1, out, 1}};

    asmtest_valtrace_t *v = build_alloc_trace();
    if (!v) {
        CHECK(0, "alloc: build_alloc_trace");
        return;
    }

    /* (a) RAW def-use threads the caller THROUGH the helper body. */
    asmtest_defuse_t *raw = asmtest_defuse_build(v);
    CHECK(raw && has_edge(raw, 2, 4),
          "alloc/raw: caller reads RET from the body's internal write (2->4)");
    at_val_rec_t sink = reg(0, false);
    sink.step = 4;
    asmtest_slice_t *rslice = asmtest_slice_backward(raw, sink);
    CHECK(rslice && asmtest_slice_contains(rslice, 2),
          "alloc/raw: backward slice of the return use includes body step2");
    asmtest_slice_free(rslice);
    asmtest_defuse_free(raw);

    /* (b) SUMMARIZED: the helper collapses to a node at its entry step1. */
    asmtest_defuse_t *g =
        asmtest_defuse_build_summarized(v, ALLOC_MAP, ALLOC_NMAP, helpers, 1);
    CHECK(g && g->nsteps == 5,
          "alloc: summarized graph still spans five steps");
    CHECK(has_edge(g, 0, 1),
          "alloc: caller ARG def flows into the helper summary (0->1)");
    CHECK(has_edge(g, 1, 4),
          "alloc: helper output (RET) flows to the caller's use (1->4)");
    CHECK(!has_edge(g, 2, 4),
          "alloc: the body's internal RET write is gone (no 2->4)");

    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && asmtest_slice_contains(bwd, 0) &&
              asmtest_slice_contains(bwd, 1) && asmtest_slice_contains(bwd, 4),
          "alloc: return use traces back to the arg def across the summary");
    CHECK(bwd && !asmtest_slice_contains(bwd, 2) &&
              !asmtest_slice_contains(bwd, 3),
          "alloc: the helper BODY steps are excluded from the slice");
    CHECK(bwd && bwd->n == 3,
          "alloc: the slice is exactly {0,1,4} (caller + summary, no body)");
    asmtest_slice_free(bwd);

    /* forward from the arg def reaches the caller's use through the summary */
    at_val_rec_t seed = reg(0, false);
    seed.step = 0;
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && asmtest_slice_contains(fwd, 4),
          "alloc: forward slice of the arg def reaches the return use");
    asmtest_slice_free(fwd);

    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

/*
 * The write-barrier trace (Foo the caller, WriteBarrier the body):
 *   step0: Foo   PTR <- &field  (value = FIELD_ADDR, captured)   [lea]
 *   step1: Foo   VAL <- ref      (the reference being stored; the source)
 *   step2: WB    reads PTR/VAL, writes M[FIELD_ADDR] (body, internal store)
 *   step3: WB    card-table update (body, internal)
 *   step4: Foo   RD <- M[FIELD_ADDR]  (a later load of the same field)
 *
 * The barrier's contract is VAL -> memory at [PTR]. Summarized, step2 becomes a
 * node reading VAL and writing M[FIELD_ADDR] (address resolved from PTR's captured
 * value), so the stored value connects to the later load (1->2->4) with the body
 * dropped.
 */
static const asmtest_method_t WB_MAP[] = {
    {FOO_ADDR, FOO_SIZE, "Foo", 1},
    {HELP_ADDR, HELP_SIZE, "WriteBarrier", 1},
};
enum { WB_NMAP = (int)(sizeof WB_MAP / sizeof WB_MAP[0]) };

static asmtest_valtrace_t *build_barrier_trace(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(8, 32, 0);
    if (!v)
        return NULL;
    at_val_rec_t s0[] = {reg_val(PTR, FIELD_ADDR)};
    at_val_rec_t s1[] = {reg(VAL, true)};
    at_val_rec_t s2[] = {reg(PTR, false), reg(VAL, false),
                         mem(FIELD_ADDR, 8, true)};
    at_val_rec_t s3[] = {reg(SCRATCH, true)};
    at_val_rec_t s4[] = {reg(RD, true), mem(FIELD_ADDR, 8, false)};
    asmtest_valtrace_append(v, FOO_ADDR + 0x00, s0, 1);
    asmtest_valtrace_append(v, FOO_ADDR + 0x08, s1, 1);
    asmtest_valtrace_append(v, HELP_ADDR + 0x00, s2, 3);
    asmtest_valtrace_append(v, HELP_ADDR + 0x10, s3, 1);
    asmtest_valtrace_append(v, FOO_ADDR + 0x20, s4, 2);
    return v;
}

static void test_barrier_summary(void) {
    static const asmtest_helper_loc_t in[] = {{AT_HELPER_REG, VAL, 0}};
    static const asmtest_helper_loc_t out[] = {{AT_HELPER_MEM_AT_REG, PTR, 8}};
    const asmtest_helper_t helpers[] = {{"WriteBarrier", in, 1, out, 1}};

    asmtest_valtrace_t *v = build_barrier_trace();
    if (!v) {
        CHECK(0, "barrier: build_barrier_trace");
        return;
    }

    asmtest_defuse_t *g =
        asmtest_defuse_build_summarized(v, WB_MAP, WB_NMAP, helpers, 1);
    CHECK(g != NULL, "barrier: summarized graph built");
    CHECK(has_edge(g, 1, 2),
          "barrier: the stored reference value flows into the summary (1->2)");
    CHECK(has_edge(g, 2, 4),
          "barrier: the barrier's memory write reaches the later load (2->4)");

    /* taint of the stored value reaches the later field load, across the body */
    at_val_rec_t sink = reg(0, false);
    sink.step = 4;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && asmtest_slice_contains(bwd, 1) &&
              asmtest_slice_contains(bwd, 2) && asmtest_slice_contains(bwd, 4),
          "barrier: the later load traces back to the stored value");
    CHECK(bwd && !asmtest_slice_contains(bwd, 3),
          "barrier: the body-only card-table step is excluded");
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);

    /* When the pointer register carries NO captured value, the MEM_AT_REG output
     * is skipped (conservative): no memory write, so no 2->4 edge is fabricated. */
    asmtest_valtrace_t *vu = asmtest_valtrace_new(8, 32, 0);
    if (vu) {
        at_val_rec_t u0[] = {reg(PTR, true)}; /* no value_valid */
        at_val_rec_t u1[] = {reg(VAL, true)};
        at_val_rec_t u2[] = {reg(PTR, false), reg(VAL, false)};
        at_val_rec_t u4[] = {reg(RD, true), mem(FIELD_ADDR, 8, false)};
        asmtest_valtrace_append(vu, FOO_ADDR + 0x00, u0, 1);
        asmtest_valtrace_append(vu, FOO_ADDR + 0x08, u1, 1);
        asmtest_valtrace_append(vu, HELP_ADDR + 0x00, u2, 2);
        asmtest_valtrace_append(vu, FOO_ADDR + 0x20, u4, 2);
        asmtest_defuse_t *gu =
            asmtest_defuse_build_summarized(vu, WB_MAP, WB_NMAP, helpers, 1);
        CHECK(gu && !has_edge(gu, 2, 3) && !has_edge(gu, 1, 3),
              "barrier: an unknown pointer value skips the memory output "
              "(no fabricated edge)");
        asmtest_defuse_free(gu);
        asmtest_valtrace_free(vu);
    } else {
        CHECK(0, "barrier: valtrace_new (unknown-pointer case)");
    }

    asmtest_valtrace_free(v);
}

/* ---- an UNRECOGNIZED call is descended, never summarized ------------------ */
static void test_unknown_not_summarized(void) {
    /* Same trace shape as the alloc case, but the helper table only knows
     * "AllocHelper" while the body resolves to "MysteryHelper" — so nothing is
     * summarized and the summarized graph EQUALS the raw descent. */
    static const asmtest_helper_loc_t in[] = {{AT_HELPER_REG, ARG, 0}};
    static const asmtest_helper_loc_t out[] = {{AT_HELPER_REG, RET, 0}};
    const asmtest_helper_t helpers[] = {{"AllocHelper", in, 1, out, 1}};

    static const asmtest_method_t map[] = {
        {FOO_ADDR, FOO_SIZE, "Foo", 1},
        {HELP_ADDR, HELP_SIZE, "MysteryHelper", 1}, /* not in the table */
    };

    asmtest_valtrace_t *v = build_alloc_trace();
    if (!v) {
        CHECK(0, "unknown: build_alloc_trace");
        return;
    }
    asmtest_defuse_t *g =
        asmtest_defuse_build_summarized(v, map, 2, helpers, 1);
    /* descended, not summarized: the body's real edges survive ... */
    CHECK(has_edge(g, 2, 4),
          "unknown: unrecognized body is descended (body edge 2->4 kept)");
    /* ... and NO summary edge is invented (the alloc summary would have made
     * 1->4 with the body dropped; here it must be absent). */
    CHECK(!has_edge(g, 1, 4),
          "unknown: no summary edge is fabricated for an unrecognized call");

    at_val_rec_t sink = reg(0, false);
    sink.step = 4;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && asmtest_slice_contains(bwd, 2),
          "unknown: the body step stays in the slice (full descent)");
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

/* ---- NULL / empty-table guards ------------------------------------------- */
static void test_guards(void) {
    static const asmtest_helper_loc_t in[] = {{AT_HELPER_REG, ARG, 0}};
    static const asmtest_helper_loc_t out[] = {{AT_HELPER_REG, RET, 0}};
    const asmtest_helper_t helpers[] = {{"AllocHelper", in, 1, out, 1}};

    CHECK(asmtest_defuse_build_summarized(NULL, ALLOC_MAP, ALLOC_NMAP, helpers,
                                          1) == NULL,
          "guard: NULL trace -> NULL");

    /* A NULL/empty helper table (or method-map) degrades to a plain def-use: the
     * summarized graph equals asmtest_defuse_build over the same trace. */
    asmtest_valtrace_t *v = build_alloc_trace();
    if (!v) {
        CHECK(0, "guard: build_alloc_trace");
        return;
    }
    asmtest_defuse_t *plain = asmtest_defuse_build(v);
    asmtest_defuse_t *g0 =
        asmtest_defuse_build_summarized(v, ALLOC_MAP, ALLOC_NMAP, NULL, 0);
    asmtest_defuse_t *g1 =
        asmtest_defuse_build_summarized(v, NULL, 0, helpers, 1);
    CHECK(plain && g0 && g0->n == plain->n && has_edge(g0, 2, 4),
          "guard: empty helper table -> plain def-use (body descended)");
    CHECK(g1 && g1->n == plain->n && has_edge(g1, 2, 4),
          "guard: empty method-map -> plain def-use (nothing to resolve)");
    asmtest_defuse_free(plain);
    asmtest_defuse_free(g0);
    asmtest_defuse_free(g1);
    asmtest_valtrace_free(v);
}

int main(void) {
    test_match();
    test_alloc_summary();
    test_barrier_summary();
    test_unknown_not_summarized();
    test_guards();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
