/* test_view.c — headless unit test for the data-flow view's pure logic.
 *
 * The Increment-7 "Data flow" TUI window (asmspy.c mode 9) can't be driven in
 * CI, so its non-ncurses payoff logic is factored into asmspy_dataview.h and
 * exercised here against HAND-BUILT value traces (no ptrace, no Capstone):
 *
 *   - asmspy_df_annotate      — the per-step "->0x2a" / "[0xEA]<-0xV" annotation
 *   - asmspy_df_defuse_counts — the reads-here vs written-here-read-later split
 *   - asmspy_df_rowstyle      — the L2 slice highlight/dim decision
 *   - asmspy_df_loc_str       — the def-use location label
 *
 * The slices/def-use themselves come from the library (asmtest_defuse_build /
 * asmtest_slice_*), so this also cross-checks that the view seeds and reads them
 * the right way round. Built + run by `make cli-smoke` before the ptrace smoke.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "asmspy_dataview.h"

static int failures;

static void check_str(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", name, got, want);
        failures++;
    }
}
static void check_sz(const char *name, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %zu, want %zu\n", name, got, want);
        failures++;
    }
}
static void check_int(const char *name, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %d, want %d\n", name, got, want);
        failures++;
    }
}

/* ---- record builders (mirror what a producer fills) ---- */
static at_val_rec_t reg(uint32_t id, uint64_t val, bool write, bool valid) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_REG;
    r.reg = id;
    r.is_write = write;
    r.value_valid = valid;
    r.value = val;
    r.size = 8;
    return r;
}
static at_val_rec_t mem(uint64_t addr, uint64_t val, bool write, bool valid) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_MEM_ABS;
    r.addr = addr;
    r.is_write = write;
    r.value_valid = valid;
    r.value = val;
    r.size = 8;
    return r;
}

/* Register ids — REAL, distinct 64-bit Capstone GP containers, NOT "any distinct
 * integers". Since dataflow-producer-correctness T3 (48b3c92), asmtest_defuse_build
 * canonicalizes register keys through src/dataflow.c's reg_slice, which folds a
 * sub-register alias into its 64-bit container. The old arbitrary ids 1/2/3/4 are
 * Capstone AH/AL/AX/BH — three of them alias inside RAX — so they stopped behaving
 * as four independent registers (the AX write at step 2 clobbers the AH byte the
 * read at step 3 wanted, moving edge 0->3 to 2->3 and growing both slices). Full
 * 64-bit registers each own a distinct container. Literal mirrors of capstone/x86.h
 * (pinned Capstone 5.0.1), the same tactic examples/test_dataflow.c uses to stay
 * Capstone-free. */
#define RA    35 /* X86_REG_RAX */
#define RC    37 /* X86_REG_RBX */
#define RS    38 /* X86_REG_RCX */
#define RD    40 /* X86_REG_RDX */
#define MEM_M 0x601000ULL

/* Build a 5-step trace with a sink chain (0 -> 3 -> 4, the last two through a
 * memory store/load) and an INDEPENDENT chain (1 -> 2), so slices are proper
 * subsets rather than the whole trace:
 *   0: RA = 0x2a
 *   1: RC = 0x5                     (independent)
 *   2: RS = RC                      (reads RC)
 *   3: [M] = RA                     (reads RA, stores to memory)
 *   4: RD = [M]                     (loads from memory)  <- the sink
 * Def-use edges: {1->2, 0->3, 3->4}. */
static asmtest_valtrace_t *build_trace(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(16, 64, 64);
    at_val_rec_t s0[] = {reg(RA, 0x2a, true, true)};
    at_val_rec_t s1[] = {reg(RC, 0x5, true, true)};
    at_val_rec_t s2[] = {reg(RC, 0x5, false, true), reg(RS, 0x5, true, true)};
    at_val_rec_t s3[] = {reg(RA, 0x2a, false, true),
                         mem(MEM_M, 0x2a, true, true)};
    at_val_rec_t s4[] = {mem(MEM_M, 0x2a, false, true),
                         reg(RD, 0x2a, true, true)};
    asmtest_valtrace_append(v, 0x0, s0, 1);
    asmtest_valtrace_append(v, 0x4, s1, 1);
    asmtest_valtrace_append(v, 0x9, s2, 2);
    asmtest_valtrace_append(v, 0xd, s3, 2);
    asmtest_valtrace_append(v, 0x14, s4, 2);
    return v;
}

static void test_annotate(void) {
    asmtest_valtrace_t *v = build_trace();
    /* threaded cursor across a top-down walk (as the renderer does) */
    size_t cur = 0;
    char a[192];
    size_t seen;
    seen = asmspy_df_annotate(v, 0, &cur, 6, a, sizeof a);
    check_str("annot#0", a, "->0x2a");
    check_sz("annot#0 nrec", seen, 1);
    seen = asmspy_df_annotate(v, 1, &cur, 6, a, sizeof a);
    check_str("annot#1", a, "->0x5");
    check_sz("annot#1 nrec", seen, 1);
    seen = asmspy_df_annotate(v, 2, &cur, 6, a, sizeof a);
    check_str("annot#2", a, "->0x5"); /* the RC read is the disasm's operand */
    check_sz("annot#2 nrec", seen, 2);
    seen = asmspy_df_annotate(v, 3, &cur, 6, a, sizeof a);
    check_str("annot#3", a, "[0x601000]<-0x2a"); /* memory STORE */
    seen = asmspy_df_annotate(v, 4, &cur, 6, a, sizeof a);
    check_str("annot#4", a, "[0x601000]->0x2a ->0x2a"); /* LOAD + reg write */
    check_sz("annot#4 nrec", seen, 2);

    /* a one-off lookup (fresh cursor) must match the threaded walk */
    size_t one = 0;
    asmspy_df_annotate(v, 3, &one, 6, a, sizeof a);
    check_str("annot#3 one-off", a, "[0x601000]<-0x2a");

    asmtest_valtrace_free(v);
}

/* Edge-case annotation grammar on a separate small trace. */
static void test_annotate_edges(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(16, 64, 0);
    at_val_rec_t wide = reg(RA, 0, true, true);
    wide.wide = true; /* an XMM/YMM result: value omitted */
    at_val_rec_t s0[] = {wide};
    at_val_rec_t s1[] = {mem(0xdead00, 0, false, false)}; /* load, no value */
    at_val_rec_t s2[] = {mem(0xbeef00, 0, true, false)};  /* store, no value */
    at_val_rec_t s3[] = {reg(RC, 0, false, true)};        /* a lone read */
    at_val_rec_t s4[] = {reg(RD, 0, true, false)};        /* write, no value */
    at_val_rec_t many[8];
    for (int i = 0; i < 8; i++)
        many[i] = reg((uint32_t)(10 + i), (uint64_t)i, true, true);
    asmtest_valtrace_append(v, 0, s0, 1);
    asmtest_valtrace_append(v, 1, s1, 1);
    asmtest_valtrace_append(v, 2, s2, 1);
    asmtest_valtrace_append(v, 3, s3, 1);
    asmtest_valtrace_append(v, 4, s4, 1);
    asmtest_valtrace_append(v, 5, many, 8);

    char a[192];
    size_t one = 0;
    asmspy_df_annotate(v, 0, &one, 6, a, sizeof a);
    check_str("annot wide", a, "->[wide]");
    one = 0;
    asmspy_df_annotate(v, 1, &one, 6, a, sizeof a);
    check_str("annot load-noval", a, "[0xdead00]->?");
    one = 0;
    asmspy_df_annotate(v, 2, &one, 6, a, sizeof a);
    check_str("annot store-noval", a, "[0xbeef00]<-?");
    one = 0;
    asmspy_df_annotate(v, 3, &one, 6, a, sizeof a);
    check_str("annot lone-read", a, ""); /* a register read shows nothing */
    one = 0;
    asmspy_df_annotate(v, 4, &one, 6, a, sizeof a);
    check_str("annot write-noval", a, ""); /* no captured value -> nothing */
    /* 8 register writes with a cap of 6 -> six tokens then "..." */
    one = 0;
    asmspy_df_annotate(v, 5, &one, 6, a, sizeof a);
    check_str("annot overflow", a, "->0x0 ->0x1 ->0x2 ->0x3 ->0x4 ->0x5 ...");

    /* NULL trace / zero cap are safe no-ops */
    a[0] = 'x';
    check_sz("annot null", asmspy_df_annotate(NULL, 0, &one, 6, a, sizeof a),
             0);
    check_str("annot null empties", a, "");

    asmtest_valtrace_free(v);
}

static void test_defuse_counts(void) {
    asmtest_valtrace_t *v = build_trace();
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    /* the trace was designed to yield exactly {1->2, 0->3, 3->4} */
    check_sz("edge count", g->n, 3);

    size_t in, out;
    asmspy_df_defuse_counts(g, 0, &in, &out);
    check_sz("du#0 in", in, 0);
    check_sz("du#0 out", out, 1);
    asmspy_df_defuse_counts(g, 1, &in, &out);
    check_sz("du#1 in", in, 0);
    check_sz("du#1 out", out, 1);
    asmspy_df_defuse_counts(g, 2, &in, &out);
    check_sz("du#2 in", in, 1);
    check_sz("du#2 out", out, 0);
    asmspy_df_defuse_counts(g, 3, &in, &out);
    check_sz("du#3 in", in, 1);
    check_sz("du#3 out", out, 1);
    asmspy_df_defuse_counts(g, 4, &in, &out);
    check_sz("du#4 in", in, 1);
    check_sz("du#4 out", out, 0);

    /* NULL graph and NULL out-pointers are safe */
    asmspy_df_defuse_counts(NULL, 0, &in, &out);
    check_sz("du null in", in, 0);
    check_sz("du null out", out, 0);
    asmspy_df_defuse_counts(g, 3, NULL, NULL); /* must not crash */

    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

static void test_slices_and_rowstyle(void) {
    asmtest_valtrace_t *v = build_trace();
    asmtest_defuse_t *g = asmtest_defuse_build(v);

    /* no slice active: every row is NORMAL */
    for (uint32_t s = 0; s < 5; s++)
        check_int("rowstyle none", asmspy_df_rowstyle(NULL, s),
                  ASMSPY_DF_ROW_NORMAL);

    /* backward slice from the sink (#4): {0,3,4} are producers of it */
    at_val_rec_t seed = {0};
    seed.step = 4;
    asmtest_slice_t *bs = asmtest_slice_backward(g, seed);
    check_sz("bwd slice size", bs->n, 3);
    check_int("bwd contains 0", asmtest_slice_contains(bs, 0), 1);
    check_int("bwd contains 1", asmtest_slice_contains(bs, 1), 0);
    check_int("bwd contains 3", asmtest_slice_contains(bs, 3), 1);
    check_int("rowstyle bwd #0", asmspy_df_rowstyle(bs, 0),
              ASMSPY_DF_ROW_INSLICE);
    check_int("rowstyle bwd #1", asmspy_df_rowstyle(bs, 1),
              ASMSPY_DF_ROW_DIMMED);
    check_int("rowstyle bwd #2", asmspy_df_rowstyle(bs, 2),
              ASMSPY_DF_ROW_DIMMED);
    check_int("rowstyle bwd #4", asmspy_df_rowstyle(bs, 4),
              ASMSPY_DF_ROW_INSLICE);
    asmtest_slice_free(bs);

    /* forward slice from the independent def (#1): {1,2} only */
    seed.step = 1;
    asmtest_slice_t *fs = asmtest_slice_forward(g, seed);
    check_sz("fwd slice size", fs->n, 2);
    check_int("fwd contains 1", asmtest_slice_contains(fs, 1), 1);
    check_int("fwd contains 2", asmtest_slice_contains(fs, 2), 1);
    check_int("fwd contains 0", asmtest_slice_contains(fs, 0), 0);
    check_int("rowstyle fwd #1", asmspy_df_rowstyle(fs, 1),
              ASMSPY_DF_ROW_INSLICE);
    check_int("rowstyle fwd #0", asmspy_df_rowstyle(fs, 0),
              ASMSPY_DF_ROW_DIMMED);
    check_int("rowstyle fwd #4", asmspy_df_rowstyle(fs, 4),
              ASMSPY_DF_ROW_DIMMED);
    asmtest_slice_free(fs);

    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

static void test_loc_str(void) {
    at_val_rec_t r = reg(7, 0, false, true);
    at_val_rec_t m = mem(0x601000, 0, false, true);
    char b[48];
    asmspy_df_loc_str(&r, b, sizeof b);
    check_str("loc reg", b, "reg#7");
    asmspy_df_loc_str(&m, b, sizeof b);
    check_str("loc mem", b, "[0x601000]");
    asmspy_df_loc_str(NULL, b, sizeof b);
    check_str("loc null", b, "");
}

int main(void) {
    test_annotate();
    test_annotate_edges();
    test_defuse_counts();
    test_slices_and_rowstyle();
    test_loc_str();

    if (failures) {
        fprintf(stderr, "test_view: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_view: PASS\n");
    return 0;
}
