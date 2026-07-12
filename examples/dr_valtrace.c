/*
 * dr_valtrace.c — Phase 5 (increment 1): the DynamoRIO L0 VALUE producer end-to-end.
 * Runs a self-contained x86-64 routine NATIVELY, in-band, whole-process under
 * DynamoRIO (src/dataflow_dr.c + the value-capture client src/dataflow_dr_client.c),
 * captures a real per-step register/memory value trace, builds the shared L1 def-use +
 * L2 slices over it, and asserts them against a HAND-DERIVED expectation — plus the
 * plan's ORACLE cross-check: where Unicorn is present, the emulator L0 producer runs
 * the SAME fixture and the in-band DR slices must equal the emulator slices.
 *
 * This is the whole-process in-band counterpart of the scoped ptrace value producer's
 * test (test_dataflow_ptrace.c): same fixture, same shared analysis layer, same oracle
 * discipline — the point of the shared asmtest_valtrace_t sink. Named dr_valtrace.c
 * (not test_*.c) so the root Makefile's SUITES wildcard does not sweep this standalone
 * DynamoRIO harness into the forking runner (in-process DR is hostile to per-test
 * fork()); it is wired + run explicitly by mk/native-trace.mk's drtrace-test lane.
 *
 * Self-skips cleanly (exit 0) when DynamoRIO / the value client is unavailable, exactly
 * like the C native-trace smoke test (test_drtrace.c). DynamoRIO permits ONE in-process
 * lifecycle per process, so this drives ONE fixture under DR; the emulator oracle needs
 * no DR and runs freely alongside.
 *
 * Fixture (leaf, x86-64 SysV — rdi=arg0, rsi=arg1), shared with test_dataflow_ptrace:
 *   df_chain(a,b): mov rax,rdi / mov [rsp-8],rax / mov rcx,[rsp-8] /
 *                  lea rdx,[rcx+rsi] / mov rax,rdx / ret — a load-after-store + a
 *                  register move chain; forward(0)=backward(4)={0,1,2,3,4}.
 */
#include "asmtest_valtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The DR value producer ships NO header (a value-trace PRODUCER is a tier, not part of
 * the shared asmtest_valtrace.h sink API), so this suite re-declares its entry points
 * and return codes here, exactly as it re-declares the emulator producer below. */
#define DF_DR_OK     0
#define DF_DR_FAULT  1
#define DF_DR_EINVAL (-1)
#define DF_DR_ENOSYS (-3)
#define DF_DR_ENODR  (-4)

int asmtest_dataflow_dr_available(void);
int asmtest_dataflow_dr_run(const uint8_t *code, size_t code_len,
                            const long *args, int nargs, uint64_t max_insns,
                            long *result, asmtest_valtrace_t *vt);

#ifdef DF_HAVE_EMU
/* The emulator L0 producer (oracle); linked + declared only where Unicorn is present. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
#endif

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

static const uint8_t df_chain[] = {
    0x48, 0x89, 0xf8,             /* 0x00 mov rax, rdi        */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax    */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]    */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi]  */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx        */
    0xc3,                         /* 0x14 ret                 */
};

static int has_edge(const asmtest_defuse_t *g, uint32_t from, uint32_t to) {
    for (size_t i = 0; i < g->n; i++)
        if (g->edges[i].from_step == from && g->edges[i].to_step == to)
            return 1;
    return 0;
}

/* A memory READ record's captured value at `step`. */
static int mem_read_value(const asmtest_valtrace_t *v, uint32_t step,
                          uint64_t *out) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == step && r->kind != AT_LOC_REG && !r->is_write &&
            r->value_valid) {
            *out = r->value;
            return 1;
        }
    }
    return 0;
}

/* True if some register READ record at `step` captured the value `want`. Reg-id-
 * agnostic (the def-use structure is checked separately), so the assertion holds
 * without coupling to Capstone's ABI enum values. */
static int has_reg_read_value(const asmtest_valtrace_t *v, uint32_t step,
                              uint64_t want) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == step && r->kind == AT_LOC_REG && !r->is_write &&
            r->value_valid && r->value == want)
            return 1;
    }
    return 0;
}

/* True if some register WRITE record at `step` captured the value `want` (the
 * one-step-deferred destination-state model). */
static int has_reg_write_value(const asmtest_valtrace_t *v, uint32_t step,
                               uint64_t want) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == step && r->kind == AT_LOC_REG && r->is_write &&
            r->value_valid && r->value == want)
            return 1;
    }
    return 0;
}

static int slices_equal(const asmtest_slice_t *a, const asmtest_slice_t *b) {
    if (a == NULL || b == NULL || a->n != b->n)
        return 0;
    for (size_t i = 0; i < a->n; i++)
        if (a->steps[i] != b->steps[i])
            return 0;
    return 1;
}

static void test_chain(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    if (v == NULL) {
        CHECK(0, "chain: valtrace_new");
        return;
    }
    long args[2] = {7, 5};
    long result = 0;
    int rc = asmtest_dataflow_dr_run(df_chain, sizeof df_chain, args, 2, 0,
                                     &result, v);
    CHECK(rc == DF_DR_OK, "chain: routine captured in-band under DynamoRIO");
    CHECK(result == 12, "chain: routine returned 12 (rax = rdi + rsi)");
    CHECK(v->steps_len == 6,
          "chain: six in-region steps captured (5 body + ret)");
    if (v->steps_len == 6) {
        static const uint64_t want[6] = {0x00, 0x03, 0x08, 0x0d, 0x11, 0x14};
        int ok = 1;
        for (int i = 0; i < 6; i++)
            if (v->insn_off[i] != want[i])
                ok = 0;
        CHECK(ok, "chain: per-step offsets match the disassembly");
    }

    /* Value annotations captured via DR (register file + instrumented memory ref). */
    CHECK(has_reg_read_value(v, 0, 7),
          "chain: step0 register read captured live value 7 (rax<-rdi)");
    CHECK(has_reg_write_value(v, 0, 7),
          "chain: step0 register write captured 7 (deferred post-state rax)");
    uint64_t lv = 0;
    CHECK(mem_read_value(v, 2, &lv) && lv == 7,
          "chain: load at step2 read back 7 from memory (instrumented ref)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL, "chain: def-use graph built over the in-band trace");
    CHECK(has_edge(g, 1, 2),
          "chain: load-after-store edge step1 -> step2 (M[rsp-8])");
    CHECK(has_edge(g, 0, 1) && has_edge(g, 2, 3) && has_edge(g, 3, 4),
          "chain: register move chain edges 0->1, 2->3, 3->4");

    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 4) && !asmtest_slice_contains(fwd, 5),
          "chain: forward slice(step0) = {0,1,2,3,4}, excludes ret");
    at_val_rec_t sink = {0};
    sink.step = 4;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && bwd->n == 5 && asmtest_slice_contains(bwd, 0) &&
              asmtest_slice_contains(bwd, 4) && !asmtest_slice_contains(bwd, 5),
          "chain: backward slice(step4) = {0,1,2,3,4}, excludes ret");

#ifdef DF_HAVE_EMU
    /* Oracle cross-check: the emulator L0 on the SAME fixture must yield the same
     * def-use slices (the captured addresses differ; the STRUCTURE must not). */
    asmtest_valtrace_t *ve = asmtest_valtrace_new(64, 512, 512);
    int erc =
        asmtest_dataflow_emu_run(df_chain, sizeof df_chain, args, 2, 0, ve);
    asmtest_defuse_t *ge = (erc == 0) ? asmtest_defuse_build(ve) : NULL;
    asmtest_slice_t *efwd = ge ? asmtest_slice_forward(ge, seed) : NULL;
    asmtest_slice_t *ebwd = ge ? asmtest_slice_backward(ge, sink) : NULL;
    CHECK(efwd && slices_equal(fwd, efwd),
          "chain: in-band forward slice == emulator oracle forward slice");
    CHECK(ebwd && slices_equal(bwd, ebwd),
          "chain: in-band backward slice == emulator oracle backward slice");
    asmtest_slice_free(efwd);
    asmtest_slice_free(ebwd);
    asmtest_defuse_free(ge);
    asmtest_valtrace_free(ve);
#else
    printf("# SKIP oracle cross-check: built without libunicorn\n");
#endif

    asmtest_slice_free(fwd);
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a hard kill */
    if (!asmtest_dataflow_dr_available()) {
        printf("# SKIP dr-valtrace: DynamoRIO / value client unavailable\n"
               "1..0\n");
        return 0;
    }
    test_chain();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
