/*
 * dr_taint.c — DynamoRIO taint tier (Increment 4): the in-band inline
 * dst_tag = union(src_tags) producer + seed/sink surface end-to-end. Runs a self-
 * contained x86-64 routine NATIVELY, in-band, whole-process under DynamoRIO with the
 * taint client (src/dataflow_dr_client_inlined.c built -DASMTEST_TAINT), SEEDS a known
 * buffer, and (a) asserts the client's per-step taint witness (dv->step_taint) equals
 * the emulator-oracle FORWARD SLICE from the seed step — inline propagation reproduces
 * def-use forward reachability — and (b) asserts a tainted value reaching a SINK produces
 * one at_taint_hit_t with the right off/tag/kind. Three sink kinds are exercised: a
 * conditional-branch condition (kind 1), a call ARGUMENT register (kind 2), and a
 * memory-copy LENGTH — the rep-movs count register (kind 0). For the branch and mem-len
 * sinks the hit's off is in the forward slice and the app-side def-use BFS resolves
 * seed_off + depth; the call-arg sink is calling-convention-based (a direct call does not
 * machine-read its args), so its oracle tie is the arg's DEFINING move being in the slice.
 * Each sink has a negative control (unseeded => empty taint set / zero hits).
 *
 * This mirrors dr_valtrace.c's structure + oracle discipline; the ADDITIVE value
 * capture still runs (asserted: result + step count), so this also proves the taint
 * client fills at_drval_t identically. Named dr_taint.c (not test_*.c) so the root
 * Makefile's SUITES wildcard does not sweep this standalone DynamoRIO harness into the
 * forking runner; it is wired + run explicitly by mk/native-trace.mk's
 * dr-taint-native-test lane. Modes selected by argv[1]: seeded / negative / sink /
 * sink-negative / heapstore / highbyte / callarg / callarg-negative / memlen /
 * memlen-negative — the sink family (sink/callarg/memlen + each negative) covers all
 * three at_taint_hit_t kinds (1 branch, 2 call-arg, 0 mem-len).
 *
 * DynamoRIO permits ONE in-process lifecycle per process, so each invocation drives ONE
 * scenario, selected by argv[1] (default = seeded). Self-skips cleanly (exit 0) when
 * DynamoRIO / the client is unavailable.
 *
 * Fixtures (leaf, x86-64 SysV — rdi=buf, rsi=b/buf2), all reading a SEEDED memory buffer
 * at step0 so taint originates in the shadow:
 *   taint_chain:      mov rax,[rdi] / mov [rsp-8],rax / mov rcx,[rsp-8] /
 *                     lea rdx,[rcx+rsi] / mov rax,rdx / ret   (propagation oracle; also
 *                     the `highbyte` per-byte-union case, seeding only buf's high bytes)
 *   taint_sink_chain: ... / add rcx,rsi / jz +3 / ...         (branch-condition sink)
 *   taint_heapstore:  mov rax,[rdi] / mov [rsi],rax / mov rcx,[rsi] / ... (create-on-
 *                     touch through a fresh, never-pre-touched heap store)
 *   taint_callarg:    mov rax,[rdi] / mov rdi,rax / call +1 / ret / ret  (call-arg sink:
 *                     the tainted value reaches call ARG0 rdi)
 *   taint_memlen:     mov rcx,[rdi] / rep movsb / mov rax,rcx / ret      (mem-len sink:
 *                     the tainted value is the rep-movs LENGTH in rcx)
 * The store/reload exercise the integer-memory shadow, the moves/lea/add the reg + flag
 * tags; forward(step0) excludes ret.
 */
#include "asmtest_valtrace.h"

#include "asmtest_taint.h" /* at_tag_t, AT_TAG_TAINTED */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Producers ship no header (a value-trace PRODUCER is a tier, not part of the shared
 * sink API), so re-declare the entry points + return codes here, as dr_valtrace does. */
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
/* The emulator L0 producer (oracle); linked + declared only where Unicorn is present. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
/* The emulator maps a fixed code/stack window (dataflow_emu.c) and cannot read the DR
 * run's host heap buffer, so the oracle run points rdi at a mapped stack address — the
 * def-use STRUCTURE (hence the forward slice) is value/address independent. */
#define EMU_MAPPED_PTR 0x00200100L
#endif

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

static const uint8_t taint_chain[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]     (SEED origin) */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax   (spill)       */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]   (reload)      */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi]               */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx                     */
    0xc3,                         /* 0x14 ret                              */
};

/* Sink fixture (rdi=buf seeded, rsi=b): derives the seeded value into a flag, then
 * branches on it — the tainted flag reaches a conditional-branch SINK (kind = 1).
 *   taint_sink_chain: mov rax,[rdi] / mov [rsp-8],rax / mov rcx,[rsp-8] /
 *                     add rcx,rsi / jz +3 / mov rax,rcx / ret
 * add taints rcx AND the flags; the jz at 0x10 reads the tainted ZF => one hit at 0x10.
 * forward(step0) = {0,1,2,3,4,5} (jz at step4, mov rax,rcx at step5; ret excluded). */
static const uint8_t taint_sink_chain[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]   (SEED origin)    */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax (spill)          */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8] (reload)         */
    0x48, 0x01, 0xf1,             /* 0x0d add rcx, rsi     (rcx+flags taint)*/
    0x74, 0x03,                   /* 0x10 jz 0x15          (SINK: taint ZF) */
    0x48, 0x89, 0xc8,             /* 0x12 mov rax, rcx                      */
    0xc3,                         /* 0x15 ret                               */
};
#define SINK_OFF   0x10 /* the jz instruction's region offset */
#define SINK_DEPTH 4    /* def-use edges seed(step0) -> jz(step4)          */

/* Create-on-touch fixture (rdi=buf seeded, rsi=buf2 a FRESH heap buffer, never seeded
 * or otherwise touched in the shadow): stores the tainted value to buf2 then reloads it.
 * The store to buf2 hits a null leaf => the inline store-tag SLOWPATH must create the
 * leaf on touch, or the reloaded rcx would come back clean.
 *   taint_heapstore: mov rax,[rdi] / mov [rsi],rax / mov rcx,[rsi] / mov rax,rcx / ret
 * forward(step0) = {0,1,2,3} (ret excluded). */
static const uint8_t taint_heapstore[] = {
    0x48, 0x8b, 0x07, /* 0x00 mov rax, [rdi] (SEED origin)              */
    0x48, 0x89, 0x06, /* 0x03 mov [rsi], rax (store to fresh heap buf2) */
    0x48, 0x8b, 0x0e, /* 0x06 mov rcx, [rsi] (reload from buf2)         */
    0x48, 0x89, 0xc8, /* 0x09 mov rax, rcx                              */
    0xc3,             /* 0x0c ret                                       */
};

/* Call-arg sink fixture (rdi=buf seeded): moves the seeded value into rdi (SysV arg0), then
 * CALLs — a tainted value reaches a call ARGUMENT register (kind = 2). A direct call does not
 * machine-read its argument registers, so this sink is calling-convention-based (rdi watched at
 * the call site), decoupled from the value trace's machine-level def-use — hence, unlike the
 * branch/mem-len sinks, the CALL step itself is not in forward(seed); the arg's DEFINING move
 * is (asserted below). The nested call/ret is balanced (the leaf runs as a normal C function).
 *   taint_callarg: mov rax,[rdi] / mov rdi,rax / call +1 / ret / ret
 * one hit at off 0x06, kind 2; forward(step0) includes the mov rdi,rax at 0x03. */
static const uint8_t taint_callarg[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]  (SEED origin)      */
    0x48, 0x89, 0xc7,             /* 0x03 mov rdi, rax    (arg0 tainted)     */
    0xe8, 0x01, 0x00, 0x00, 0x00, /* 0x06 call 0x0c       (SINK: arg tainted)*/
    0xc3,                         /* 0x0b ret             (to harness)       */
    0xc3,                         /* 0x0c ret             (callee -> 0x0b)   */
};
#define CALLARG_OFF 0x06 /* the call instruction's region offset */

/* Mem-len sink fixture (rdi=buf seeded holding a SMALL byte count; rsi=buf2 a readable src):
 * loads the seeded value as a length into rcx (the string-copy count register), then rep movsb
 * — a tainted value reaches a memory-copy LENGTH (kind = 0). rcx IS a machine source of rep
 * movs, so def-use connects seed->rep-movs and the sink step is in forward(seed).
 *   taint_memlen: mov rcx,[rdi] / rep movsb / mov rax,rcx / ret
 * one hit at off 0x03, kind 0; forward(step0) = {0,1,2} (ret excluded), depth 1. */
static const uint8_t taint_memlen[] = {
    0x48, 0x8b, 0x0f, /* 0x00 mov rcx, [rdi] (SEED origin: tainted count)  */
    0xf3, 0xa4,       /* 0x03 rep movsb      (SINK: mem-copy length rcx)   */
    0x48, 0x89, 0xc8, /* 0x05 mov rax, rcx   (rax = 0 -> deterministic)    */
    0xc3,             /* 0x08 ret                                          */
};
#define MEMLEN_OFF   0x03 /* the rep movsb instruction's region offset      */
#define MEMLEN_DEPTH 1    /* def-use edges seed(step0) -> rep movsb(step1)  */

static int slices_equal(const asmtest_slice_t *a, const asmtest_slice_t *b) {
    if (a == NULL || b == NULL || a->n != b->n)
        return 0;
    for (size_t i = 0; i < a->n; i++)
        if (a->steps[i] != b->steps[i])
            return 0;
    return 1;
}

/* The trace step whose instruction offset is `off`, or -1. */
static int step_at_off(const asmtest_valtrace_t *v, uint64_t off) {
    for (size_t i = 0; i < v->steps_len; i++)
        if (v->insn_off[i] == off)
            return (int)i;
    return -1;
}

/* BFS shortest def-use distance from `from` to `to` over the graph's edges, or -1 if
 * unreachable. This is the app-side "validator's def-use BFS" the taint ABI defers the
 * hit's seed_off/depth to (the client leaves them 0). */
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

/* Fill each hit's app-side seed_off + depth (the client leaves them 0): seed_off is the
 * seed step's offset; depth is the def-use BFS distance seed_step -> sink_step. */
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

/* Seeded scenario: seed M[buf], run, and diff the client taint witness against the
 * forward slice from the seed step. */
static void test_seeded(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "seeded: valtrace_new");
        return;
    }

    /* A real host buffer the fixture loads from; its address is BOTH arg0 (rdi) and the
     * seed base. Value 7 + b(5) => rax = 12, matching df_chain's result. */
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "seeded: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 5};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_chain, sizeof taint_chain, args, 2, 0, (uint64_t)(uintptr_t)buf,
        sizeof(uint64_t), AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK,
          "seeded: routine captured in-band under the taint client");
    CHECK(result == 12, "seeded: routine returned 12 (rax = *buf + b)");
    CHECK(v->steps_len == 6,
          "seeded: six in-region steps captured (value capture intact)");

    /* The taint witness set must be exactly {0,1,2,3,4} (ret excluded). */
    if (v->steps_len == 6) {
        CHECK(step_taint[0] && step_taint[1] && step_taint[2] &&
                  step_taint[3] && step_taint[4] && !step_taint[5],
              "seeded: steps 0-4 tainted, ret (step5) clean");
    }

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    CHECK(g != NULL, "seeded: def-use graph built over the in-band trace");
    at_val_rec_t seed = {0};
    seed.step = 0; /* the load from the seeded buffer is the taint origin */
    asmtest_slice_t *fwd = asmtest_slice_forward(g, seed);
    CHECK(fwd && asmtest_slice_contains(fwd, 0) &&
              asmtest_slice_contains(fwd, 4) && !asmtest_slice_contains(fwd, 5),
          "seeded: forward slice(step0) = {0,1,2,3,4}, excludes ret");

    /* THE ORACLE DIFF: the client's inline taint set must equal the forward slice. */
    int mism = 0;
    for (size_t i = 0; i < v->steps_len; i++) {
        int tainted = step_taint[i] != 0;
        int inslice = fwd ? asmtest_slice_contains(fwd, (uint32_t)i) : 0;
        if (tainted != inslice)
            mism++;
    }
    CHECK(mism == 0, "seeded: client taint set == asmtest_slice_forward(seed "
                     "step) [ORACLE DIFF]");

#ifdef DF_HAVE_EMU
    /* Cross-check the DR forward slice against the independent emulator oracle's forward
     * slice (structure only — the emulator run points rdi at a mapped address). */
    asmtest_valtrace_t *ve = asmtest_valtrace_new(64, 512, 512);
    long eargs[2] = {EMU_MAPPED_PTR, 5};
    int erc = ve ? asmtest_dataflow_emu_run(taint_chain, sizeof taint_chain,
                                            eargs, 2, 0, ve)
                 : -1;
    asmtest_defuse_t *ge = (erc == 0) ? asmtest_defuse_build(ve) : NULL;
    asmtest_slice_t *efwd = ge ? asmtest_slice_forward(ge, seed) : NULL;
    CHECK(efwd && slices_equal(fwd, efwd),
          "seeded: DR forward slice == emulator oracle forward slice");
    asmtest_slice_free(efwd);
    asmtest_defuse_free(ge);
    asmtest_valtrace_free(ve);
#else
    printf("# SKIP emulator forward-slice cross-check: built without "
           "libunicorn\n");
#endif

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    asmtest_valtrace_free(v);
}

/* Negative control: the SAME fixture, unseeded (seed_len=0). The def-use structure is
 * unchanged, but the client must report ZERO tainted steps — proving taint is seed-
 * gated (value-driven), not an artifact of the structure. */
static void test_negative(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "negative: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "negative: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 5};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_chain, sizeof taint_chain, args, 2, 0, /*seed_base*/ 0,
        /*seed_len*/ 0, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK, "negative: routine captured in-band (unseeded)");
    CHECK(result == 12,
          "negative: routine still returned 12 (value capture intact)");
    CHECK(v->steps_len == 6, "negative: six in-region steps captured");

    int any = 0;
    for (size_t i = 0; i < v->steps_len; i++)
        if (step_taint[i])
            any++;
    CHECK(any == 0,
          "negative: zero tainted steps (unseeded => empty taint set)");

    /* The def-use forward slice is STILL {0,1,2,3,4} — structure unchanged, taint empty. */
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    CHECK(fwd && fwd->n == 5,
          "negative: forward slice still {0,1,2,3,4} while taint set is empty");
    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    asmtest_valtrace_free(v);
}

/* Sink scenario: a tainted flag reaches a conditional-branch sink => one at_taint_hit_t
 * with the right off/tag/kind, its off in the forward slice, and the app-side def-use
 * BFS resolving seed_off + depth. */
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
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "sink: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 5};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_sink_chain, sizeof taint_sink_chain, args, 2, 0,
        (uint64_t)(uintptr_t)buf, sizeof(uint64_t), AT_TAG_TAINTED, &result, v,
        step_taint, sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK,
          "sink: routine captured in-band under the taint client");
    CHECK(result == 12,
          "sink: routine returned 12 (rcx = *buf + b; jz not taken)");
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

    /* App-side validator fills seed_off + depth from the def-use graph. */
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
    asmtest_valtrace_free(v);
}

/* Sink negative control: the SAME sink fixture unseeded => the branch flag stays clean
 * => zero hits (the sink is seed-gated, not structural). */
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
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "sink-negative: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 5};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_sink_chain, sizeof taint_sink_chain, args, 2, 0, /*seed_base*/ 0,
        /*seed_len*/ 0, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK, "sink-negative: routine captured (unseeded)");
    CHECK(
        report.hits_total == 0 && report.hits_len == 0 && !report.truncated,
        "sink-negative: zero hits (unseeded => clean flag never reaches sink)");

    free(buf);
    asmtest_valtrace_free(v);
}

/* Call-arg sink scenario (kind = 2): a tainted value reaches a call ARGUMENT register => one
 * at_taint_hit_t with the right off/tag/kind. The arg's DEFINING move is in forward(seed)
 * [ORACLE tie-in]; the call step itself is not (a direct call does not machine-read its args,
 * so the call-arg sink is calling-convention-based, decoupled from machine-level def-use). */
static void test_callarg(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "callarg: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "callarg: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 0};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_callarg, sizeof taint_callarg, args, 2, 0,
        (uint64_t)(uintptr_t)buf, sizeof(uint64_t), AT_TAG_TAINTED, &result, v,
        step_taint, sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK,
          "callarg: routine captured in-band under the taint client");
    CHECK(result == 7, "callarg: routine returned 7 (rax = *buf, unchanged)");
    CHECK(report.hits_total == 1 && report.hits_len == 1 && !report.truncated,
          "callarg: exactly one taint hit recorded");
    if (report.hits_len == 1) {
        at_taint_hit_t *h = &report.hits[0];
        CHECK(h->off == CALLARG_OFF, "callarg: hit offset is the call (0x06)");
        CHECK(h->tag != AT_TAG_CLEAN, "callarg: hit tag is tainted");
        CHECK(h->kind == 2, "callarg: hit kind = 2 (call argument)");
        CHECK(h->ea == 0, "callarg: hit ea = 0 (register sink)");
    }

    /* ORACLE tie-in: the argument's DEFINING move (mov rdi,rax at 0x03) is in forward(seed) —
     * the tainted value that reached the arg is genuinely seed-derived. */
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    int argdef = step_at_off(v, 0x03);
    CHECK(fwd && argdef >= 0 && asmtest_slice_contains(fwd, (uint32_t)argdef),
          "callarg: arg-defining move is in forward slice(seed) [ORACLE]");

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    asmtest_valtrace_free(v);
}

/* Call-arg negative control: the SAME fixture unseeded => the arg register stays clean => zero
 * hits (the sink is seed-gated, not structural). */
static void test_callarg_negative(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "callarg-negative: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "callarg-negative: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 0};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_callarg, sizeof taint_callarg, args, 2, 0, /*seed_base*/ 0,
        /*seed_len*/ 0, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK, "callarg-negative: routine captured (unseeded)");
    CHECK(report.hits_total == 0 && report.hits_len == 0 && !report.truncated,
          "callarg-negative: zero hits (unseeded => clean arg never reaches "
          "sink)");

    free(buf);
    asmtest_valtrace_free(v);
}

/* Mem-len sink scenario (kind = 0): a tainted value reaches a memory-copy LENGTH (the rep
 * movsb count register rcx) => one at_taint_hit_t with the right off/tag/kind. rcx is a machine
 * source of rep movs, so the sink step is in forward(seed) and the app-side def-use BFS
 * resolves seed_off + depth. */
static void test_memlen(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "memlen: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    uint64_t *buf2 = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "memlen: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 4; /* a SMALL, tainted byte count for the rep movsb */
    *buf2 = 0;
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_memlen, sizeof taint_memlen, args, 2, 0, (uint64_t)(uintptr_t)buf,
        sizeof(uint64_t), AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK,
          "memlen: routine captured in-band under the taint client");
    CHECK(report.hits_total == 1 && report.hits_len == 1 && !report.truncated,
          "memlen: exactly one taint hit recorded");
    if (report.hits_len == 1) {
        at_taint_hit_t *h = &report.hits[0];
        CHECK(h->off == MEMLEN_OFF,
              "memlen: hit offset is the rep movsb (0x03)");
        CHECK(h->tag != AT_TAG_CLEAN, "memlen: hit tag is tainted");
        CHECK(h->kind == 0, "memlen: hit kind = 0 (mem-copy length)");
        CHECK(h->ea == 0, "memlen: hit ea = 0 (register sink)");
    }

    /* ORACLE: rcx (the count) is a machine source of rep movsb, so the sink step is in
     * forward(seed); the app-side def-use BFS resolves seed_off + depth. */
    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    int sink_step = step_at_off(v, MEMLEN_OFF);
    CHECK(fwd && sink_step >= 0 &&
              asmtest_slice_contains(fwd, (uint32_t)sink_step),
          "memlen: rep movsb is in forward slice(seed) [ORACLE]");

    fill_seed_and_depth(&report, g, v, 0);
    if (report.hits_len == 1) {
        CHECK(report.hits[0].seed_off == 0x00,
              "memlen: app-side seed_off resolves to the seed load (0x00)");
        CHECK(report.hits[0].depth == MEMLEN_DEPTH,
              "memlen: app-side depth = 1 def-use edge seed->sink");
    }

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* Mem-len negative control: the SAME fixture unseeded => the count register stays clean => zero
 * hits (the sink is seed-gated, not structural). */
static void test_memlen_negative(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    at_taint_hit_t hits[8];
    at_taint_report_t report;
    memset(&report, 0, sizeof report);
    report.hits = hits;
    report.hits_cap = 8;
    if (v == NULL) {
        CHECK(0, "memlen-negative: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    uint64_t *buf2 = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "memlen-negative: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 4;
    *buf2 = 0;
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_memlen, sizeof taint_memlen, args, 2, 0, /*seed_base*/ 0,
        /*seed_len*/ 0, AT_TAG_TAINTED, &result, v, step_taint,
        sizeof step_taint / sizeof step_taint[0], &report);

    CHECK(rc == DF_DR_OK, "memlen-negative: routine captured (unseeded)");
    CHECK(report.hits_total == 0 && report.hits_len == 0 && !report.truncated,
          "memlen-negative: zero hits (unseeded => clean count never reaches "
          "sink)");

    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* Create-on-touch scenario: taint flows THROUGH a store to a fresh, never-touched heap
 * buffer and back. Passes only if the inline store-tag slowpath creates the leaf on
 * first touch (no pre-touch); the taint set must equal forward(seed) = {0,1,2,3}. */
static void test_heapstore(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "heapstore: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    uint64_t *buf2 = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL || buf2 == NULL) {
        CHECK(0, "heapstore: buffer alloc");
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    *buf2 = 0;
    long args[2] = {(long)(uintptr_t)buf, (long)(uintptr_t)buf2};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_heapstore, sizeof taint_heapstore, args, 2, 0,
        (uint64_t)(uintptr_t)buf, sizeof(uint64_t), AT_TAG_TAINTED, &result, v,
        step_taint, sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK,
          "heapstore: routine captured in-band under the taint client");
    CHECK(result == 7,
          "heapstore: routine returned 7 (round-tripped *buf via buf2)");
    CHECK(v->steps_len == 5, "heapstore: five in-region steps captured");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    int mism = 0;
    for (size_t i = 0; i < v->steps_len; i++) {
        int tainted = step_taint[i] != 0;
        int inslice = fwd ? asmtest_slice_contains(fwd, (uint32_t)i) : 0;
        if (tainted != inslice)
            mism++;
    }
    CHECK(mism == 0, "heapstore: taint flows through the fresh-heap store "
                     "[CREATE-ON-TOUCH]");
    CHECK(step_taint[2] != 0, "heapstore: reload from buf2 (step2) tainted "
                              "(slowpath created the leaf)");

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    free(buf2);
    asmtest_valtrace_free(v);
}

/* Per-byte union scenario: seed ONLY the HIGH 4 bytes of the 8-byte buffer, then load
 * all 8. A low-byte-only tag read (shadow[buf+0], clean) would miss it and taint nothing;
 * the byte-granular per-byte union reads shadow[buf+0..7], catches the tainted high bytes,
 * and the taint set must equal forward(seed) = {0,1,2,3,4}. Reuses taint_chain. */
static void test_highbyte(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    if (v == NULL) {
        CHECK(0, "highbyte: valtrace_new");
        return;
    }
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    if (buf == NULL) {
        CHECK(0, "highbyte: buffer alloc");
        asmtest_valtrace_free(v);
        return;
    }
    *buf = 7;
    long args[2] = {(long)(uintptr_t)buf, 5};
    long result = 0;
    int rc = asmtest_dataflow_dr_taint_run(
        taint_chain, sizeof taint_chain, args, 2, 0,
        (uint64_t)(uintptr_t)buf + 4, /*seed_len*/ 4, AT_TAG_TAINTED, &result,
        v, step_taint, sizeof step_taint / sizeof step_taint[0], NULL);

    CHECK(rc == DF_DR_OK,
          "highbyte: routine captured in-band under the taint client");
    CHECK(v->steps_len == 6, "highbyte: six in-region steps captured");
    CHECK(step_taint[0] != 0, "highbyte: step0 load catches the high-byte-only "
                              "seed [PER-BYTE UNION]");

    asmtest_defuse_t *g = asmtest_defuse_build(v);
    at_val_rec_t seed = {0};
    seed.step = 0;
    asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
    int mism = 0;
    for (size_t i = 0; i < v->steps_len; i++) {
        int tainted = step_taint[i] != 0;
        int inslice = fwd ? asmtest_slice_contains(fwd, (uint32_t)i) : 0;
        if (tainted != inslice)
            mism++;
    }
    CHECK(mism == 0,
          "highbyte: taint set == forward slice (high bytes propagate)");

    asmtest_slice_free(fwd);
    asmtest_defuse_free(g);
    free(buf);
    asmtest_valtrace_free(v);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a hard kill */
    if (!asmtest_dataflow_dr_available()) {
        printf("# SKIP dr-taint: DynamoRIO / taint client unavailable\n1..0\n");
        return 0;
    }
    const char *mode = (argc > 1) ? argv[1] : "";
    if (strcmp(mode, "negative") == 0)
        test_negative();
    else if (strcmp(mode, "sink") == 0)
        test_sink();
    else if (strcmp(mode, "sink-negative") == 0)
        test_sink_negative();
    else if (strcmp(mode, "heapstore") == 0)
        test_heapstore();
    else if (strcmp(mode, "highbyte") == 0)
        test_highbyte();
    else if (strcmp(mode, "callarg") == 0)
        test_callarg();
    else if (strcmp(mode, "callarg-negative") == 0)
        test_callarg_negative();
    else if (strcmp(mode, "memlen") == 0)
        test_memlen();
    else if (strcmp(mode, "memlen-negative") == 0)
        test_memlen_negative();
    else
        test_seeded();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
