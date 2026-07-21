/*
 * test_dataflow_pt.c — F5: the PT + CODE-IMAGE + UNICORN-REPLAY value tier
 * (src/dataflow_pt.c) end-to-end. F5 is the least-perturbing L0 producer: fully
 * OUT OF BAND (no single-step, no block-step — zero stops of the target), it
 * reconstructs per-instruction values by decoding an Intel PT trace to the
 * executed offset path, materializing the bytes live at trace time from the
 * code-image, and REPLAYING that exact path through Unicorn. This suite proves the
 * dataflow-pt-replay-tier exit criteria:
 *
 *   T1  test_pt_replay_path_matches_emu — build the offset path by running the
 *       EMULATOR ORACLE first (asmtest_dataflow_emu_run over the canonical ROUTINE),
 *       feed that path back into asmtest_dataflow_pt_replay_path, and assert the two
 *       value traces are byte-identical (insn_off + recs + wide, and a raw memcmp).
 *       NEGATIVE CONTROL: perturb one path offset -> DF_PT_FAULT, vt->truncated, a
 *       non-zero diverged_at. Unicorn only — runs wherever the blockstep lane runs.
 *   T2  test_pt_replay_from_fixture — the full bridge, driven by the SYNTHETIC PT
 *       AUX fixture (asmtest_pt_encode_fixture, libipt's own encoder, NO PT PMU): a
 *       self code-image tracks the ROUTINE bytes, the fixture is decoded + rebased +
 *       materialized + replayed, and the value trace is asserted equal to the emu
 *       oracle — for the TAKEN walk ({20,22} -> 42) and the NOT-TAKEN walk
 *       ({200,1}, the dec at 0xe runs), the same TNT discriminator test_wholewindow_
 *       decode uses, now proving the REPLAY follows the decoded path. libipt-gated.
 *   T3  test_pt_gates — an IMPURE region (cpuid) truncates with reason "cpuid" and
 *       info.pure==0; a VEX region truncates with reason "vex/evex"; BOTH execute
 *       NOTHING (steps==0). F5 reuses the blockstep verdicts (no second scanner).
 *   T5  test_pt_defuse_slice_equiv — the def-use (L1) graph and a backward slice
 *       (L2) built over the F5 replay trace EQUAL those built over the emu oracle
 *       trace: F5 is a drop-in L0 producer, not a special case the analysis knows.
 *   T4  test_pt_live_replay — the live foreign-pid single-step-oracle match. It CONSUMES
 *       the now-landed intel-pt-attach-foreign-pid capture (asmtest_hwtrace_pt_attach_*,
 *       that sibling is ☑5/5): forks a deterministic victim, PT-captures ONE in-region
 *       invocation with ZERO single-steps of the target, and asserts the F5 replay of the
 *       decoded path matches the emulator L0 AND the force_singlestep block-step oracle's
 *       executed path + GP result. Gated only on bare-metal Intel PT silicon now (a RUNTIME
 *       availability probe, no compile-time gate — the capture symbol links on every host),
 *       so it self-skips — or, under ASMTEST_REQUIRE_PT, fails-not-skips — naming that one
 *       remaining hardware gate.
 *
 * The tier ships NO header — a value-trace PRODUCER is a tier, not part of the
 * shared asmtest_valtrace.h sink API — so this suite re-declares its entry points,
 * the info struct, and the DF_PT_* codes here (exactly as test_dataflow_blockstep.c
 * re-declares its producer), and ASSERTS THE LAYOUT GUARD before trusting info.*.
 * Off Linux x86-64 / without Capstone+Unicorn the producer is an ENOSYS stub and
 * this suite prints a skip; built only where libunicorn is present (mk/dataflow.mk
 * gates the dataflow-pt-test lane on DF_HAVE_UNICORN). Linux x86-64.
 */
#define _GNU_SOURCE

#include "asmtest_codeimage.h" /* the temporal byte source the T2 bridge tracks */
#include "asmtest_hwtrace.h" /* T4: the landed intel-pt-attach-foreign-pid capture this tier CONSUMES */
#include "asmtest_valtrace.h" /* L0 sink + L1 def-use + L2 slice + at_val_rec_t */

#include <stddef.h> /* offsetof — the re-declared-struct layout guard */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h> /* PR_SET_PTRACER — the T4 live victim opts into being traced (Yama) */
#include <sys/wait.h> /* waitpid — reap the T4 live PT victim */
#endif

/* --- F5 producer surface, re-declared (the tier ships no header) --- */
#define DF_PT_OK     0
#define DF_PT_FAULT  1
#define DF_PT_EINVAL (-1)
#define DF_PT_ENOSYS (-3)

typedef struct {
    int pure;
    const char *reason;
    uint64_t steps;
    uint64_t path_len;
    uint64_t diverged_at;
    int vec_seeded;
} asmtest_dataflow_pt_info_t;

void asmtest_dataflow_pt_info_layout(size_t *size, size_t *last_off);

int asmtest_dataflow_pt_replay_path(const uint8_t *code, size_t code_len,
                                    const uint64_t *path, size_t path_len,
                                    const long *args, int nargs,
                                    asmtest_valtrace_t *vt,
                                    asmtest_dataflow_pt_info_t *info);

int asmtest_dataflow_pt_replay(const uint8_t *aux, size_t aux_len,
                               const asmtest_codeimage_t *img, uint64_t when,
                               uint64_t region_base, size_t region_len,
                               const long *args, int nargs,
                               asmtest_valtrace_t *vt,
                               asmtest_dataflow_pt_info_t *info);

/* The EMULATOR L0 ORACLE (src/dataflow_emu.c), re-declared exactly as
 * test_dataflow_emu.c does — the reference every data-flow tier cross-checks against. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);

/* The synthetic PT AUX fixture (src/pt_backend.c) — libipt's own packet encoder, NO PT
 * hardware. Only referenced under ASMTEST_HAVE_LIBIPT (else it is an ENOSYS stub anyway).
 * ASMTEST_HW_OK / ASMTEST_HWTRACE_INTEL_PT / the asmtest_hwtrace_pt_attach_* capture the
 * T4 live case consumes all come from asmtest_hwtrace.h, included above. */
int asmtest_pt_encode_fixture(uint8_t *buf, size_t cap, uint64_t base_ip,
                              int taken, size_t *out_len);

/* The single-step ORACLE (src/dataflow_blockstep.c), re-declared exactly as
 * test_dataflow_blockstep.c does — T4 compares the OUT-OF-BAND PT replay against the
 * force_singlestep=1 ground truth (which itself single-steps a forked child, so it runs on
 * any ptrace Linux; on a PT box both the capture and this oracle execute). The layout guard
 * in main() asserts these re-declarations match the producer before any info.* is trusted. */
typedef struct {
    uint64_t off, len;
} asmtest_blockstep_extent_t;

typedef struct {
    uint64_t max_insns;
    int force_singlestep;
    int inject_divergence;
    int inject_block;
    uint64_t region_off;
    int no_vec_seed;
    int no_mxcsr_seed;
    int no_vec_canary;
    int force_replay;
    uint64_t stack_hi_pad;
    int no_syscall_inject;
    int no_undef_mask;
    uint64_t inject_flag_bit;
    int no_hw_record;
    const asmtest_blockstep_extent_t *extents;
    size_t nextents;
} asmtest_blockstep_opts_t;

typedef struct {
    int pure;
    const char *reason;
    uint64_t stops;
    uint64_t steps;
    uint64_t entry_rsp;
    int vec_width;
    int vec_nregs;
    int uc_vec_width;
    int vec_seeded;
    int mxcsr_seeded;
    int injectable;
    uint64_t injected;
    uint64_t hw_hits;
} asmtest_blockstep_info_t;

int asmtest_dataflow_blockstep_run(const uint8_t *code, size_t code_len,
                                   const long *args, int nargs,
                                   const asmtest_blockstep_opts_t *opts,
                                   long *result, asmtest_valtrace_t *vt,
                                   asmtest_blockstep_info_t *info);
void asmtest_dataflow_blockstep_info_layout(size_t *size, size_t *last_off);
void asmtest_dataflow_blockstep_opts_layout(size_t *size, size_t *last_off);

static int checks, failures;
#define CHECK(c, ...)                                                          \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - " : "not ok %d - ", checks);                     \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* ------------------------------------------------------------------ */
/* Fixtures                                                            */
/* ------------------------------------------------------------------ */

/* The canonical ROUTINE the whole PT substrate keys on (identical bytes to
 * src/pt_backend.c and examples/test_hwtrace.c):
 *   0x00 mov rax,rdi ; 0x03 add rax,rsi ; 0x06 cmp rax,100 ; 0x0c jle +3 ;
 *   0x0e dec rax     ; 0x11 ret
 * Deterministic, register-only, pure + replayable. args {20,22} -> 42 <= 100 => jle
 * TAKEN, the 0xe dec SKIPPED, returns 42; args {200,1} -> 201 > 100 => jle NOT taken,
 * the 0xe dec RUNS, returns 200. */
static const uint8_t ROUTINE[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0,
                                  0x48, 0x3d, 0x64, 0x00, 0x00, 0x00,
                                  0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3};

/* T3: an IMPURE-but-decodable region (cpuid) and a NON-REPLAYABLE one (VEX-128). */
static const uint8_t imp_cpuid[] = {
    0xb8, 0x01, 0x00, 0x00, 0x00, 0x0f,
    0xa2, 0x48, 0x89, 0xf8, 0xc3}; /* ...;cpuid;mov rax,rdi;ret */
static const uint8_t vex128[] = {0xc5, 0xf1, 0xd4, 0xc2,
                                 0xc3}; /* vpaddq xmm0,xmm1,xmm2 ; ret */

/* ------------------------------------------------------------------ */
/* Byte-identical trace comparison (no rsp-normalization needed: the emu oracle and  */
/* the PT replay use the SAME fixed guest layout — code at DF_CODE_BASE, stack at     */
/* DF_STACK_BASE — so even absolute stack effective-addresses coincide).             */
/* ------------------------------------------------------------------ */

static int rec_eq(const at_val_rec_t *x, const at_val_rec_t *y) {
    return x->kind == y->kind && x->reg == y->reg && x->base == y->base &&
           x->index == y->index && x->scale == y->scale && x->disp == y->disp &&
           x->addr == y->addr && x->size == y->size &&
           x->is_write == y->is_write && x->value_valid == y->value_valid &&
           x->wide == y->wide && x->wide_off == y->wide_off &&
           x->value == y->value && x->step == y->step;
}

/* 1 iff A and B are identical over insn_off + recs + wide; *rawmemcmp gets whether a
 * literal memcmp of the record arrays also matched (the stricter bar, catching padding). */
static int traces_identical(const asmtest_valtrace_t *A,
                            const asmtest_valtrace_t *B, int *rawmemcmp) {
    if (rawmemcmp)
        *rawmemcmp = 0;
    if (A->steps_len != B->steps_len) {
        printf("#   steps_len differ: A=%zu B=%zu\n", A->steps_len,
               B->steps_len);
        return 0;
    }
    if (A->recs_len != B->recs_len) {
        printf("#   recs_len differ: A=%zu B=%zu\n", A->recs_len, B->recs_len);
        return 0;
    }
    for (size_t i = 0; i < A->steps_len; i++)
        if (A->insn_off[i] != B->insn_off[i]) {
            printf("#   insn_off[%zu] differ: A=0x%llx B=0x%llx\n", i,
                   (unsigned long long)A->insn_off[i],
                   (unsigned long long)B->insn_off[i]);
            return 0;
        }
    for (size_t i = 0; i < A->recs_len; i++)
        if (!rec_eq(&A->recs[i], &B->recs[i])) {
            const at_val_rec_t *x = &A->recs[i], *y = &B->recs[i];
            printf("#   rec[%zu] differ (step %u kind %d reg %u write %d): "
                   "A.value=0x%llx B.value=0x%llx\n",
                   i, x->step, (int)x->kind, x->reg, (int)x->is_write,
                   (unsigned long long)x->value, (unsigned long long)y->value);
            return 0;
        }
    if (A->wide_len != B->wide_len ||
        (A->wide_len && memcmp(A->wide, B->wide, A->wide_len) != 0)) {
        printf("#   wide[] differ\n");
        return 0;
    }
    if (rawmemcmp)
        *rawmemcmp =
            (A->recs_len == B->recs_len) &&
            memcmp(A->recs, B->recs, A->recs_len * sizeof(at_val_rec_t)) == 0;
    return 1;
}

/* Run the emulator oracle over `code` with `args`, returning a freshly allocated valtrace
 * (caller frees) or NULL. */
static asmtest_valtrace_t *emu_oracle(const uint8_t *code, size_t code_len,
                                      const long *args, int nargs) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(256, 8192, 4096);
    if (v == NULL)
        return NULL;
    if (asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, v) != 0) {
        asmtest_valtrace_free(v);
        return NULL;
    }
    return v;
}

/* The final GP result of a value trace = the last value written to rax (Capstone RAX id 35).
 * AT_ prefix so it does not collide with <sys/ucontext.h>'s REG_RAX (pulled in via <sys/wait.h>). */
#define AT_REG_RAX 35
static int last_rax(const asmtest_valtrace_t *v, uint64_t *out) {
    int found = 0;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->kind == AT_LOC_REG && r->reg == AT_REG_RAX && r->is_write &&
            r->value_valid) {
            *out = r->value;
            found = 1;
        }
    }
    return found;
}

/* ------------------------------------------------------------------ */
/* T1 — replay a hand-fed (oracle-derived) path; byte-identical + control            */
/* ------------------------------------------------------------------ */

static void test_pt_replay_path_matches_emu(void) {
    long args[2] = {20, 22};
    asmtest_valtrace_t *oracle = emu_oracle(ROUTINE, sizeof ROUTINE, args, 2);
    if (oracle == NULL) {
        CHECK(0, "T1: emu oracle over ROUTINE{20,22}");
        return;
    }
    /* Harvest the executed offset path from the oracle — exactly what a decoded PT trace yields. */
    size_t path_len = oracle->steps_len;
    uint64_t *path = (uint64_t *)malloc(path_len * sizeof(uint64_t));
    if (path == NULL) {
        CHECK(0, "T1: malloc path");
        asmtest_valtrace_free(oracle);
        return;
    }
    for (size_t i = 0; i < path_len; i++)
        path[i] = oracle->insn_off[i];

    asmtest_valtrace_t *replay = asmtest_valtrace_new(256, 8192, 4096);
    asmtest_dataflow_pt_info_t info;
    int rc = asmtest_dataflow_pt_replay_path(ROUTINE, sizeof ROUTINE, path,
                                             path_len, args, 2, replay, &info);
    CHECK(rc == DF_PT_OK && !replay->truncated,
          "T1: clean replay of the oracle path (rc=%d truncated=%d steps=%llu "
          "path_len=%llu pure=%d)",
          rc, (int)replay->truncated, (unsigned long long)info.steps,
          (unsigned long long)info.path_len, info.pure);
    CHECK(info.steps == path_len && info.path_len == path_len &&
              info.diverged_at == 0,
          "T1: replayed every path step, no divergence (steps=%llu of %zu)",
          (unsigned long long)info.steps, path_len);

    uint64_t rr = 0, ro = 0;
    CHECK(last_rax(replay, &rr) && last_rax(oracle, &ro) && rr == 42 &&
              ro == 42,
          "T1: both traces return rax = a+b = 42 (oracle=%llu replay=%llu)",
          (unsigned long long)ro, (unsigned long long)rr);

    int raw = 0;
    int same = traces_identical(oracle, replay, &raw);
    CHECK(same,
          "T1: PT replay value trace is BYTE-IDENTICAL to the emulator oracle "
          "(%zu steps, %zu records)",
          oracle->steps_len, oracle->recs_len);
    CHECK(raw,
          "T1: raw memcmp of the record arrays is identical too (no struct-"
          "padding divergence)");

    /* NEGATIVE CONTROL: perturb one offset in the path -> the replay must DIVERGE. path[2] is the
     * cmp at 0x6; a bogus 0x99 there makes step 2's executed offset mismatch. */
    if (path_len > 2) {
        uint64_t *bad = (uint64_t *)malloc(path_len * sizeof(uint64_t));
        if (bad != NULL) {
            memcpy(bad, path, path_len * sizeof(uint64_t));
            bad[2] = 0x99;
            asmtest_valtrace_t *pv = asmtest_valtrace_new(256, 8192, 4096);
            asmtest_dataflow_pt_info_t bi;
            int brc = asmtest_dataflow_pt_replay_path(
                ROUTINE, sizeof ROUTINE, bad, path_len, args, 2, pv, &bi);
            CHECK(brc == DF_PT_FAULT && pv->truncated && bi.diverged_at == 2,
                  "T1 control: a perturbed path offset TRUNCATES the replay "
                  "(rc=%d truncated=%d diverged_at=%llu — non-zero)",
                  brc, (int)pv->truncated, (unsigned long long)bi.diverged_at);
            asmtest_valtrace_free(pv);
            free(bad);
        }
    }

    free(path);
    asmtest_valtrace_free(replay);
    asmtest_valtrace_free(oracle);
}

/* ------------------------------------------------------------------ */
/* T3 — the purity / replayability gates truncate honestly, execute nothing          */
/* ------------------------------------------------------------------ */

static void test_pt_gates(void) {
    /* A trivial (irrelevant) path — the gate fires BEFORE any execution, so its contents do not
     * matter; what matters is that nothing runs. */
    static const uint64_t dummy_path[] = {0x0};

    /* IMPURE: cpuid in the region -> declined, reason "cpuid", pure==0, nothing executed. */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(256, 8192, 4096);
        asmtest_dataflow_pt_info_t info;
        int rc = asmtest_dataflow_pt_replay_path(
            imp_cpuid, sizeof imp_cpuid, dummy_path, 1, NULL, 0, v, &info);
        CHECK(rc == DF_PT_FAULT && info.pure == 0 && info.reason != NULL &&
                  strcmp(info.reason, "cpuid") == 0 && v->truncated &&
                  info.steps == 0,
              "T3: an IMPURE region (cpuid) truncates — pure=%d reason=%s "
              "steps=%llu (nothing executed)",
              info.pure, info.reason ? info.reason : "?",
              (unsigned long long)info.steps);
        asmtest_valtrace_free(v);
    }

    /* NON-REPLAYABLE: a VEX-128 encoding -> declined (Unicorn would run it silently WRONG),
     * reason "vex/evex", nothing executed. */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(256, 8192, 4096);
        asmtest_dataflow_pt_info_t info;
        int rc = asmtest_dataflow_pt_replay_path(
            vex128, sizeof vex128, dummy_path, 1, NULL, 0, v, &info);
        CHECK(rc == DF_PT_FAULT && info.reason != NULL &&
                  strcmp(info.reason, "vex/evex") == 0 && v->truncated &&
                  info.steps == 0,
              "T3: a NON-REPLAYABLE region (VEX-128) truncates — reason=%s "
              "steps=%llu (correctness gate, nothing executed)",
              info.reason ? info.reason : "?", (unsigned long long)info.steps);
        asmtest_valtrace_free(v);
    }
}

/* ------------------------------------------------------------------ */
/* T5 — F5's valtrace is a first-class L0: def-use + slice EQUAL the oracle's         */
/* ------------------------------------------------------------------ */

static int defuse_equal(const asmtest_defuse_t *a, const asmtest_defuse_t *b) {
    if (a == NULL || b == NULL || a->n != b->n || a->nsteps != b->nsteps)
        return 0;
    for (size_t i = 0; i < a->n; i++) {
        if (a->edges[i].from_step != b->edges[i].from_step ||
            a->edges[i].to_step != b->edges[i].to_step ||
            !rec_eq(&a->edges[i].loc, &b->edges[i].loc))
            return 0;
    }
    return 1;
}

static int slice_equal(const asmtest_slice_t *a, const asmtest_slice_t *b) {
    if (a == NULL || b == NULL || a->n != b->n)
        return 0;
    for (size_t i = 0; i < a->n; i++)
        if (a->steps[i] != b->steps[i])
            return 0;
    return 1;
}

static void test_pt_defuse_slice_equiv(void) {
    long args[2] = {20, 22};
    asmtest_valtrace_t *oracle = emu_oracle(ROUTINE, sizeof ROUTINE, args, 2);
    if (oracle == NULL) {
        CHECK(0, "T5: emu oracle");
        return;
    }
    size_t path_len = oracle->steps_len;
    uint64_t *path = (uint64_t *)malloc(path_len * sizeof(uint64_t));
    asmtest_valtrace_t *replay = asmtest_valtrace_new(256, 8192, 4096);
    if (path == NULL || replay == NULL) {
        CHECK(0, "T5: alloc");
        goto done;
    }
    for (size_t i = 0; i < path_len; i++)
        path[i] = oracle->insn_off[i];
    asmtest_dataflow_pt_info_t info;
    if (asmtest_dataflow_pt_replay_path(ROUTINE, sizeof ROUTINE, path, path_len,
                                        args, 2, replay, &info) != DF_PT_OK) {
        CHECK(0, "T5: replay");
        goto done;
    }

    asmtest_defuse_t *go = asmtest_defuse_build(oracle);
    asmtest_defuse_t *gr = asmtest_defuse_build(replay);
    CHECK(go != NULL && gr != NULL && go->n > 0 && defuse_equal(go, gr),
          "T5: F5's def-use graph EQUALS the emulator oracle's (%zu edges "
          "over %zu steps) — F5 is a drop-in L1 producer",
          go ? go->n : 0, go ? go->nsteps : 0);

    /* Backward slice from the ROUTINE's result — seed at the deepest consumer (the greatest
     * to_step in the graph, e.g. the `jle` reading the `cmp`'s flags), so the slice walks the
     * whole arithmetic def-use chain (jle <- cmp <- add <- mov) rather than a trivial single step.
     * Identical traces yield identical slices, proving the L2 slicer treats F5 as an ordinary L0. */
    uint32_t seed_step = 0;
    for (size_t i = 0; i < go->n; i++)
        if (go->edges[i].to_step > seed_step)
            seed_step = go->edges[i].to_step;
    at_val_rec_t seed;
    memset(&seed, 0, sizeof seed);
    seed.step = seed_step;
    asmtest_slice_t *so = asmtest_slice_backward(go, seed);
    asmtest_slice_t *sr = asmtest_slice_backward(gr, seed);
    CHECK(so != NULL && sr != NULL && so->n > 1 && slice_equal(so, sr),
          "T5: F5's backward slice from step %u EQUALS the oracle's, walking "
          "the def-use chain (%zu steps)",
          seed_step, so ? so->n : 0);

    asmtest_slice_free(so);
    asmtest_slice_free(sr);
    asmtest_defuse_free(go);
    asmtest_defuse_free(gr);
done:
    free(path);
    asmtest_valtrace_free(replay);
    asmtest_valtrace_free(oracle);
}

/* ------------------------------------------------------------------ */
/* T2 — the full decode->rebase->materialize->replay bridge, SYNTHETIC AUX           */
/* ------------------------------------------------------------------ */

#if defined(ASMTEST_HAVE_LIBIPT)
/* One bridge run: encode the fixture for `taken` at the tracked address, replay it with `args`,
 * and assert the value trace equals the emu oracle (byte-identical) and returns `want`. */
static void run_fixture_case(const char *name, asmtest_codeimage_t *img,
                             uint64_t base, uint64_t when, int taken,
                             const long *args, int nargs, uint64_t want) {
    uint8_t aux[256];
    size_t aux_len = 0;
    int enc = asmtest_pt_encode_fixture(aux, sizeof aux, base, taken, &aux_len);
    if (enc != ASMTEST_HW_OK || aux_len == 0) {
        CHECK(0, "T2 %s: encode fixture (rc=%d)", name, enc);
        return;
    }
    asmtest_valtrace_t *replay = asmtest_valtrace_new(256, 8192, 4096);
    asmtest_dataflow_pt_info_t info;
    int rc =
        asmtest_dataflow_pt_replay(aux, aux_len, img, when, base,
                                   sizeof ROUTINE, args, nargs, replay, &info);
    CHECK(rc == DF_PT_OK && !replay->truncated,
          "T2 %s: decode->rebase->materialize->replay clean (rc=%d "
          "truncated=%d steps=%llu)",
          name, rc, (int)replay->truncated, (unsigned long long)info.steps);

    asmtest_valtrace_t *oracle =
        emu_oracle(ROUTINE, sizeof ROUTINE, args, nargs);
    if (oracle == NULL) {
        CHECK(0, "T2 %s: emu oracle", name);
        asmtest_valtrace_free(replay);
        return;
    }
    uint64_t rr = 0;
    CHECK(last_rax(replay, &rr) && rr == want,
          "T2 %s: replayed result rax = %llu (want %llu)", name,
          (unsigned long long)rr, (unsigned long long)want);
    int raw = 0;
    CHECK(traces_identical(oracle, replay, &raw),
          "T2 %s: replay value trace is byte-identical to the emu oracle (the "
          "REPLAY follows the DECODED path, not a baked-in answer)",
          name);
    asmtest_valtrace_free(oracle);
    asmtest_valtrace_free(replay);
}

static void test_pt_replay_from_fixture(void) {
    if (!asmtest_codeimage_available()) {
        char why[256];
        asmtest_codeimage_skip_reason(why, sizeof why);
        printf("# SKIP T2 fixture bridge: code-image recorder unavailable "
               "(%s) — the recorder-backed decode needs it\n",
               why);
        return;
    }
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        ps = 4096;
    uint8_t *p = (uint8_t *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP T2 fixture bridge: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, (size_t)ps, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_codeimage_t *img = asmtest_codeimage_new(0);
    int trk = img ? asmtest_codeimage_track(img, p, sizeof ROUTINE) : -1;
    if (trk != ASMTEST_CI_OK) {
        printf("# SKIP T2 fixture bridge: could not track ROUTINE (rc=%d)\n",
               trk);
        if (img)
            asmtest_codeimage_free(img);
        munmap(p, (size_t)ps);
        return;
    }
    uint64_t base = (uint64_t)(uintptr_t)
        p; /* fixture base_ip == region_base -> rebase is identity */
    uint64_t when = asmtest_codeimage_now(img);

    long taken_args[2] = {20, 22};
    run_fixture_case("taken(20,22)", img, base, when, /*taken*/ 1, taken_args,
                     2, /*want*/ 42);
    long nt_args[2] = {200, 1};
    run_fixture_case("not-taken(200,1) — the 0xe dec runs", img, base, when,
                     /*taken*/ 0, nt_args, 2, /*want*/ 200);

    asmtest_codeimage_free(img);
    munmap(p, (size_t)ps);
}
#else
static void test_pt_replay_from_fixture(void) {
    printf("# SKIP T2 fixture bridge: built without libipt (no PT decode) — "
           "exercised in the docker-dataflow-pt lane (libipt+Unicorn image)\n");
}
#endif /* ASMTEST_HAVE_LIBIPT */

/* ------------------------------------------------------------------ */
/* T4 — live foreign-pid PT capture + single-step oracle match                        */
/*                                                                                    */
/* The sibling intel-pt-attach-foreign-pid (now ☑5/5) OWNS the capture: this tier opens */
/* NO perf event (doc-set position 9). It forks a deterministic victim, PT-captures ONE */
/* in-region invocation with ZERO single-steps via asmtest_hwtrace_pt_attach_*, then    */
/* replays the decoded offset path through F5 and asserts the value trace matches the   */
/* emulator L0 AND the force_singlestep block-step oracle's executed path + GP result.  */
/*                                                                                    */
/* The ONE remaining gate is bare-metal Intel PT silicon (the intel_pt PMU with         */
/* perf_event_paranoid<0 / CAP_PERFMON) — the single legitimate self-skip under         */
/* CLAUDE.md. asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT) is the RUNTIME probe;  */
/* there is no compile-time gate any more — the capture symbol is in the tree, so the    */
/* live body always LINKS and is reachable the instant a PT box runs it.                */
/* ------------------------------------------------------------------ */
#if defined(__linux__) && defined(__x86_64__)
#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* Fork a victim that runs ROUTINE(a,b) ONCE on a pipe go-signal, PT-capture that single
 * invocation over the foreign pid with no single-step, and return the decoded trace (its
 * insns[] hold ABSOLUTE addresses = base_ip + offset, the pt_attach_end window ABI).
 * *result_out gets the victim's region return value. Returns a heap asmtest_trace_t* (caller
 * frees) or NULL on setup failure. The fork shares the ROUTINE mapping at [base, base+len). */
static asmtest_trace_t *pt_capture_one_region(uint64_t base, size_t len, long a,
                                              long b, long *result_out) {
    int go[2], done[2], stay[2];
    if (pipe(go) != 0)
        return NULL;
    if (pipe(done) != 0) {
        close(go[0]);
        close(go[1]);
        return NULL;
    }
    if (pipe(stay) != 0) {
        close(go[0]);
        close(go[1]);
        close(done[0]);
        close(done[1]);
        return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(go[0]);
        close(go[1]);
        close(done[0]);
        close(done[1]);
        close(stay[0]);
        close(stay[1]);
        return NULL;
    }
    if (pid == 0) {
        /* Victim: opt into ptrace (Yama), wait for the parent to arm PT, run the region
         * exactly ONCE, hand the result back — then STAY ALIVE until the parent has
         * decoded. The decode walks the victim's own .text (where TIP.PGE landed, in the
         * caller) via process_vm_readv, so the victim must outlive attach_end; the parent
         * closes stay[1] once decode is done. A closed go pipe (parent bailed) reads EOF —
         * it still runs once and exits, so the parent's waitpid never hangs. */
        int pr = prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
        (void)pr;
        close(go[1]);
        close(done[0]);
        close(stay[1]);
        long (*fn)(long, long) = (long (*)(long, long))(uintptr_t)base;
        char c;
        ssize_t rn = read(go[0], &c, 1);
        (void)rn;
        long r = fn(a, b);
        ssize_t wn = write(done[1], &r, sizeof r);
        (void)wn;
        ssize_t sn =
            read(stay[0], &c, 1); /* block until the parent finishes decode */
        (void)sn;
        _exit(0);
    }
    /* Parent: arm PT on the foreign pid BEFORE releasing the child, so the one region walk
     * falls inside the capture window. The ROUTINE mapping is anonymous => obj_hint NULL =>
     * pt_attach_end's software IP post-filter (scoped by attach_track) keeps only in-region
     * offsets. */
    close(go[0]);
    close(done[1]);
    close(stay[0]);
    asmtest_pt_attach_t *at = NULL;
    asmtest_trace_t *tr = NULL;
    long r = 0;
    int brc = asmtest_hwtrace_pt_attach_begin((int)pid, NULL, &at);
    if (brc == ASMTEST_HW_OK && at != NULL) {
        asmtest_hwtrace_pt_attach_track(at, base, len);
        char one = 1;
        ssize_t wn =
            write(go[1], &one, 1); /* release: victim runs the region */
        (void)wn;
        ssize_t rn =
            read(done[0], &r, sizeof r); /* wait for region completion */
        (void)rn;
        int trunc = 0;
        asmtest_hwtrace_pt_attach_poll(at, 100, &trunc);
        tr = asmtest_trace_new(256, 64);
        if (tr != NULL) {
            int erc = asmtest_hwtrace_pt_attach_end(at, 0, tr);
            if (erc != ASMTEST_HW_OK) {
                if (getenv("ASMTEST_PT_DEBUG"))
                    fprintf(stderr, "# pt capture: attach_end rc=%d\n", erc);
                asmtest_trace_free(tr);
                tr = NULL;
            }
        } else {
            asmtest_hwtrace_pt_attach_end(at, 0, NULL); /* teardown only */
        }
    } else if (getenv("ASMTEST_PT_DEBUG")) {
        fprintf(stderr, "# pt capture: attach_begin rc=%d at=%p\n", brc,
                (void *)at);
    }
    close(stay[1]); /* let the (now-decoded) victim exit */
    close(go[1]);   /* EOF unblocks the child even if we never released it */
    close(done[0]);
    int st;
    waitpid(pid, &st, 0);
    if (result_out)
        *result_out = r;
    return tr;
}

/* One live case: capture ROUTINE(a,b), replay the decoded path through F5, and assert the
 * out-of-band value trace matches BOTH the emulator L0 and the single-step block-step oracle.
 * want = ROUTINE's own semantics (a+b, or a+b-1 when a+b>100 so the 0xe dec runs) — a property
 * of the args, never a baked constant. */
static void run_live_case(uint64_t base, size_t len, long a, long b) {
    char nm[64];
    snprintf(nm, sizeof nm, "live(%ld,%ld)", a, b);
    long args[2] = {a, b};
    long want = (a + b <= 100) ? (a + b) : (a + b - 1);

    long vres = 0;
    asmtest_trace_t *cap = pt_capture_one_region(base, len, a, b, &vres);
    if (cap == NULL) {
        CHECK(0, "T4 %s: PT capture of one region invocation", nm);
        return;
    }
    CHECK((uint64_t)vres == (uint64_t)want,
          "T4 %s: victim region returned %ld (want %ld)", nm, vres, want);

    /* Decoded insns[] are ABSOLUTE (base_ip + offset) — recover region-relative offsets. */
    size_t n = cap->insns_len;
    uint64_t *path = (uint64_t *)malloc((n ? n : 1) * sizeof(uint64_t));
    if (path == NULL) {
        CHECK(0, "T4 %s: malloc decoded path", nm);
        asmtest_trace_free(cap);
        return;
    }
    int in_region = (n > 0);
    for (size_t i = 0; i < n; i++) {
        if (cap->insns[i] < base || cap->insns[i] >= base + len)
            in_region = 0;
        else
            path[i] = cap->insns[i] - base;
    }
    CHECK(in_region && !cap->truncated,
          "T4 %s: decoded %zu in-region offsets, no truncation", nm, n);

    /* The single-step ground truth (force_singlestep=1) — its executed offset path is what the
     * PT decode must equal, and its GP result the cross-check for the victim's. */
    asmtest_blockstep_opts_t oo;
    memset(&oo, 0, sizeof oo);
    oo.force_singlestep = 1;
    asmtest_valtrace_t *ss = asmtest_valtrace_new(256, 8192, 4096);
    asmtest_blockstep_info_t si;
    long sres = 0;
    int src = asmtest_dataflow_blockstep_run(ROUTINE, sizeof ROUTINE, args, 2,
                                             &oo, &sres, ss, &si);
    int path_eq = (src == 0 && ss != NULL && ss->steps_len == n);
    for (size_t i = 0; path_eq && i < n; i++)
        if (ss->insn_off[i] != path[i])
            path_eq = 0;
    CHECK(
        path_eq && (uint64_t)sres == (uint64_t)want,
        "T4 %s: PT-decoded path == single-step oracle path (%zu steps, result "
        "%ld==%ld)",
        nm, n, sres, want);

    /* Replay the PT path through F5 and prove the reconstructed value trace is byte-identical
     * to the emulator L0. ROUTINE is register-only (no memory records), so the doc's
     * rsp-relative normalization is a no-op here — the register value trace is process-
     * independent, so a byte-identical compare is exact. */
    asmtest_valtrace_t *replay = asmtest_valtrace_new(256, 8192, 4096);
    asmtest_dataflow_pt_info_t info;
    int rc = asmtest_dataflow_pt_replay_path(ROUTINE, sizeof ROUTINE, path, n,
                                             args, 2, replay, &info);
    uint64_t rr = 0;
    CHECK(rc == DF_PT_OK && !replay->truncated && last_rax(replay, &rr) &&
              rr == (uint64_t)want,
          "T4 %s: F5 replay of the PT path is clean, rax=%llu (want %llu)", nm,
          (unsigned long long)rr, (unsigned long long)want);
    asmtest_valtrace_t *emu = emu_oracle(ROUTINE, sizeof ROUTINE, args, 2);
    int raw = 0;
    CHECK(emu != NULL && traces_identical(emu, replay, &raw),
          "T4 %s: F5 replay value trace is byte-identical to the emulator L0 "
          "(zero single-steps of the target)",
          nm);

    if (emu != NULL)
        asmtest_valtrace_free(emu);
    asmtest_valtrace_free(replay);
    asmtest_valtrace_free(ss);
    free(path);
    asmtest_trace_free(cap);
}

static void test_pt_live_replay(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_INTEL_PT)) {
        const char *msg = "pt live replay: no intel_pt PMU (needs bare-metal "
                          "Intel; absent on AMD/VM/CI)";
        if (getenv("ASMTEST_REQUIRE_PT") != NULL) {
            /* Fail-not-skip: a runner that CLAIMS PT (ASMTEST_REQUIRE_PT=1) goes RED on a
             * silently-hidden PMU, exactly as intel-pt-whole-window-substrate#T5's
             * hwtrace-pt-live does. The sibling capture symbol is in the tree now, so the ONLY
             * thing keeping this red is missing silicon. */
            CHECK(0, "T4 [ASMTEST_REQUIRE_PT=1]: %s", msg);
        } else {
            printf("# SKIP %s\n", msg);
        }
        return;
    }
    /* Intel PT present: run the real out-of-band capture + oracle match on BOTH walks. */
    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        ps = 4096;
    uint8_t *p = (uint8_t *)mmap(NULL, (size_t)ps, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        CHECK(0, "T4: mmap ROUTINE for the live victim");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, (size_t)ps, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
    uint64_t base = (uint64_t)(uintptr_t)p;
    run_live_case(base, sizeof ROUTINE, 20,
                  22); /* a+b=42<=100 -> jle taken, 0xe skipped */
    run_live_case(base, sizeof ROUTINE, 200,
                  1); /* a+b=201>100 -> jle not taken, 0xe runs */
    munmap(p, (size_t)ps);
}
#else
static void test_pt_live_replay(void) {
    printf("# SKIP pt live replay: not Linux x86-64\n");
}
#endif

/* ------------------------------------------------------------------ */

int main(void) {
    printf("# F5: PT + code-image + Unicorn-replay value tier\n");

    /* Before ANY info.* field below is trusted: this suite re-declares asmtest_dataflow_pt_info_t
     * (the tier ships no header). A silent layout skew between the two copies corrupts every field
     * read out of `info` — exactly the F6 hazard the blockstep tier's guard exists for (`size`
     * alone misses a field absorbed by tail padding; `last_off` moves whenever an earlier field
     * changes). Check it, do not assume it. */
    {
        size_t isz = 0, ioff = 0;
        asmtest_dataflow_pt_info_layout(&isz, &ioff);
        CHECK(isz == sizeof(asmtest_dataflow_pt_info_t) &&
                  ioff == offsetof(asmtest_dataflow_pt_info_t, vec_seeded),
              "layout: the suite's re-declared info struct matches the "
              "producer's SIZE (%zu) and final-field OFFSET (%zu)",
              isz, ioff);
    }

    /* Same F6-hazard guard for the block-step oracle structs the T4 live case re-declares
     * (opts + info from src/dataflow_blockstep.c). These run on EVERY host (the layout fns are
     * defined unconditionally, in the real producer and its ENOSYS stub alike), so a skew in
     * the re-declared oracle structs is caught HERE — before a PT box ever runs the gated live
     * body that passes these structs to asmtest_dataflow_blockstep_run. */
    {
        size_t osz = 0, ooff = 0;
        asmtest_dataflow_blockstep_opts_layout(&osz, &ooff);
        CHECK(osz == sizeof(asmtest_blockstep_opts_t) &&
                  ooff == offsetof(asmtest_blockstep_opts_t, nextents),
              "layout: the re-declared block-step OPTS struct matches the "
              "producer's SIZE (%zu) and final-field OFFSET (%zu)",
              osz, ooff);
        size_t bisz = 0, bioff = 0;
        asmtest_dataflow_blockstep_info_layout(&bisz, &bioff);
        CHECK(bisz == sizeof(asmtest_blockstep_info_t) &&
                  bioff == offsetof(asmtest_blockstep_info_t, hw_hits),
              "layout: the re-declared block-step INFO struct matches the "
              "producer's SIZE (%zu) and final-field OFFSET (%zu)",
              bisz, bioff);
    }

    /* Platform probe: on a build without Capstone+Unicorn the producer is an ENOSYS stub. */
    {
        asmtest_valtrace_t *v = asmtest_valtrace_new(8, 64, 0);
        long a[2] = {20, 22};
        uint64_t path0[1] = {0};
        asmtest_dataflow_pt_info_t info;
        int rc = asmtest_dataflow_pt_replay_path(ROUTINE, sizeof ROUTINE, path0,
                                                 0, a, 2, v, &info);
        asmtest_valtrace_free(v);
        if (rc == DF_PT_ENOSYS) {
            printf("# SKIP dataflow-pt: built off Linux x86-64 / without "
                   "Capstone+Unicorn\n1..%d\n",
                   checks);
            return failures ? 1 : 0;
        }
    }

    test_pt_replay_path_matches_emu(); /* T1 */
    test_pt_gates();                   /* T3 */
    test_pt_replay_from_fixture();     /* T2 (libipt-gated) */
    test_pt_defuse_slice_equiv();      /* T5 */
    test_pt_live_replay(); /* T4 (Intel-PT-silicon-gated; self-skips off PT) */

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    else
        printf("# all %d checks passed\n", checks);
    return failures ? 1 : 0;
}
