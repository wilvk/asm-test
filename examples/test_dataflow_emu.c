/*
 * test_dataflow_emu.c — Phase 2: the EMULATOR L0 producer end-to-end. Runs a
 * self-contained x86-64 routine under Unicorn (src/dataflow_emu.c), builds the
 * shared L1 def-use graph + L2 slices over the captured value trace, and asserts
 * they match a HAND-DERIVED expectation — the CI proving ground for L0->L1->L2 with
 * no hardware. Built only when libunicorn is present (mk/dataflow.mk); the output
 * is REPLAY of the guest bytes, not observation of a live process.
 *
 * Routine df_chain(a, b)  [rdi=a, rsi=b]:
 *   0x00  mov rax, rdi          ; step0: RA <- RDI
 *   0x03  mov [rsp-8], rax      ; step1: M[rsp-8] <- RA
 *   0x08  mov rcx, [rsp-8]      ; step2: RC <- M[rsp-8]   (load-after-store)
 *   0x0d  lea rdx, [rcx+rsi]    ; step3: RD <- RC (+ RSI) (no flags)
 *   0x11  mov rax, rdx          ; step4: RA <- RD
 *   0x14  ret                   ; step5
 * Hand-derived: forward(step0) = backward(step4) = {0,1,2,3,4}; the `ret` (step5)
 * is not in either slice.
 */
#include "asmtest_valtrace.h"

#include <stdio.h>

/* The emulator L0 producer (Unicorn-gated; declared here since it is a tier
 * producer, not part of the pure public sink surface). */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);

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

/* Find a memory WRITE record at `step` and report its captured value. */
static int mem_write_value(const asmtest_valtrace_t *v, uint32_t step,
                           uint64_t *out) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == step && r->kind != AT_LOC_REG && r->is_write &&
            r->value_valid) {
            *out = r->value;
            return 1;
        }
    }
    return 0;
}

int main(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 256);
    if (v == NULL) {
        printf("# SKIP dataflow-emu: valtrace_new failed\n1..0\n");
        return 0;
    }
    long args[2] = {7, 5}; /* rdi=7, rsi=5 */
    int rc = asmtest_dataflow_emu_run(df_chain, sizeof df_chain, args, 2, 0, v);
    CHECK(rc == 0, "emu: routine ran to the sentinel return (replay, not "
                   "observation)");
    CHECK(v->steps_len == 6, "emu: six steps captured (5 body + ret)");
    if (v->steps_len == 6) {
        static const uint64_t want[6] = {0x00, 0x03, 0x08, 0x0d, 0x11, 0x14};
        int ok = 1;
        for (int i = 0; i < 6; i++)
            if (v->insn_off[i] != want[i])
                ok = 0;
        CHECK(ok, "emu: per-step instruction offsets match the disassembly");
    }
    CHECK(v->mem_space == AT_LOC_MEM_ABS, "emu: mem_space is absolute");

    /* captured value: the store at step1 writes rax = rdi = 7 to the stack */
    uint64_t sv = 0;
    CHECK(mem_write_value(v, 1, &sv) && sv == 7,
          "emu: store at step1 captured value 7 (rax<-rdi via mem hook)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL, "emu: def-use graph built");
    CHECK(has_edge(g, 1, 2),
          "emu: load-after-store edge step1 -> step2 (M[rsp-8])");
    CHECK(has_edge(g, 0, 1) && has_edge(g, 2, 3) && has_edge(g, 3, 4),
          "emu: register move chain edges 0->1, 2->3, 3->4");

    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 1) &&
              asmtest_slice_contains(fwd, 2) &&
              asmtest_slice_contains(fwd, 3) &&
              asmtest_slice_contains(fwd, 4) && !asmtest_slice_contains(fwd, 5),
          "emu: forward slice(step0) = {0,1,2,3,4}, excludes ret");

    at_val_rec_t sink = {0};
    sink.step = 4;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && bwd->n == 5 && asmtest_slice_contains(bwd, 0) &&
              asmtest_slice_contains(bwd, 4) && !asmtest_slice_contains(bwd, 5),
          "emu: backward slice(step4) = {0,1,2,3,4}, excludes ret");

    asmtest_slice_free(fwd);
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
