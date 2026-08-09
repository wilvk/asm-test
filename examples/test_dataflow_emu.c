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

/* The per-guest entry (R5, docs/internal/gui/32): the same producer selecting a
 * guest by arch. asmtest_dataflow_emu_run is exactly this pinned to x86-64. */
int asmtest_dataflow_emu_run_arch(asmtest_arch_t arch, const uint8_t *code,
                                  size_t code_len, const long *args, int nargs,
                                  uint64_t max_insns, asmtest_valtrace_t *vt);

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

/* Find a register WRITE record at `step` and report its captured value. */
static int reg_write_value(const asmtest_valtrace_t *v, uint32_t step,
                           uint64_t *out) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == step && r->kind == AT_LOC_REG && r->is_write &&
            r->value_valid) {
            *out = r->value;
            return 1;
        }
    }
    return 0;
}

/* AArch64 leaf routine df_add(a, b)  [x0=a, x1=b] — the R5 arm64 guest end to end:
 *   0x00  add x2, x0, x1   ; step0: X2 <- X0 + X1   (reads x0,x1; writes x2)
 *   0x04  mov x0, x2       ; step1: X0 <- X2        (the return value; edge x2)
 *   0x08  ret              ; step2: return via x30
 * Hand-derived: a def-use edge step0 -> step1 (x2), and x0 = a + b at step1. This
 * is the SAME L0->L1 machinery the x86-64 chain exercises, under the arm64 guest —
 * a different `df_guest`, not a second code path. */
static const uint8_t arm64_df_add[] = {
    0x02, 0x00, 0x01, 0x8b, /* 0x00 add x2, x0, x1 */
    0xe0, 0x03, 0x02, 0xaa, /* 0x04 mov x0, x2     */
    0xc0, 0x03, 0x5f, 0xd6, /* 0x08 ret            */
};

/* ARM32 (A32) leaf routine df_add(a, b)  [r0=a, r1=b] — the doc-60 T1 arm32
 * guest end to end, the SAME shape as arm64_df_add above:
 *   0x00  add r2, r0, r1   ; step0: R2 <- R0 + R1   (reads r0,r1; writes r2)
 *   0x04  mov r0, r2       ; step1: R0 <- R2        (the return value; edge r2)
 *   0x08  bx lr            ; step2: return via lr
 * Hand-derived: a def-use edge step0 -> step1 (r2), and r0 = a + b at step1 —
 * a different `df_guest`, not a second code path. */
static const uint8_t arm32_df_add[] = {
    0x01, 0x20, 0x80, 0xe0, /* 0x00 add r2, r0, r1 */
    0x02, 0x00, 0xa0, 0xe1, /* 0x04 mov r0, r2     */
    0x1e, 0xff, 0x2f, 0xe1, /* 0x08 bx lr          */
};

/* RISC-V (RV64) leaf routine df_add(a, b)  [a0=a, a1=b] — the doc-60 T3
 * riscv64 guest end to end, the SAME shape as arm64_df_add/arm32_df_add
 * above (assembled with `.option norvc`, so every instruction is the base
 * 4-byte RV64I encoding — no compressed forms, mirroring this file's
 * arm64/arm32 blocks' own fixed 4-byte instruction stride):
 *   0x00  add a2, a0, a1  ; step0: A2 <- A0 + A1   (reads a0,a1; writes a2)
 *   0x04  mv a0, a2       ; step1: A0 <- A2        (the return value; edge a2)
 *   0x08  ret             ; step2: return via ra (jalr x0, 0(ra))
 * Hand-derived: a def-use edge step0 -> step1 (a2), and a0 = a + b at step1 —
 * a different `df_guest`, not a second code path. */
static const uint8_t riscv_df_add[] = {
    0x33, 0x06, 0xb5, 0x00, /* 0x00 add a2, a0, a1 */
    0x13, 0x05, 0x06, 0x00, /* 0x04 mv a0, a2      */
    0x67, 0x80, 0x00, 0x00, /* 0x08 ret            */
};

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

    /* --- R5: the AArch64 guest, same L0->L1 machinery, a different df_guest --- */
    asmtest_valtrace_t *va = asmtest_valtrace_new(64, 512, 256);
    if (va != NULL) {
        long aargs[2] = {7, 5}; /* x0=7, x1=5 -> x0 = 12 */
        int arc =
            asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM64, arm64_df_add,
                                          sizeof arm64_df_add, aargs, 2, 0, va);
        CHECK(arc == 0,
              "arm64: routine ran to the sentinel return (via x30/LR)");
        CHECK(va->steps_len == 3,
              "arm64: three steps captured (add, mov, ret)");
        if (va->steps_len == 3) {
            static const uint64_t want[3] = {0x00, 0x04, 0x08};
            int ok = 1;
            for (int i = 0; i < 3; i++)
                if (va->insn_off[i] != want[i])
                    ok = 0;
            CHECK(ok,
                  "arm64: per-step offsets are the 4-byte instruction stride");
        }
        uint64_t x2 = 0;
        CHECK(reg_write_value(va, 0, &x2) && x2 == 12,
              "arm64: add x2, x0, x1 captured x2 = 7 + 5 = 12");
        asmtest_defuse_t *ga = asmtest_defuse_build(va);
        CHECK(ga != NULL, "arm64: def-use graph built");
        CHECK(has_edge(ga, 0, 1),
              "arm64: def-use edge step0 -> step1 (x2 add -> mov)");
        asmtest_defuse_free(ga);
        asmtest_valtrace_free(va);
    } else {
        CHECK(0, "arm64: valtrace_new failed");
    }

    /* --- doc 60 T1: the ARM32 guest, same L0->L1 machinery, a different df_guest --- */
    asmtest_valtrace_t *v32 = asmtest_valtrace_new(64, 512, 256);
    if (v32 != NULL) {
        long a32[2] = {7, 5}; /* r0=7, r1=5 -> r0 = 12 */
        int rc32 =
            asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_ARM32, arm32_df_add,
                                          sizeof arm32_df_add, a32, 2, 0, v32);
        CHECK(rc32 == 0, "arm32: routine ran to the sentinel return (via lr)");
        CHECK(v32->steps_len == 3,
              "arm32: three steps captured (add, mov, bx)");
        if (v32->steps_len == 3) {
            static const uint64_t want[3] = {0x00, 0x04, 0x08};
            int ok = 1;
            for (int i = 0; i < 3; i++)
                if (v32->insn_off[i] != want[i])
                    ok = 0;
            CHECK(ok,
                  "arm32: per-step offsets are the 4-byte instruction stride");
        }
        uint64_t r2 = 0;
        CHECK(reg_write_value(v32, 0, &r2) && r2 == 12,
              "arm32: add r2, r0, r1 captured r2 = 7 + 5 = 12");
        asmtest_defuse_t *g32 = asmtest_defuse_build(v32);
        CHECK(g32 != NULL, "arm32: def-use graph built");
        CHECK(has_edge(g32, 0, 1),
              "arm32: def-use edge step0 -> step1 (r2 add -> mov)");
        asmtest_defuse_free(g32);
        asmtest_valtrace_free(v32);
    } else {
        CHECK(0, "arm32: valtrace_new failed");
    }

    /* --- doc 60 T3: the RISC-V (RV64) guest, same L0->L1 machinery, a
     * different df_guest --- */
    asmtest_valtrace_t *vr = asmtest_valtrace_new(64, 512, 256);
    if (vr != NULL) {
        long ar[2] = {7, 5}; /* a0=7, a1=5 -> a0 = 12 */
        int rcr =
            asmtest_dataflow_emu_run_arch(ASMTEST_ARCH_RISCV64, riscv_df_add,
                                          sizeof riscv_df_add, ar, 2, 0, vr);
        CHECK(rcr == 0, "riscv64: routine ran to the sentinel return (via ra)");
        CHECK(vr->steps_len == 3,
              "riscv64: three steps captured (add, mv, ret)");
        if (vr->steps_len == 3) {
            static const uint64_t want[3] = {0x00, 0x04, 0x08};
            int ok = 1;
            for (int i = 0; i < 3; i++)
                if (vr->insn_off[i] != want[i])
                    ok = 0;
            CHECK(
                ok,
                "riscv64: per-step offsets are the 4-byte instruction stride");
        }
        uint64_t a2 = 0;
        CHECK(reg_write_value(vr, 0, &a2) && a2 == 12,
              "riscv64: add a2, a0, a1 captured a2 = 7 + 5 = 12");
        asmtest_defuse_t *gr = asmtest_defuse_build(vr);
        CHECK(gr != NULL, "riscv64: def-use graph built");
        CHECK(has_edge(gr, 0, 1),
              "riscv64: def-use edge step0 -> step1 (a2 add -> mv)");
        asmtest_defuse_free(gr);
        asmtest_valtrace_free(vr);
    } else {
        CHECK(0, "riscv64: valtrace_new failed");
    }

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
