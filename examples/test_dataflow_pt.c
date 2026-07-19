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
 *   T4  test_pt_live_replay — the live foreign-pid single-step-oracle match. DOUBLE-
 *       gated (bare-metal Intel PT silicon AND the sibling intel-pt-attach-foreign-
 *       pid capture symbol, absent today), so it self-skips naming BOTH gates.
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
#include "asmtest_valtrace.h" /* L0 sink + L1 def-use + L2 slice + at_val_rec_t */

#include <stddef.h> /* offsetof — the re-declared-struct layout guard */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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
 * hardware. Only referenced under ASMTEST_HAVE_LIBIPT (else it is an ENOSYS stub anyway). */
#define ASMTEST_HW_OK 0
int asmtest_pt_encode_fixture(uint8_t *buf, size_t cap, uint64_t base_ip,
                              int taken, size_t *out_len);

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

/* The final GP result of a value trace = the last value written to rax (Capstone RAX id 35). */
#define REG_RAX 35
static int last_rax(const asmtest_valtrace_t *v, uint64_t *out) {
    int found = 0;
    for (size_t i = 0; i < v->recs_len; i++) {
        const at_val_rec_t *r = &v->recs[i];
        if (r->kind == AT_LOC_REG && r->reg == REG_RAX && r->is_write &&
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
/* T4 — live foreign-pid + single-step oracle match (DOUBLE-gated, self-skips)        */
/* ------------------------------------------------------------------ */

static void test_pt_live_replay(void) {
#if defined(__linux__) && defined(__x86_64__)
    /* T4 is gated on TWO independent, un-installable prerequisites:
     *   (1) BARE-METAL INTEL PT SILICON — the intel_pt PMU with perf_event_paranoid<0 or
     *       CAP_PERFMON. This host is AMD Zen 2; VMs, Docker, and GitHub-hosted runners hide
     *       the PMU. (Hardware gate — the one legitimate self-skip under the CLAUDE.md rule.)
     *   (2) THE SIBLING intel-pt-attach-foreign-pid CAPTURE — its #T1 foreign-pid AUX capture
     *       entry and #T2 paired live code-image (position 9: F5 opens NO perf event; it
     *       CONSUMES that doc's capture). That dependency is 0/5: the capture entry SYMBOL does
     *       not exist in the tree yet, so a call to it cannot even be COMPILED. The live body is
     *       therefore guarded out below (ASMTEST_PT_FOREIGN_CAPTURE is never defined today) with
     *       a TODO tied to that doc, rather than referencing a symbol that does not link.
     *
     * When BOTH land, the body: spawn a deterministic victim (bindings/dataflow_victim.c shape —
     * publishes base=/len=/pid=, args from argv), capture its PT trace over one region invocation
     * with NO single-step of the target, asmtest_dataflow_pt_replay it, and assert (a) the value
     * trace's region result == a+b, (b) truncated == false, and (c) it matches the single-step
     * oracle from asmtest_dataflow_blockstep_run(force_singlestep=1), compared rsp-relative. */
#if defined(                                                                   \
    ASMTEST_PT_FOREIGN_CAPTURE) /* the sibling capture symbol — absent today */
    /* TODO(intel-pt-attach-foreign-pid#T1/#T2): call the foreign-pid capture entry here to obtain
     * (a) the linearized AUX blob and (b) the code-image it tracked, then feed both to
     * asmtest_dataflow_pt_replay and oracle-match against blockstep force_singlestep. */
#else
    const char *msg =
        "pt live replay: no intel_pt PMU (needs bare-metal Intel; absent on "
        "AMD/VM/CI) AND intel-pt-attach-foreign-pid capture symbol not in tree "
        "yet (sibling dep 0/5)";
    if (getenv("ASMTEST_REQUIRE_PT") != NULL) {
        /* Fail-not-skip: a runner that CLAIMS PT (ASMTEST_REQUIRE_PT=1) must go RED on a
         * silently-hidden PMU, exactly as intel-pt-whole-window-substrate#T5's hwtrace-pt-live
         * does. Here it also stays red until the sibling capture symbol lands. */
        CHECK(0, "T4 [ASMTEST_REQUIRE_PT=1]: %s", msg);
    } else {
        printf("# SKIP %s\n", msg);
    }
#endif
#else
    printf("# SKIP pt live replay: not Linux x86-64\n");
#endif
}

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
    test_pt_live_replay();             /* T4 (double-gated, self-skips) */

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    else
        printf("# all %d checks passed\n", checks);
    return failures ? 1 : 0;
}
