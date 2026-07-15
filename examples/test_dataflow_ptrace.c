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
#include "asmtest_codeimage.h" /* Increment 3: the versioned code-image the decode reads from */
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

/* Increment 3 — df_chain with ONE byte patched (offset 2: 0xf8 -> 0xf0), so step 0 decodes
 * `mov rax, rsi` (reads RSI) instead of `mov rax, rdi` (reads RDI). SAME instruction length,
 * so single-stepping stays offset-synchronized, but a DIFFERENT read-set. The code-image
 * records df_chain as version 0 (the bytes live when the trace runs) and this as version 1;
 * decoding at v0 must still enumerate the RDI read even though live memory holds the RSI
 * read — the "time-correct bytes survive a mid-capture patch/relocate" contract. */
static const uint8_t df_chain_v2[] = {
    0x48, 0x89, 0xf0, /* 0x00 mov rax, rsi   (patched; was mov rax,rdi) */
    0x48, 0x89, 0x44, 0x24,
    0xf8, /* 0x03 mov [rsp-8], rax                          */
    0x48, 0x8b, 0x4c, 0x24,
    0xf8,                   /* 0x08 mov rcx, [rsp-8]                          */
    0x48, 0x8d, 0x14, 0x31, /* 0x0d lea rdx, [rcx+rsi]                        */
    0x48, 0x89, 0xd0,       /* 0x11 mov rax, rdx                              */
    0xc3,                   /* 0x14 ret                                       */
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

/* Increment 2 — a NON-leaf region that calls a helper MID-region. rdi (arg0) is the
 * helper's address, rsi/rdx (arg1/arg2) are the data. The helper lives in a SEPARATE
 * inherited mapping, so `call rdi` leaves the recorded region [base,base+9): the producer
 * must STEP OVER it and resume, yielding a value trace whose rax chain (step0 write ->
 * step2 read) threads ACROSS the call. Returns rsi + rdx = 12. */
static const uint8_t call_region[] = {
    0x48, 0x89, 0xf0, /* 0x00 mov rax, rsi   (rax = arg1)      */
    0xff, 0xd7,       /* 0x03 call rdi       (call the helper) */
    0x48, 0x01, 0xd0, /* 0x05 add rax, rdx   (rax += arg2)     */
    0xc3,             /* 0x08 ret                              */
};

/* The stepped-over helper: sets a caller-saved scratch reg and returns, preserving the
 * region's rax/rsi/rdx. Its two instructions must NOT appear in the value trace. */
static const uint8_t callout_helper[] = {
    0x48, 0xc7, 0xc1, 0x64, 0x00, 0x00, 0x00, /* mov rcx, 100 */
    0xc3,                                     /* ret          */
};

/* A helper that NEVER returns to its caller — a direct _exit(0) syscall. The step-over's
 * run-to-return breakpoint is thus never hit, so the producer must catch the target's
 * exit and truncate HONESTLY (vt->truncated) rather than hang waiting for a return. */
static const uint8_t callout_helper_noreturn[] = {
    0xb8, 0x3c, 0x00, 0x00, 0x00, /* mov eax, 60  (__NR_exit) */
    0x31, 0xff,                   /* xor edi, edi (status 0)  */
    0x0f, 0x05,                   /* syscall                  */
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

/* ---- Increment 1: attach to a FOREIGN, already-running process by pid ---- */
#if defined(__linux__) && defined(__x86_64__)
#include <pthread.h> /* Increment 4: the multi-thread worker fixture */
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h> /* Increment 4: SYS_gettid (the worker publishes its own tid) */
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int asmtest_dataflow_ptrace_attach_pid(pid_t pid, uint64_t base,
                                       size_t code_len, uint64_t max_insns,
                                       long *result, asmtest_valtrace_t *vt);

/* Increment 4: worker-thread targeting. SEIZE all threads, single-step whichever enters
 * the region (by its own tid, never the leader); only_tid (0 = any) pins one thread. */
int asmtest_dataflow_ptrace_attach_pid_tid(pid_t pid, pid_t only_tid,
                                           uint64_t base, size_t code_len,
                                           uint64_t max_insns, long *result,
                                           asmtest_valtrace_t *vt);

/* Increment 3: attach_pid with an optional versioned code-image (+ `when` sequence) as the
 * operand-decode byte source. img == NULL is the native attach_pid behaviour. Re-declared
 * here like the other producer entries (the producer ships no public header). */
int asmtest_dataflow_ptrace_attach_pid_versioned(pid_t pid, uint64_t base,
                                                 size_t code_len,
                                                 uint64_t max_insns,
                                                 asmtest_codeimage_t *img,
                                                 uint64_t when, long *result,
                                                 asmtest_valtrace_t *vt);

typedef long (*fn2_t)(long, long);
typedef struct {
    volatile long counter;
} sp_ctl;

static void test_attach_pid(void) {
    /* Increment 1: attach to a process we did NOT create. The victim runs INDEPENDENTLY (no
     * PTRACE_TRACEME) — it loops calling df_chain at a known mmap'd address, bumping a shared
     * counter each iteration. We attach BY PID: SEIZE, run_to the region entry, read the
     * region bytes FROM the target (process_vm_readv), single-step the region, then DETACH so
     * the victim SURVIVES — proven by the counter still advancing after the detach. */
    size_t len = sizeof df_chain;
    void *ex = mmap(NULL, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ex == MAP_FAILED) {
        printf("# SKIP attach_pid: mmap failed\n");
        return;
    }
    memcpy(ex, df_chain, len);
    if (mprotect(ex, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(ex, len);
        printf("# SKIP attach_pid: mprotect failed\n");
        return;
    }
    uint64_t base = (uint64_t)(uintptr_t)ex;

    sp_ctl *ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) {
        munmap(ex, len);
        printf("# SKIP attach_pid: shm mmap failed\n");
        return;
    }
    ctl->counter = 0;

    pid_t pid = fork();
    if (pid < 0) {
        munmap(ex, len);
        munmap(ctl, sizeof *ctl);
        printf("# SKIP attach_pid: fork failed\n");
        return;
    }
    if (pid == 0) {
        /* Independent victim: loop the fixture at `base`, bump the counter, forever. */
        struct timespec ts = {0, 2 * 1000 * 1000};
        for (;;) {
            volatile long r = ((fn2_t)base)(7, 5);
            (void)r;
            ctl->counter++;
            nanosleep(&ts, NULL);
        }
        _exit(0);
    }

    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    long result = 0;
    int rc = asmtest_dataflow_ptrace_attach_pid(pid, base, len, 0, &result, v);
    if (rc == DF_PTRACE_ETRACE) {
        printf("# SKIP attach_pid: PTRACE_SEIZE unavailable here "
               "(yama/seccomp)\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        asmtest_valtrace_free(v);
        munmap(ex, len);
        munmap(ctl, sizeof *ctl);
        return;
    }
    CHECK(rc == DF_PTRACE_OK,
          "attach_pid: attached a FOREIGN running pid + stepped the region");
    CHECK(result == 12,
          "attach_pid: attached region returned 12 (rax = rdi + rsi)");
    CHECK(v->steps_len == 6,
          "attach_pid: six in-region steps captured over the foreign victim");

    /* Survival: the detached victim keeps looping, so the shared counter advances. */
    long c0 = ctl->counter;
    struct timespec ts = {0, 40 * 1000 * 1000};
    nanosleep(&ts, NULL);
    CHECK(ctl->counter > c0,
          "attach_pid: victim SURVIVED the detach (counter kept advancing)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL && has_edge(g, 1, 2),
          "attach_pid: load-after-store edge step1 -> step2");
    CHECK(has_edge(g, 0, 1) && has_edge(g, 2, 3) && has_edge(g, 3, 4),
          "attach_pid: register move chain edges 0->1, 2->3, 3->4");
    at_val_rec_t seed = {0}; /* step 0 */
    at_val_rec_t sink = {0};
    sink.step = 4;
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    asmtest_slice_t *bwd = asmtest_slice_backward(g, sink);
#ifdef DF_HAVE_EMU
    asmtest_valtrace_t *ve = asmtest_valtrace_new(64, 512, 512);
    long ea[2] = {7, 5};
    asmtest_dataflow_emu_run(df_chain, sizeof df_chain, ea, 2, 0, ve);
    asmtest_defuse_t *ge = asmtest_defuse_build(ve);
    asmtest_slice_t *efwd = ge ? asmtest_slice_forward(ge, seed) : NULL;
    asmtest_slice_t *ebwd = ge ? asmtest_slice_backward(ge, sink) : NULL;
    CHECK(efwd && slices_equal(fwd, efwd),
          "attach_pid: live forward slice == emulator oracle forward slice");
    CHECK(ebwd && slices_equal(bwd, ebwd),
          "attach_pid: live backward slice == emulator oracle backward slice");
    asmtest_slice_free(efwd);
    asmtest_slice_free(ebwd);
    asmtest_defuse_free(ge);
    asmtest_valtrace_free(ve);
#else
    CHECK(bwd && bwd->n == 5 && asmtest_slice_contains(bwd, 0) &&
              asmtest_slice_contains(bwd, 4),
          "attach_pid: backward slice(step4) = {0,1,2,3,4}");
#endif
    asmtest_slice_free(fwd);
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    munmap(ex, len);
    munmap(ctl, sizeof *ctl);
}

/* ---- Increment 2: call-out step-over (a region that calls a helper mid-region) ---- */

/* mmap `code` into a fresh R+X page (RW then R+X, so it works on a W^X kernel). The
 * fork()ed tracee in asmtest_dataflow_ptrace_run inherits it, so the region can `call`
 * into it as a helper OUTSIDE the recorded region. Returns the mapping or NULL. */
static void *map_rx(const uint8_t *code, size_t len) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    memcpy(p, code, len);
    if (mprotect(p, len, PROT_READ | PROT_EXEC) != 0) {
        munmap(p, len);
        return NULL;
    }
    return p;
}

static void test_callout(void) {
    /* Increment 2 exit criteria (a) + (b): a region that CALLS OUT to a helper
     * mid-region. The helper is a separate inherited mapping, so `call rdi` leaves the
     * recorded region; the producer must run the helper at native speed to its return,
     * record NOTHING over it, and resume — a COMPLETE value trace across the call whose
     * rax def-use threads step0 -> step2 (over the call), with NO helper instruction
     * recorded. */
    void *helper = map_rx(callout_helper, sizeof callout_helper);
    if (helper == NULL) {
        printf("# SKIP callout: helper mmap failed\n");
        return;
    }
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    long args[3] = {(long)(uintptr_t)helper, 5, 7};
    long result = 0;
    int rc = asmtest_dataflow_ptrace_run(call_region, sizeof call_region, args,
                                         3, 0, 0, &result, v);
    CHECK(rc == DF_PTRACE_OK,
          "callout: stepped OVER the helper and completed the region");
    CHECK(result == 12,
          "callout: region returned 12 across the call (rsi + rdx)");
    CHECK(!v->truncated, "callout: value trace is COMPLETE (not truncated)");
    CHECK(v->steps_len == 4,
          "callout: four IN-REGION steps captured (helper not stepped)");
    if (v->steps_len == 4) {
        static const uint64_t want[4] = {0x00, 0x03, 0x05, 0x08};
        int ok = 1;
        for (int i = 0; i < 4; i++)
            if (v->insn_off[i] != want[i])
                ok = 0;
        CHECK(ok,
              "callout: per-step offsets are the region's {0,3,5,8}, not the "
              "helper's");
    }
    /* (b) No recorded step is a helper-internal instruction (every offset is inside
     * the recorded region [0, sizeof call_region)). */
    int any_helper = 0;
    for (size_t i = 0; i < v->steps_len; i++)
        if (v->insn_off[i] >= sizeof call_region)
            any_helper = 1;
    CHECK(!any_helper, "callout: no helper-internal instruction recorded");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL && has_edge(g, 0, 2),
          "callout: rax def-use edge step0 -> step2 threads ACROSS the call");
    at_val_rec_t seed = {0}; /* step 0 */
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 2),
          "callout: forward slice(step0) reaches the post-call add (step2)");
    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);
    munmap(helper, sizeof callout_helper);
}

static void test_callout_noreturn(void) {
    /* Increment 2 exit criterion (c): a callee that NEVER returns must truncate
     * HONESTLY, not hang. The helper is a direct _exit syscall, so the step-over's
     * run-to-return breakpoint is never hit; the producer catches the target's exit and
     * flags truncated, having recorded the region up to (and including) the call. */
    void *helper =
        map_rx(callout_helper_noreturn, sizeof callout_helper_noreturn);
    if (helper == NULL) {
        printf("# SKIP callout-noreturn: helper mmap failed\n");
        return;
    }
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    long args[3] = {(long)(uintptr_t)helper, 5, 7};
    long result = -1;
    int rc = asmtest_dataflow_ptrace_run(call_region, sizeof call_region, args,
                                         3, 0, 0, &result, v);
    CHECK(rc != DF_PTRACE_OK,
          "callout-noreturn: non-returning helper did NOT complete the region");
    CHECK(
        v->truncated,
        "callout-noreturn: truncated HONESTLY (no hang on the missing return)");
    CHECK(v->steps_len >= 2 && v->insn_off[0] == 0x00 && v->insn_off[1] == 0x03,
          "callout-noreturn: captured the region up to the call before "
          "truncating");
    asmtest_valtrace_free(v);
    munmap(helper, sizeof callout_helper_noreturn);
}

/* ---- Increment 3: time-correct bytes + method attribution (JIT patch survival) ---- */

/* The FIRST register READ record captured at `step` — its reg id (into *reg_out) and inline
 * value (into *val_out). df_chain's step 0 has exactly one GP-register read (the mov source),
 * so this pins down WHICH source register the operand enumerator decoded for that step. */
static int reg_read_first(const asmtest_valtrace_t *v, uint32_t step,
                          uint32_t *reg_out, uint64_t *val_out) {
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->step == step && r->kind == AT_LOC_REG && !r->is_write &&
            r->value_valid && !r->wide) {
            if (reg_out != NULL)
                *reg_out = r->reg;
            if (val_out != NULL)
                *val_out = r->value;
            return 1;
        }
    }
    return 0;
}

static void test_versioned(void) {
    /* Increment 3 exit criteria: (a) a region PATCHED mid-capture still decodes the operands
     * of the bytes that were LIVE at each step (the code-image version), not the final bytes;
     * (b) each step attributes to the correct method + version across an induced re-JIT.
     *
     * A code-image records df_chain as version 0 (the bytes live when the trace runs); the
     * region is then patched IN PLACE (a stand-in for an in-place tiered re-JIT / address
     * reuse) and recorded as version 1. The victim EXECUTES the patched code, but decoding at
     * version 0 must still enumerate the ORIGINAL instruction's operands — proving the
     * producer decodes the time-correct bytes, not a stale/late live snapshot. */
    if (!asmtest_codeimage_available()) {
        printf("# SKIP versioned: soft-dirty code-image unavailable here\n");
        return;
    }
    size_t len = sizeof df_chain;

    /* ONE RWX MAP_SHARED region: the victim executes it AND the parent patches it in place,
     * so the patch is visible to the victim and — the code-image tracks THIS process — the
     * parent's own write is what the soft-dirty refresh detects (a foreign process's write
     * would not set this process's PTE soft-dirty bit). */
    void *ex = mmap(NULL, len, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ex == MAP_FAILED) {
        printf("# SKIP versioned: RWX MAP_SHARED mmap failed\n");
        return;
    }
    memcpy(ex, df_chain, len);
    uint64_t base = (uint64_t)(uintptr_t)ex;

    sp_ctl *ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) {
        munmap(ex, len);
        printf("# SKIP versioned: shm mmap failed\n");
        return;
    }
    ctl->counter = 0;

    pid_t pid = fork();
    if (pid < 0) {
        munmap(ex, len);
        munmap(ctl, sizeof *ctl);
        printf("# SKIP versioned: fork failed\n");
        return;
    }
    if (pid == 0) {
        /* Independent victim: loop calling the shared region (which the parent patches). */
        struct timespec ts = {0, 2 * 1000 * 1000};
        for (;;) {
            volatile long r = ((fn2_t)base)(7, 5);
            (void)r;
            ctl->counter++;
            nanosleep(&ts, NULL);
        }
        _exit(0);
    }

    /* Record the code-image on THIS process (base is the same shared page in both). v0 = the
     * original df_chain — the "live-at-that-step" bytes. */
    asmtest_codeimage_t *img = asmtest_codeimage_new(0);
    int trk = img != NULL ? asmtest_codeimage_track(img, ex, len) : -1;
    uint64_t when_v0 = asmtest_codeimage_now(img);

    /* Patch the region in place (mov rax,rdi -> mov rax,rsi); record v1 = the patched bytes. */
    memcpy(ex, df_chain_v2, len);
    int nvz = img != NULL ? asmtest_codeimage_refresh(img) : -1;
    uint64_t when_v1 = asmtest_codeimage_now(img);

    if (img == NULL || trk != ASMTEST_CI_OK || nvz < 1) {
        printf("# SKIP versioned: code-image track/refresh unavailable\n");
        asmtest_codeimage_free(img);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        munmap(ex, len);
        munmap(ctl, sizeof *ctl);
        return;
    }
    CHECK(when_v1 > when_v0,
          "versioned: code-image recorded v0 (original) then v1 (patched)");

    /* --- (a) capture decoding at v0 (original) while live memory holds the patch --- */
    asmtest_valtrace_t *va = asmtest_valtrace_new(64, 512, 512);
    long ra = 0;
    int rca = asmtest_dataflow_ptrace_attach_pid_versioned(
        pid, base, len, 0, img, when_v0, &ra, va);
    if (rca == DF_PTRACE_ETRACE) {
        printf(
            "# SKIP versioned: PTRACE_SEIZE unavailable here (yama/seccomp)\n");
        asmtest_valtrace_free(va);
        asmtest_codeimage_free(img);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        munmap(ex, len);
        munmap(ctl, sizeof *ctl);
        return;
    }
    CHECK(rca == DF_PTRACE_OK,
          "versioned: stepped the region decoding the tracked v0 bytes");
    CHECK(ra == 10, "versioned: the LIVE code that ran was the PATCHED variant "
                    "(returned 10, not 12)");
    int offs_ok = (va->steps_len == 6);
    if (offs_ok) {
        static const uint64_t want[6] = {0x00, 0x03, 0x08, 0x0d, 0x11, 0x14};
        for (int i = 0; i < 6; i++)
            if (va->insn_off[i] != want[i])
                offs_ok = 0;
    }
    CHECK(offs_ok, "versioned: six in-region steps, offsets in sync with the "
                   "tracked bytes");

    uint32_t reg_a = 0;
    uint64_t val_a = 0;
    int got_a = reg_read_first(va, 0, &reg_a, &val_a);
    CHECK(
        got_a && val_a == 7,
        "versioned: step0 decoded v0 `mov rax,rdi` (read rdi=7) — SURVIVED the "
        "patch; a live re-read would read rsi=5");

    asmtest_defuse_t *ga = asmtest_defuse_build(va);
    CHECK(ga != NULL && has_edge(ga, 1, 2) && has_edge(ga, 0, 1) &&
              has_edge(ga, 2, 3) && has_edge(ga, 3, 4),
          "versioned: the v0 decode yields the original routine's coherent "
          "def-use");

    /* --- decode must FOLLOW the version: capture at v1 (patched) reads rsi=5 --- */
    asmtest_valtrace_t *vb = asmtest_valtrace_new(64, 512, 512);
    long rb = 0;
    int rcb = asmtest_dataflow_ptrace_attach_pid_versioned(
        pid, base, len, 0, img, when_v1, &rb, vb);
    uint32_t reg_b = 0;
    uint64_t val_b = 0;
    int got_b = (rcb == DF_PTRACE_OK) && reg_read_first(vb, 0, &reg_b, &val_b);
    CHECK(got_b && val_b == 5,
          "versioned: at v1 step0 decodes `mov rax,rsi` (read rsi=5) — decode "
          "follows the version");
    CHECK(got_a && got_b && reg_a != reg_b,
          "versioned: v0 and v1 decode DIFFERENT source registers for step0");

    /* --- (b) method + version attribution across the induced re-JIT --- */
    /* insn_off[] carries region OFFSETS, so the method-map is offset-keyed: method "M" owns
     * the whole region [0,len). At v0 the JIT map said M was version 0; at v1, version 1. */
    asmtest_method_t map_v0[] = {{0, len, "M", 0}};
    asmtest_method_t map_v1[] = {{0, len, "M", 1}};
    asmtest_method_attr_t attr_a[8], attr_b[8];
    int na = asmtest_method_attribute(map_v0, 1, va, attr_a, 8);
    int nb = asmtest_method_attribute(map_v1, 1, vb, attr_b, 8);
    int a_ok = (na == 6), b_ok = (nb == 6);
    for (int i = 0; i < na; i++)
        if (attr_a[i].record < 0 || attr_a[i].version != 0 ||
            attr_a[i].method != attr_a[0].method)
            a_ok = 0;
    for (int i = 0; i < nb; i++)
        if (attr_b[i].record < 0 || attr_b[i].version != 1)
            b_ok = 0;
    CHECK(a_ok, "versioned: every step attributes to method M version 0 at "
                "code-image v0");
    CHECK(b_ok, "versioned: every step attributes to version 1 across the "
                "induced re-JIT");
    CHECK(
        na == 6 && nb == 6 && attr_a[0].method >= 0 &&
            attr_a[0].method == attr_b[0].method,
        "versioned: method M keeps a STABLE identity across the version bump");

    /* In-place tiered re-JIT at a reused address: both records live, GREATEST version wins. */
    asmtest_method_t map_both[] = {{0, len, "M", 0}, {0, len, "M", 1}};
    CHECK(asmtest_method_resolve_pc(map_both, 2, 0x00) == 1,
          "versioned: in-place re-JIT resolves a reused address to the newest "
          "version");

    asmtest_defuse_free(ga);
    asmtest_valtrace_free(va);
    asmtest_valtrace_free(vb);
    asmtest_codeimage_free(img);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    munmap(ex, len);
    munmap(ctl, sizeof *ctl);
}

/* ---- Increment 4: worker-thread targeting (the target runs OFF the leader) ---- */

/* Shared (MAP_SHARED) control block: the victim's threads publish their kernel tids and
 * bump per-thread counters so the parent (tracer) can (a) target a worker by tid, (b) prove
 * capture came from the intended thread (distinct args -> distinct values), and (c) prove
 * the siblings + captured thread all keep running after the detach. `base` is the region
 * address, the same in the parent and its fork()ed victim (inherited mapping). */
typedef struct {
    volatile long
        leader_counter; /* the leader bumps this; it NEVER runs the region */
    volatile int worker_a_tid;
    volatile long worker_a_counter;
    volatile int worker_b_tid;
    volatile long worker_b_counter;
    volatile uint64_t base;
    volatile long a0_a, a1_a, a0_b, a1_b;
} wk_ctl;

/* worker A / B bodies: publish gettid, then loop calling the region at ctl->base with the
 * worker's OWN args, bumping the worker's counter each iteration (a small sleep keeps the
 * shared-breakpoint race gentle so the pinned thread is caught quickly). Two workers with
 * distinct args let the only_tid test prove WHICH thread was captured. */
static void *wk_worker_a(void *arg) {
    wk_ctl *ctl = (wk_ctl *)arg;
    ctl->worker_a_tid = (int)syscall(SYS_gettid);
    struct timespec ts = {0, 3 * 1000 * 1000};
    for (;;) {
        volatile long r = ((fn2_t)(uintptr_t)ctl->base)(ctl->a0_a, ctl->a1_a);
        (void)r;
        ctl->worker_a_counter++;
        nanosleep(&ts, NULL);
    }
    return NULL;
}
static void *wk_worker_b(void *arg) {
    wk_ctl *ctl = (wk_ctl *)arg;
    ctl->worker_b_tid = (int)syscall(SYS_gettid);
    struct timespec ts = {0, 3 * 1000 * 1000};
    for (;;) {
        volatile long r = ((fn2_t)(uintptr_t)ctl->base)(ctl->a0_b, ctl->a1_b);
        (void)r;
        ctl->worker_b_counter++;
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void test_attach_worker(void) {
    /* Exit criterion (worker capture): a victim whose target routine runs ONLY on a WORKER
     * thread; the leader NEVER calls the region. The leader-only attach_pid would never
     * enter / hang; attach_pid_tid SEIZEs all threads and steps whichever enters — the
     * worker — while the leader keeps running free. */
    void *ex = map_rx(df_chain, sizeof df_chain);
    if (ex == NULL) {
        printf("# SKIP attach_worker: region mmap failed\n");
        return;
    }
    size_t len = sizeof df_chain;
    uint64_t base = (uint64_t)(uintptr_t)ex;

    wk_ctl *ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) {
        munmap(ex, len);
        printf("# SKIP attach_worker: shm mmap failed\n");
        return;
    }
    memset((void *)ctl, 0, sizeof *ctl);
    ctl->base = base;
    ctl->a0_a = 7;
    ctl->a1_a = 5;

    pid_t pid = fork();
    if (pid < 0) {
        munmap(ex, len);
        munmap((void *)ctl, sizeof *ctl);
        printf("# SKIP attach_worker: fork failed\n");
        return;
    }
    if (pid == 0) {
        /* Victim: the leader spawns a worker that loops the region, then the leader
         * busy-loops WITHOUT ever touching the region (the target is off-leader). */
        pthread_t th;
        if (pthread_create(&th, NULL, wk_worker_a, (void *)ctl) != 0)
            _exit(1);
        struct timespec ts = {0, 3 * 1000 * 1000};
        for (;;) {
            ctl->leader_counter++;
            nanosleep(&ts, NULL);
        }
        _exit(0);
    }

    /* Wait until the worker published its tid AND ran the region once (bounded, so a stuck
     * victim self-skips rather than hangs the suite). */
    int ready = 0;
    for (int i = 0; i < 500; i++) {
        if (ctl->worker_a_tid != 0 && ctl->worker_a_counter > 0) {
            ready = 1;
            break;
        }
        struct timespec ts = {0, 3 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (!ready) {
        printf("# SKIP attach_worker: victim worker did not start\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        munmap(ex, len);
        munmap((void *)ctl, sizeof *ctl);
        return;
    }

    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    long result = 0;
    int rc = asmtest_dataflow_ptrace_attach_pid_tid(pid, 0, base, len, 0,
                                                    &result, v);
    if (rc == DF_PTRACE_ETRACE) {
        printf("# SKIP attach_worker: PTRACE_SEIZE unavailable here "
               "(yama/seccomp)\n");
        asmtest_valtrace_free(v);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        munmap(ex, len);
        munmap((void *)ctl, sizeof *ctl);
        return;
    }
    CHECK(
        rc == DF_PTRACE_OK,
        "attach_worker: SEIZE-all + stepped the WORKER's region (off-leader)");
    CHECK(result == 12, "attach_worker: worker region returned 12 (rdi + rsi)");
    CHECK(v->steps_len == 6,
          "attach_worker: six in-region steps captured on the worker thread");

    /* Both the leader (a sibling, never traced-stepped) and the worker (single-stepped then
     * detached) must keep running — proving siblings run free AND the target survived. */
    long lc0 = ctl->leader_counter;
    long wc0 = ctl->worker_a_counter;
    struct timespec ts = {0, 60 * 1000 * 1000};
    nanosleep(&ts, NULL);
    CHECK(
        ctl->leader_counter > lc0,
        "attach_worker: the LEADER kept running at full speed (sibling free)");
    CHECK(ctl->worker_a_counter > wc0,
          "attach_worker: the worker SURVIVED the detach (kept looping)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(
        g != NULL && has_edge(g, 1, 2),
        "attach_worker: load-after-store edge step1 -> step2 (worker capture)");
    CHECK(g != NULL && has_edge(g, 0, 1) && has_edge(g, 2, 3) &&
              has_edge(g, 3, 4),
          "attach_worker: register move chain edges 0->1, 2->3, 3->4 (worker)");
    at_val_rec_t sink = {0};
    sink.step = 4;
    asmtest_slice_t *bwd = g ? asmtest_slice_backward(g, sink) : NULL;
    CHECK(
        bwd && bwd->n == 5 && asmtest_slice_contains(bwd, 0) &&
            asmtest_slice_contains(bwd, 4),
        "attach_worker: backward slice(step4) = {0,1,2,3,4} (worker capture)");
    asmtest_slice_free(bwd);
    asmtest_defuse_free(g);
    asmtest_valtrace_free(v);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    munmap(ex, len);
    munmap((void *)ctl, sizeof *ctl);
}

static void test_attach_worker_tid(void) {
    /* Exit criterion (only_tid restricts to exactly one thread): TWO workers both loop the
     * SAME region with DISTINCT args (A: 7,5 -> 12, stores rdi=7; B: 100,200 -> 300, stores
     * rdi=100). Pinning only_tid = A must capture A's values even though B also races the
     * shared entry breakpoint — a non-target hit is stepped OVER the bp, which stays armed
     * for A. The captured return + step-1 store prove WHICH thread was stepped. */
    void *ex = map_rx(df_chain, sizeof df_chain);
    if (ex == NULL) {
        printf("# SKIP attach_worker_tid: region mmap failed\n");
        return;
    }
    size_t len = sizeof df_chain;
    uint64_t base = (uint64_t)(uintptr_t)ex;

    wk_ctl *ctl = mmap(NULL, sizeof *ctl, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ctl == MAP_FAILED) {
        munmap(ex, len);
        printf("# SKIP attach_worker_tid: shm mmap failed\n");
        return;
    }
    memset((void *)ctl, 0, sizeof *ctl);
    ctl->base = base;
    ctl->a0_a = 7;
    ctl->a1_a = 5; /* A -> 12 */
    ctl->a0_b = 100;
    ctl->a1_b = 200; /* B -> 300 */

    pid_t pid = fork();
    if (pid < 0) {
        munmap(ex, len);
        munmap((void *)ctl, sizeof *ctl);
        printf("# SKIP attach_worker_tid: fork failed\n");
        return;
    }
    if (pid == 0) {
        pthread_t ta, tb;
        if (pthread_create(&ta, NULL, wk_worker_a, (void *)ctl) != 0)
            _exit(1);
        if (pthread_create(&tb, NULL, wk_worker_b, (void *)ctl) != 0)
            _exit(1);
        struct timespec ts = {0, 5 * 1000 * 1000};
        for (;;) {
            ctl->leader_counter++;
            nanosleep(&ts, NULL);
        }
        _exit(0);
    }

    int ready = 0;
    for (int i = 0; i < 500; i++) {
        if (ctl->worker_a_tid != 0 && ctl->worker_b_tid != 0 &&
            ctl->worker_a_counter > 0 && ctl->worker_b_counter > 0) {
            ready = 1;
            break;
        }
        struct timespec ts = {0, 3 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    if (!ready) {
        printf("# SKIP attach_worker_tid: victim workers did not start\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        munmap(ex, len);
        munmap((void *)ctl, sizeof *ctl);
        return;
    }

    pid_t a_tid = (pid_t)ctl->worker_a_tid;
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    long result = 0;
    int rc = asmtest_dataflow_ptrace_attach_pid_tid(pid, a_tid, base, len, 0,
                                                    &result, v);
    if (rc == DF_PTRACE_ETRACE) {
        printf("# SKIP attach_worker_tid: PTRACE_SEIZE unavailable here "
               "(yama/seccomp)\n");
        asmtest_valtrace_free(v);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        munmap(ex, len);
        munmap((void *)ctl, sizeof *ctl);
        return;
    }
    CHECK(rc == DF_PTRACE_OK,
          "attach_worker_tid: only_tid pinned worker A under contention");
    CHECK(
        result == 12,
        "attach_worker_tid: captured A's return 12 (not B's 300) — tid pinned");
    uint64_t sv = 0;
    CHECK(mem_write_value(v, 1, &sv) && sv == 7,
          "attach_worker_tid: step1 store is A's rdi=7 (not B's 100)");
    CHECK(v->steps_len == 6,
          "attach_worker_tid: six in-region steps on the pinned worker");

    long wa0 = ctl->worker_a_counter;
    long wb0 = ctl->worker_b_counter;
    struct timespec ts = {0, 60 * 1000 * 1000};
    nanosleep(&ts, NULL);
    CHECK(ctl->worker_a_counter > wa0 && ctl->worker_b_counter > wb0,
          "attach_worker_tid: BOTH workers kept running after detach (free)");

    asmtest_valtrace_free(v);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    munmap(ex, len);
    munmap((void *)ctl, sizeof *ctl);
}
#else
static void test_attach_pid(void) {}
static void test_callout(void) {}
static void test_callout_noreturn(void) {}
static void test_versioned(void) {}
static void test_attach_worker(void) {}
static void test_attach_worker_tid(void) {}
#endif

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
    test_attach_pid();
    test_callout();
    test_callout_noreturn();
    test_versioned();
    test_attach_worker();
    test_attach_worker_tid();

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
