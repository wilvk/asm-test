/*
 * test_dataflow_ptrace.c — Phase 3: the SCOPED PTRACE L0 producer end-to-end. Forks a
 * live tracee, single-steps a self-contained x86-64 routine (src/dataflow_ptrace.c),
 * and reads REAL per-step register/memory values out of band, then builds the shared
 * L1 def-use + L2 slices over the captured value trace and asserts them against a
 * HAND-DERIVED expectation — the same ground-truth style test_dataflow_emu uses, plus
 * the plan's ORACLE cross-check: where Unicorn is present, the emulator L0 runs the same
 * fixture and the live ptrace slices must equal the emulator slices.
 *
 * Coverage (data-flow-tracing-plan.md Phase 3 exit criteria): a deterministic region's
 * value trace whose slices match the oracle; a gs:-relative access (segment-base
 * resolution); a RIP-relative load (next-instruction EA fixup); XMM and YMM vector
 * operands (GETFPREGS / NT_X86_XSTATE); and the goal-(a) ATTACH path — SEIZE a live
 * victim, capture the scoped region, DETACH, and assert the target survives. Self-skips
 * cleanly where ptrace is unavailable (seccomp) or the build lacks Capstone.
 *
 * Fixtures (all leaf, x86-64 SysV — rdi=arg0, rsi=arg1):
 *   df_chain(a,b): mov rax,rdi / mov [rsp-8],rax / mov rcx,[rsp-8] /
 *                  lea rdx,[rcx+rsi] / mov rax,rdx / ret     — a load-after-store +
 *                  register move chain; forward(0)=backward(4)={0,1,2,3,4}.
 */
#include "asmtest_valtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The scoped ptrace L0 producers ship NO header — a value-trace PRODUCER is a tier, not
 * part of the shared asmtest_valtrace.h sink API — so this suite re-declares their entry
 * points and return codes here, exactly as it re-declares the emulator producer below. */
#define DF_PTRACE_OK     0
#define DF_PTRACE_FAULT  1
#define DF_PTRACE_EINVAL (-1)
#define DF_PTRACE_ENOSYS (-3)
#define DF_PTRACE_ETRACE (-4)

int asmtest_dataflow_ptrace_run(const uint8_t *code, size_t code_len,
                                const long *args, int nargs, uint64_t max_insns,
                                uint64_t gs_base, long *result,
                                asmtest_valtrace_t *vt);

int asmtest_dataflow_ptrace_attach(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   uint64_t max_insns, uint64_t gs_base,
                                   long *result, int *survived,
                                   asmtest_valtrace_t *vt);

#ifdef DF_HAVE_EMU
/* The emulator L0 producer (oracle); linked + declared only where Unicorn is present. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
#endif

/* Capstone register ids the assertions name. Duplicated (not #include'd) so this suite
 * has no direct Capstone dependency of its own; they are ABI-stable enum values. */
#define REG_XMM0 122
#define REG_YMM0 154

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

/* mov rax, gs:[0x10] / ret — a gs:-segmented load. */
static const uint8_t gs_load[] = {
    0x65, 0x48, 0x8b, 0x04, 0x25,
    0x10, 0x00, 0x00, 0x00, /* mov rax,gs:[0x10] */
    0xc3,                   /* ret               */
};

/* movq xmm0,rdi / movq xmm1,rsi / paddq xmm0,xmm1 / movq rax,xmm0 / ret. */
static const uint8_t xmm_chain[] = {
    0x66, 0x48, 0x0f, 0x6e, 0xc7, /* 0x00 movq xmm0, rdi   */
    0x66, 0x48, 0x0f, 0x6e, 0xce, /* 0x05 movq xmm1, rsi   */
    0x66, 0x0f, 0xd4, 0xc1,       /* 0x0a paddq xmm0, xmm1 */
    0x66, 0x48, 0x0f, 0x7e, 0xc0, /* 0x0e movq rax, xmm0   */
    0xc3,                         /* 0x13 ret              */
};

/* vmovdqu ymm0,[rdi] / vmovdqa ymm1,ymm0 / ret — a 256-bit load + move. */
static const uint8_t ymm_chain[] = {
    0xc5, 0xfe, 0x6f, 0x07, /* 0x00 vmovdqu ymm0, [rdi] */
    0xc5, 0xfd, 0x6f, 0xc8, /* 0x04 vmovdqa ymm1, ymm0  */
    0xc3,                   /* 0x08 ret                 */
};

/* mov rax,[rip+1] / ret / .quad 0xdeadbeefcafebabe — a RIP-relative load. Its EA is
 * relative to the NEXT instruction, so it reads base+7+1 = base+8 (the quad), NOT
 * base+1 (into this instruction's own opcode bytes). Guards the next-insn EA fixup. */
static const uint8_t rip_load[] = {
    0x48, 0x8b, 0x05, 0x01, 0x00, 0x00, 0x00,       /* 0x00 mov rax,[rip+1] */
    0xc3,                                           /* 0x07 ret            */
    0xbe, 0xba, 0xfe, 0xca, 0xef, 0xbe, 0xad, 0xde, /* 0x08 .quad 0xdead... */
};

static int has_edge(const asmtest_defuse_t *g, uint32_t from, uint32_t to) {
    for (size_t i = 0; i < g->n; i++)
        if (g->edges[i].from_step == from && g->edges[i].to_step == to)
            return 1;
    return 0;
}

/* A memory WRITE record's captured value at `step`. */
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

/* Low 8 bytes of a register READ record's value at `step` (inline or from wide[]). */
static int reg_read_low8(const asmtest_valtrace_t *v, uint32_t step,
                         uint32_t reg, uint64_t *out) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step != step || r->kind != AT_LOC_REG || r->is_write ||
            r->reg != reg || !r->value_valid)
            continue;
        if (r->wide) {
            if ((size_t)r->wide_off + 8 > v->wide_len)
                return 0;
            memcpy(out, v->wide + r->wide_off, 8);
        } else {
            *out = r->value;
        }
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

/* ------------------------------------------------------------------ */

static void test_chain(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    if (v == NULL) {
        CHECK(0, "chain: valtrace_new");
        return;
    }
    long args[2] = {7, 5};
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(df_chain, sizeof df_chain, args, 2, 0,
                                         0, &result, v);
    CHECK(rc == DF_PTRACE_OK,
          "chain: live tracee single-stepped to the region return");
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

    uint64_t sv = 0, lv = 0;
    CHECK(mem_write_value(v, 1, &sv) && sv == 7,
          "chain: store at step1 captured live value 7 (rax<-rdi)");
    CHECK(mem_read_value(v, 2, &lv) && lv == 7,
          "chain: load at step2 read back 7 from the tracee's stack");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL, "chain: def-use graph built over the live trace");
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
          "chain: live forward slice == emulator oracle forward slice");
    CHECK(ebwd && slices_equal(bwd, ebwd),
          "chain: live backward slice == emulator oracle backward slice");
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

static void test_gs(void) {
    /* A caller-owned TLS block; gs:[0x10] must resolve to tls+0x10. Allocated before the
     * producer forks, so the tracee inherits it and process_vm_readv sees the same bytes. */
    unsigned char *tls = (unsigned char *)calloc(64, 1);
    if (tls == NULL) {
        CHECK(0, "gs: tls alloc");
        return;
    }
    uint64_t magic = 0xCAFEF00DD15EA5EDULL;
    memcpy(tls + 0x10, &magic, 8);

    asmtest_valtrace_t *v = asmtest_valtrace_new(16, 64, 64);
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(gs_load, sizeof gs_load, NULL, 0, 0,
                                         (uint64_t)(uintptr_t)tls, &result, v);
    CHECK(rc == DF_PTRACE_OK, "gs: tracee stepped with GS base set");
    CHECK(result == (long)magic,
          "gs: routine returned the gs:[0x10] value (rax <- gs:0x10)");
    uint64_t lv = 0;
    CHECK(mem_read_value(v, 0, &lv) && lv == magic,
          "gs: producer resolved gs_base+0x10 and read the live value");
    asmtest_valtrace_free(v);
    free(tls);
}

static void test_xmm(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(16, 128, 256);
    long args[2] = {7, 5};
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(xmm_chain, sizeof xmm_chain, args, 2,
                                         0, 0, &result, v);
    CHECK(rc == DF_PTRACE_OK, "xmm: tracee stepped through the SSE chain");
    CHECK(result == 12, "xmm: routine returned 12 (paddq of 7 and 5)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(has_edge(g, 0, 2) && has_edge(g, 1, 2),
          "xmm: paddq reads xmm0(step0) and xmm1(step1) -> edges 0->2, 1->2");
    CHECK(has_edge(g, 2, 3),
          "xmm: movq rax,xmm0 reads the paddq result -> edge 2->3");
    at_val_rec_t sink = {0};
    sink.step = 3;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(bwd && bwd->n == 4, "xmm: backward slice(step3) = {0,1,2,3}");

    /* The captured 128-bit xmm0 value read by movq rax,xmm0 (step3) is the paddq sum. */
    uint64_t xv = 0;
    CHECK(reg_read_low8(v, 3, REG_XMM0, &xv) && xv == 12,
          "xmm: live xmm0 value at step3 read is 12 (GETFPREGS)");

    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

static void test_ymm(void) {
    if (!__builtin_cpu_supports("avx")) {
        printf("# SKIP ymm: host has no AVX\n");
        return;
    }
    unsigned char *buf = (unsigned char *)calloc(32, 1);
    if (buf == NULL) {
        CHECK(0, "ymm: buf alloc");
        return;
    }
    uint64_t lo = 0x1122334455667788ULL;
    memcpy(buf, &lo, 8);

    asmtest_valtrace_t *v = asmtest_valtrace_new(16, 64, 256);
    long args[1] = {(long)(uintptr_t)buf};
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(ymm_chain, sizeof ymm_chain, args, 1,
                                         0, 0, &result, v);
    CHECK(rc == DF_PTRACE_OK, "ymm: tracee stepped through the 256-bit chain");

    /* step0 vmovdqu ymm0,[rdi]: a 32-byte memory read whose low 8 bytes are the buffer. */
    uint64_t mv = 0;
    int mok = 0;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == 0 && r->kind != AT_LOC_REG && !r->is_write &&
            r->size == 32 && r->wide && r->value_valid &&
            (size_t)r->wide_off + 8 <= v->wide_len) {
            memcpy(&mv, v->wide + r->wide_off, 8);
            mok = 1;
        }
    }
    CHECK(mok && mv == lo,
          "ymm: 256-bit load captured 32 wide bytes from [rdi]");

    /* step1 vmovdqa ymm1,ymm0: reads ymm0 (256-bit) — a live YMM register value. */
    uint64_t yv = 0;
    CHECK(reg_read_low8(v, 1, REG_YMM0, &yv) && yv == lo,
          "ymm: live ymm0 value at step1 read (NT_X86_XSTATE)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(has_edge(g, 0, 1),
          "ymm: def-use edge step0 -> step1 over the 256-bit ymm0");
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
    free(buf);
}

static void test_rip(void) {
    /* RIP-relative EAs are relative to the NEXT instruction. Without the fixup the
     * producer resolves base+disp against THIS instruction's rip and reads the opcode
     * bytes; with it, it reads the intended datum one instruction ahead. */
    asmtest_valtrace_t *v = asmtest_valtrace_new(16, 64, 64);
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(rip_load, sizeof rip_load, NULL, 0, 0,
                                         0, &result, v);
    CHECK(rc == DF_PTRACE_OK, "rip: tracee stepped a RIP-relative load");
    CHECK(result == (long)0xdeadbeefcafebabeULL,
          "rip: routine returned the RIP-relative quad (rax <- [rip+1])");
    uint64_t lv = 0;
    CHECK(mem_read_value(v, 0, &lv) && lv == 0xdeadbeefcafebabeULL,
          "rip: load EA uses the next-insn base (reads the quad, not opcode "
          "bytes)");
    asmtest_valtrace_free(v);
}

static void test_attach(void) {
    /* The plan's goal (a): attach to a LIVE, independently-running victim, capture a
     * scoped region's real value trace, then detach so the target SURVIVES. */
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    long args[2] = {7, 5};
    long result = 0;
    int survived = 0;
    int rc = asmtest_dataflow_ptrace_attach(df_chain, sizeof df_chain, args, 2,
                                            0, 0, &result, &survived, v);
    if (rc == DF_PTRACE_ETRACE) {
        printf("# SKIP attach: PTRACE_SEIZE unavailable here (yama/seccomp)\n");
        asmtest_valtrace_free(v);
        return;
    }
    CHECK(rc == DF_PTRACE_OK,
          "attach: SEIZEd a live victim and single-stepped the scoped region");
    CHECK(result == 12,
          "attach: attached region returned 12 (rax = rdi + rsi)");
    CHECK(survived == 1,
          "attach: victim SURVIVED the detach and ran its post-region code");
    CHECK(v->steps_len == 6,
          "attach: six in-region steps captured over the live victim");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL && has_edge(g, 1, 2),
          "attach: load-after-store edge step1 -> step2 (attached capture)");
    CHECK(has_edge(g, 0, 1) && has_edge(g, 2, 3) && has_edge(g, 3, 4),
          "attach: register move chain edges 0->1, 2->3, 3->4 (attached)");
    at_val_rec_t sink = {0};
    sink.step = 4;
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
    CHECK(
        bwd && bwd->n == 5 && asmtest_slice_contains(bwd, 0) &&
            asmtest_slice_contains(bwd, 4),
        "attach: backward slice(step4) = {0,1,2,3,4} on the attached capture");
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
}

int main(void) {
    /* Probe once; a negative code means the environment cannot ptrace (seccomp) or the
     * build lacks Capstone / is off-platform — self-skip cleanly like the other tiers. */
    asmtest_valtrace_t *probe = asmtest_valtrace_new(8, 16, 16);
    long args[2] = {7, 5};
    int prc = asmtest_dataflow_ptrace_run(df_chain, sizeof df_chain, args, 2, 0,
                                          0, NULL, probe);
    asmtest_valtrace_free(probe);
    if (prc == DF_PTRACE_ENOSYS) {
        printf("# SKIP dataflow-ptrace: built off Linux x86-64 / without "
               "Capstone\n"
               "1..0\n");
        return 0;
    }
    if (prc == DF_PTRACE_ETRACE) {
        printf(
            "# SKIP dataflow-ptrace: ptrace unavailable here (seccomp/yama)\n"
            "1..0\n");
        return 0;
    }

    test_chain();
    test_rip();
    test_gs();
    test_xmm();
    test_ymm();
    test_attach();

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
