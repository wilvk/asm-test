/*
 * dr_taint_simd.c — DynamoRIO taint tier (Increment 8): XMM/YMM (SSE/AVX) SIMD taint.
 *
 * Proves the SIMD extension of the in-band inline dst_tag = union(src_tags) producer: taint
 * flows THROUGH an XMM OR YMM register AND an SSE (16-byte) OR AVX (32-byte) vectorized copy,
 * oracle-diffed against the def-use forward slice exactly as examples/dr_taint.c does for the
 * GP/integer path (the YMM modes are AVX-gated and skip cleanly on a non-AVX CPU). It
 * runs a self-contained x86-64 routine NATIVELY, in-band, whole-process under DynamoRIO with
 * the taint client (src/dataflow_dr_client_inlined.c built -DASMTEST_TAINT), SEEDS a 16-byte
 * buffer, and asserts the client's per-step taint witness (dv->step_taint) equals the
 * emulator-oracle FORWARD SLICE from the seed step (asmtest_slice_forward) — the inline XMM
 * propagation reproduces def-use forward reachability across XMM registers + the 16-byte SSE
 * memory shadow. A sink variant asserts a tainted value reaching a conditional-branch SINK
 * produces one at_taint_hit_t with the right off/tag/kind. Each has a negative control
 * (unseeded => empty taint set / zero hits).
 *
 * This mirrors dr_taint.c's structure + oracle discipline; the ADDITIVE value capture still
 * runs. Named dr_taint_simd.c (not test_*.c) so the root Makefile's SUITES wildcard does not
 * sweep this standalone DynamoRIO harness into the forking runner; it is wired + run by
 * mk/native-trace.mk's dr-taint-simd-test lane (copy / negative / sink / sink-negative).
 * Self-skips cleanly (exit 0) when DynamoRIO / the client is unavailable.
 *
 * Fixtures (leaf, x86-64 SysV — rdi=buf seeded 16B, rsi=buf2 a fresh 16B heap buffer):
 *   simd_copy: movdqu xmm0,[rdi] / movdqa xmm1,xmm0 / movdqu [rsi],xmm1 / mov rax,[rsi] / ret
 *              -> taint through an XMM register (xmm0->xmm1) AND an SSE vectorized copy
 *                 (16-byte load into xmm0, 16-byte store to buf2), then a GP reload. The
 *                 16-byte movdqu load/store exercise the SIMD memory shadow; the movdqa the
 *                 XMM reg-tag file. forward(step0) = {0,1,2,3} (ret excluded).
 *   simd_sink: movdqu xmm0,[rdi] / movdqa xmm1,xmm0 / movq rcx,xmm1 / test rcx,rcx /
 *              jz +3 / mov rax,rcx / ret
 *              -> the seeded XMM lane reaches a GP register (movq) then a branch condition
 *                 (test/jz): the tainted ZF is a SINK (kind = 1) at 0x10. forward(step0) =
 *                 {0,1,2,3,4,5}; def-use depth seed(step0)->jz(step4) = 4.
 */
#include "asmtest_valtrace.h"

#include "asmtest_taint.h" /* at_tag_t, AT_TAG_TAINTED */

#include "taint_simd_fixtures.h" /* simd_copy, simd_sink, SEED16, SEED_LO64 (shared) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Producers ship no header (a value-trace PRODUCER is a tier, not part of the shared
 * sink API), so re-declare the entry points + return codes here, as dr_taint does. */
#define DF_DR_OK     0
#define DF_DR_FAULT  1
#define DF_DR_EINVAL (-1)
#define DF_DR_ENOSYS (-3)
#define DF_DR_ENODR  (-4)

int asmtest_dataflow_dr_available(void);
int asmtest_dataflow_dr_taint_run(const uint8_t *code, size_t code_len,
                                  const long *args, int nargs,
                                  uint64_t max_insns, uint64_t seed_base,
                                  uint64_t seed_len, at_tag_t seed_color,
                                  long *result, asmtest_valtrace_t *vt,
                                  at_tag_t *step_taint, size_t step_taint_cap,
                                  at_taint_report_t *report);

#ifdef DF_HAVE_EMU
/* The emulator L0 producer (secondary oracle); linked + declared only where Unicorn is
 * present. NOTE: this repo's Unicorn build enables no SSE by default (dataflow_emu.c sets no
 * CR0/CR4), so an SSE fixture may fail to execute — the cross-check below is therefore
 * TOLERANT (SKIP on emu failure). The PRIMARY oracle is the DR run's own forward slice. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
/* Two mapped pointers inside the emulator's stack window ([0x200000,0x210000), dataflow_emu.c)
 * so both movdqu [rdi] and movdqu [rsi] land in mapped memory; 0x40 apart >= 16 bytes. */
#define EMU_MAPPED_PTR  0x00200100L
#define EMU_MAPPED_PTR2 0x00200140L
#endif

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* simd_copy / simd_sink / SEED16 / SEED_LO64 moved VERBATIM to
 * examples/taint_simd_fixtures.h (included above) so the libdft64 oracle's named-skip arm
 * runs the byte-identical XMM inputs. This lane is the proof no fixture byte changed. */
#define SINK_OFF   0x10 /* the jz instruction's region offset */
#define SINK_DEPTH 4    /* def-use edges seed(step0) -> jz(step4) */

static int slices_equal(const asmtest_slice_t *a, const asmtest_slice_t *b) {
    if (a == NULL || b == NULL || a->n != b->n)
        return 0;
    for (size_t i = 0; i < a->n; i++)
        if (a->steps[i] != b->steps[i])
            return 0;
    return 1;
}

static int step_at_off(const asmtest_valtrace_t *v, uint64_t off) {
    for (size_t i = 0; i < v->steps_len; i++)
        if (v->insn_off[i] == off)
            return (int)i;
    return -1;
}

/* BFS shortest def-use distance from `from` to `to`, or -1 (mirrors dr_taint.c). */
static int defuse_depth(const asmtest_defuse_t *g, size_t nsteps, int from,
                        int to) {
    if (g == NULL || from < 0 || to < 0 || (size_t)from >= nsteps ||
        (size_t)to >= nsteps)
        return -1;
    int *dist = (int *)malloc(nsteps * sizeof *dist);
    int *q = (int *)malloc(nsteps * sizeof *q);
    if (dist == NULL || q == NULL) {
        free(dist);
        free(q);
        return -1;
    }
    for (size_t i = 0; i < nsteps; i++)
        dist[i] = -1;
    int head = 0, tail = 0;
    dist[from] = 0;
    q[tail++] = from;
    while (head < tail) {
        int cur = q[head++];
        for (size_t e = 0; e < g->n; e++) {
            if ((int)g->edges[e].from_step != cur)
                continue;
            int nx = (int)g->edges[e].to_step;
            if (nx >= 0 && (size_t)nx < nsteps && dist[nx] == -1) {
                dist[nx] = dist[cur] + 1;
                q[tail++] = nx;
            }
        }
    }
    int d = dist[to];
    free(dist);
    free(q);
    return d;
}

static void fill_seed_and_depth(at_taint_report_t *report,
                                const asmtest_defuse_t *g,
                                const asmtest_valtrace_t *v, int seed_step) {
    for (size_t i = 0; i < report->hits_len; i++) {
        at_taint_hit_t *h = &report->hits[i];
        int sink_step = step_at_off(v, h->off);
        h->seed_off = (seed_step >= 0) ? v->insn_off[seed_step] : 0;
        int d = defuse_depth(g, v->steps_len, seed_step, sink_step);
        h->depth = (d >= 0) ? (uint32_t)d : 0;
    }
}

/* Copy scenario: seed M[buf] (16 bytes), run, and diff the client taint witness against the
 * forward slice from the seed step (the XMM/SIMD path must equal def-use forward reach). */
static void test_copy(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "copy: valtrace_new");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(16);
    uint8_t *buf2 = (uint8_t *)malloc(16);
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "copy: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    memcpy(buf, SEED16, 16);
    memset(buf2, 0, 16);
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        simd_copy, sizeof simd_copy, args, 2, 0, (uint64_t)(uintptr_t)buf,
        /*seed_len*/ 16, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK,
          "copy: routine captured in-band under the taint client");
    CHECK(
        result == (long)SEED_LO64,
        "copy: rax = low 8 bytes round-tripped via the SSE copy through buf2");
    CHECK(v->steps_len == 5, "copy: five in-region steps captured");

    /* The taint witness set must be exactly {0,1,2,3} (ret excluded). */
    if (v->steps_len == 5) {
        CHECK(step_taint[0] && step_taint[1] && step_taint[2] &&
                  step_taint[3] && !step_taint[4],
              "copy: steps 0-3 tainted (xmm0->xmm1->buf2->rax), ret clean");
    }

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL, "copy: def-use graph built over the in-band SIMD trace");
    at_val_rec_t seed = {0};
    seed.step =
        0; /* the 16-byte load from the seeded buffer is the taint origin */
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 3) && !asmtest_slice_contains(fwd, 4),
          "copy: forward slice(step0) = {0,1,2,3} across XMM + SIMD memory");

    /* THE ORACLE DIFF: the client's inline SIMD taint set must equal the forward slice. */
    int mism = 0;
    for (size_t i = 0; i < v->steps_len; i++) {
        int tainted = step_taint[i] != 0;
        int inslice = fwd ? asmtest_slice_contains(fwd, (uint32_t)i) : 0;
        if (tainted != inslice)
            mism++;
    }
    CHECK(mism == 0,
          "copy: client SIMD taint set == asmtest_slice_forward(seed "
          "step) [ORACLE DIFF]");

#ifdef DF_HAVE_EMU
    /* Secondary cross-check against the independent emulator oracle's forward slice. Tolerant:
     * this repo's Unicorn build may not execute SSE (no CR4.OSFXSR), so a failed emu run is a
     * SKIP, not a failure — the primary oracle above is the hard gate. */
    asmtest_valtrace_t *ve = asmtest_valtrace_new(64, 512, 512);
    long eargs[2] = {EMU_MAPPED_PTR, EMU_MAPPED_PTR2};
    int erc = ve ? asmtest_dataflow_emu_run(simd_copy, sizeof simd_copy, eargs,
                                            2, 0, ve)
                 : -1;
    if (erc == 0) {
        asmtest_defuse_t *ge = asmtest_defuse_build(ve);
        asmtest_slice_t *efwd = ge ? asmtest_slice_forward(ge, seed) : NULL;
        CHECK(efwd && slices_equal(fwd, efwd),
              "copy: DR forward slice == emulator oracle forward slice");
        asmtest_slice_free(efwd);
        asmtest_defuse_free(ge);
    } else {
        printf("# SKIP emulator SIMD forward-slice cross-check: emu run rc=%d "
               "(Unicorn SSE unsupported in this build)\n",
               erc);
    }
    asmtest_valtrace_free(ve);
#else
    printf("# SKIP emulator forward-slice cross-check: built without "
           "libunicorn\n");
#endif

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* Negative control: the SAME copy fixture, unseeded (seed_len=0). The def-use structure is
 * unchanged, but the client must report ZERO tainted steps — proving SIMD taint is seed-gated
 * (value-driven), not an artifact of the XMM/SSE structure. */
static void test_negative(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "negative: valtrace_new");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(16);
    uint8_t *buf2 = (uint8_t *)malloc(16);
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "negative: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    memcpy(buf, SEED16, 16);
    memset(buf2, 0, 16);
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        simd_copy, sizeof simd_copy, args, 2, 0, /*seed_base*/ 0,
        /*seed_len*/ 0, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK, "negative: routine captured in-band (unseeded)");
    CHECK(result == (long)SEED_LO64,
          "negative: routine still round-trips (value capture intact)");
    CHECK(v->steps_len == 5, "negative: five in-region steps captured");

    int any = 0;
    for (size_t i = 0; i < v->steps_len; i++)
        if (step_taint[i])
            any++;
    CHECK(any == 0,
          "negative: zero tainted steps (unseeded => empty SIMD taint set)");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    CHECK(fwd && fwd->n == 4,
          "negative: forward slice still {0,1,2,3} while taint set is empty");
    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* Sink scenario: a seeded XMM lane reaches a GP register then a conditional-branch sink =>
 * one at_taint_hit_t with the right off/tag/kind, its off in the forward slice, and the
 * app-side def-use BFS resolving seed_off + depth. */
static void test_sink(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "sink: valtrace_new");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(16);
    uint8_t *buf2 = (uint8_t *)malloc(16);
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "sink: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    memcpy(buf, SEED16, 16);
    memset(buf2, 0, 16);
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        simd_sink, sizeof simd_sink, args, 2, 0, (uint64_t)(uintptr_t)buf,
        /*seed_len*/ 16, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK,
          "sink: routine captured in-band under the taint client");
    CHECK(result == (long)SEED_LO64,
          "sink: rcx = low 8 bytes via movq xmm1->rcx (jz not taken)");
    CHECK(v->steps_len == 7, "sink: seven in-region steps captured");
    CHECK(report.hits_total == 1 && report.hits_len == 1 && !report.truncated,
          "sink: exactly one taint hit recorded");
    if (report.hits_len == 1) {
        at_taint_hit_t *h = &report.hits[0];
        CHECK(h->off == SINK_OFF, "sink: hit offset is the jz branch (0x10)");
        CHECK(h->tag != AT_TAG_CLEAN, "sink: hit tag is tainted");
        CHECK(h->kind == 1, "sink: hit kind = 1 (branch condition)");
        CHECK(h->ea == 0, "sink: hit ea = 0 (register/flag sink)");
    }

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    int sink_step = step_at_off(v, SINK_OFF);
    CHECK(fwd && sink_step >= 0 &&
              asmtest_slice_contains(fwd, (uint32_t)sink_step),
          "sink: sink instruction is in forward slice(seed) [ORACLE]");

    fill_seed_and_depth(&report, g, v, 0);
    if (report.hits_len == 1) {
        CHECK(report.hits[0].seed_off == 0x00,
              "sink: app-side seed_off resolves to the seed load (0x00)");
        CHECK(report.hits[0].depth == SINK_DEPTH,
              "sink: app-side depth = 4 def-use edges seed->sink");
    }

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* Sink negative control: the SAME sink fixture unseeded => the branch flag stays clean =>
 * zero hits (the SIMD-fed sink is seed-gated, not structural). */
static void test_sink_negative(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "sink-negative: valtrace_new");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(16);
    uint8_t *buf2 = (uint8_t *)malloc(16);
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "sink-negative: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    memcpy(buf, SEED16, 16);
    memset(buf2, 0, 16);
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        simd_sink, sizeof simd_sink, args, 2, 0, /*seed_base*/ 0,
        /*seed_len*/ 0, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK, "sink-negative: routine captured (unseeded)");
    CHECK(report.hits_total == 0 && report.hits_len == 0 && !report.truncated,
          "sink-negative: zero hits (unseeded => clean flag never reaches "
          "sink)");

    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* ==== Increment 8 YMM/AVX slice: 256-bit vector taint (breadth over the SSE slice above) ====
 * Structural clones of simd_copy/simd_sink using VEX-encoded AVX (vmovdqu/vmovdqa ymm, 32-byte
 * loads/stores; a 2-byte VEX op is 4 bytes, exactly like the SSE forms, so the offsets line up).
 * The reduction to a GP register is through MEMORY (a 32-byte store then an 8-byte reload), NOT a
 * YMM->XMM sub-register read, so the oracle def-use needs no XMM/YMM sub-register aliasing —
 * exactly how the SSE copy reduces. These prove taint flows through a YMM register AND an AVX
 * 32-byte vectorized copy, and (sink) reaches a branch condition. AVX-gated at the call site. */
static const uint8_t SEED32[32] = {
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, /* low 8 -> SEED_LO64 */
    0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
};

/* ymm_copy: taint through a YMM register AND an AVX 32-byte vectorized copy. */
static const uint8_t ymm_copy[] = {
    0xc5, 0xfe, 0x6f, 0x07, /* 0x00 vmovdqu ymm0, [rdi]  (SEED load, 32B) */
    0xc5, 0xfd, 0x6f, 0xc8, /* 0x04 vmovdqa ymm1, ymm0   (YMM reg copy)   */
    0xc5, 0xfe, 0x7f, 0x0e, /* 0x08 vmovdqu [rsi], ymm1  (32B store->buf2)*/
    0x48, 0x8b, 0x06,       /* 0x0c mov     rax, [rsi]   (reload low 8)   */
    0xc3,                   /* 0x0f ret                                   */
};

/* ymm_sink: a seeded YMM lane reaches a GP register (via a 32B store + 8B reload) then a
 * branch flag (SINK). forward(step0) = {0,1,2,3,4,5}; seed(step0)->jz(step4) = 4 edges. */
static const uint8_t ymm_sink[] = {
    0xc5, 0xfe, 0x6f, 0x07, /* 0x00 vmovdqu ymm0, [rdi]  (SEED load, 32B) */
    0xc5, 0xfe, 0x7f, 0x06, /* 0x04 vmovdqu [rsi], ymm0  (32B store->buf2)*/
    0x48, 0x8b, 0x0e,       /* 0x08 mov     rcx, [rsi]   (reload low 8)   */
    0x48, 0x85, 0xc9,       /* 0x0b test    rcx, rcx     (taint ZF)       */
    0x74, 0x03,             /* 0x0e jz 0x15             (SINK)            */
    0x48, 0x89, 0xc8,       /* 0x10 mov     rax, rcx                      */
    0xc3,                   /* 0x13 ret                                   */
};
#define YMM_SINK_OFF   0x0e /* the jz instruction's region offset */
#define YMM_SINK_DEPTH 4    /* def-use edges seed(step0) -> jz(step4) */

/* YMM copy oracle: seeded => the client YMM taint set == the forward slice {0,1,2,3};
 * unseeded => zero tainted steps (seed-gated, not structural). */
static void run_ymm_copy(int seeded) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "ymm-copy: valtrace_new");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(32);
    uint8_t *buf2 = (uint8_t *)malloc(32);
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "ymm-copy: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    memcpy(buf, SEED32, 32);
    memset(buf2, 0, 32);
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        ymm_copy, sizeof ymm_copy, args, 2, 0,
        seeded ? (uint64_t)(uintptr_t)buf : 0, seeded ? 32 : 0, AT_TAG_TAINTED,
        &result, v, step_taint, sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK,
          "ymm-copy: routine captured under the taint client (AVX)");
    CHECK(result == (long)SEED_LO64,
          "ymm-copy: rax = low 8 bytes round-tripped via the AVX 32-byte copy");
    CHECK(v->steps_len == 5, "ymm-copy: five in-region steps captured");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    CHECK(fwd && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 3) && !asmtest_slice_contains(fwd, 4),
          "ymm-copy: forward slice(step0) = {0,1,2,3} across YMM + AVX memory");

    if (seeded) {
        int mism = 0;
        for (size_t i = 0; i < v->steps_len; i++) {
            int tainted = step_taint[i] != 0;
            int inslice = fwd ? asmtest_slice_contains(fwd, (uint32_t)i) : 0;
            if (tainted != inslice)
                mism++;
        }
        CHECK(mism == 0, "ymm-copy: client YMM taint set == "
                         "asmtest_slice_forward(seed step) [ORACLE DIFF]");
    } else {
        int any = 0;
        for (size_t i = 0; i < v->steps_len; i++)
            if (step_taint[i])
                any++;
        CHECK(any == 0, "ymm-negative: unseeded => zero tainted steps (empty "
                        "YMM taint set)");
    }

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* YMM sink: seeded => one at_taint_hit_t at the jz (right off/tag/kind + app-side depth);
 * unseeded => zero hits. */
static void run_ymm_sink(int seeded) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "ymm-sink: valtrace_new");
        return;
    }
    uint8_t *buf = (uint8_t *)malloc(32);
    uint8_t *buf2 = (uint8_t *)malloc(32);
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "ymm-sink: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    memcpy(buf, SEED32, 32);
    memset(buf2, 0, 32);
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        ymm_sink, sizeof ymm_sink, args, 2, 0,
        seeded ? (uint64_t)(uintptr_t)buf : 0, seeded ? 32 : 0, AT_TAG_TAINTED,
        &result, v, step_taint, sizeof step_taint / sizeof step_taint[0],
        &report);

    CHECK(rc == DF_DR_OK,
          "ymm-sink: routine captured under the taint client (AVX)");
    if (seeded) {
        CHECK(report.hits_total == 1 && report.hits_len == 1 &&
                  !report.truncated,
              "ymm-sink: exactly one taint hit (YMM seed reached the branch)");
        if (report.hits_len == 1) {
            at_taint_hit_t *h = &report.hits[0];
            CHECK(h->off == YMM_SINK_OFF,
                  "ymm-sink: hit offset is the jz branch (0x0e)");
            CHECK(h->tag != AT_TAG_CLEAN, "ymm-sink: hit tag is tainted");
            CHECK(h->kind == 1, "ymm-sink: hit kind = 1 (branch condition)");
        }
        asmtest_defuse_t *g = asmtest_defuse_build(v);
        at_val_rec_t seed = {0};
        seed.step = 0;
        asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
        int sink_step = step_at_off(v, YMM_SINK_OFF);
        CHECK(fwd && sink_step >= 0 &&
                  asmtest_slice_contains(fwd, (uint32_t)sink_step),
              "ymm-sink: sink instruction is in forward slice(seed) [ORACLE]");
        fill_seed_and_depth(&report, g, v, 0);
        if (report.hits_len == 1)
            CHECK(report.hits[0].depth == YMM_SINK_DEPTH,
                  "ymm-sink: app-side depth = 4 def-use edges seed->sink");
        asmtest_slice_free(fwd);
        asmtest_defuse_free(g);
    } else {
        CHECK(report.hits_total == 0 && report.hits_len == 0 &&
                  !report.truncated,
              "ymm-sink-negative: zero hits (unseeded => clean flag never "
              "reaches sink)");
    }

    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a hard kill */
    if (!asmtest_dataflow_dr_available()) {
        printf("# SKIP dr-taint-simd: DynamoRIO / taint client unavailable\n"
               "1..0\n");
        return 0;
    }
    const char *mode = (argc > 1) ? argv[1] : "";
    /* The YMM/AVX modes need a CPU with AVX; skip cleanly (not fail) where it is absent. */
    if (strncmp(mode, "ymm", 3) == 0 && !__builtin_cpu_supports("avx")) {
        printf("# SKIP dr-taint-simd %s: AVX not available on this CPU\n1..0\n",
               mode);
        return 0;
    }
    if (strcmp(mode, "negative") == 0)
        test_negative();
    else if (strcmp(mode, "sink") == 0)
        test_sink();
    else if (strcmp(mode, "sink-negative") == 0)
        test_sink_negative();
    else if (strcmp(mode, "ymm-copy") == 0)
        run_ymm_copy(1);
    else if (strcmp(mode, "ymm-negative") == 0)
        run_ymm_copy(0);
    else if (strcmp(mode, "ymm-sink") == 0)
        run_ymm_sink(1);
    else if (strcmp(mode, "ymm-sink-negative") == 0)
        run_ymm_sink(0);
    else
        test_copy();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
