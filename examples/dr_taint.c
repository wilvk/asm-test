/*
 * dr_taint.c — DynamoRIO taint tier (Increment 4, first slice): the in-band inline
 * dst_tag = union(src_tags) producer end-to-end. Runs a self-contained x86-64 routine
 * NATIVELY, in-band, whole-process under DynamoRIO with the taint client
 * (src/dataflow_dr_client_inlined.c built -DASMTEST_TAINT), SEEDS a known buffer, and
 * asserts the client's per-step taint witness (dv->step_taint) equals the emulator-
 * oracle FORWARD SLICE from the seed step — i.e. inline taint propagation reproduces
 * def-use forward reachability. Plus a negative control (unseeded => empty taint set).
 *
 * This mirrors dr_valtrace.c's structure + oracle discipline; the ADDITIVE value
 * capture still runs (asserted: result + step count), so this also proves the taint
 * client fills at_drval_t identically. Named dr_taint.c (not test_*.c) so the root
 * Makefile's SUITES wildcard does not sweep this standalone DynamoRIO harness into the
 * forking runner; it is wired + run explicitly by mk/native-trace.mk's
 * dr-taint-native-test lane (twice: seeded, then `negative`).
 *
 * DynamoRIO permits ONE in-process lifecycle per process, so each invocation drives ONE
 * scenario: default = seeded (positive oracle diff), argv[1]=="negative" = the unseeded
 * control. Self-skips cleanly (exit 0) when DynamoRIO / the client is unavailable.
 *
 * Fixture (leaf, x86-64 SysV — rdi=buf, rsi=b) — df_chain with step0 reading a SEEDED
 * memory buffer instead of a register arg, so taint originates in the shadow:
 *   taint_chain(buf,b): mov rax,[rdi] / mov [rsp-8],rax / mov rcx,[rsp-8] /
 *                       lea rdx,[rcx+rsi] / mov rax,rdx / ret
 *   Seed M[buf] => forward(step0) = {0,1,2,3,4} (excludes ret); the store/reload at
 *   steps 1->2 exercise the integer-memory (stack) shadow, the moves + lea the reg tags.
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
                                  at_tag_t *step_taint, size_t step_taint_cap);

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

static int slices_equal(const asmtest_slice_t *a, const asmtest_slice_t *b) {
    if (a == NULL || b == NULL || a->n != b->n)
        return 0;
    for (size_t i = 0; i < a->n; i++)
        if (a->steps[i] != b->steps[i])
            return 0;
    return 1;
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
        sizeof step_taint / sizeof step_taint[0]);

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
        sizeof step_taint / sizeof step_taint[0]);

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

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0); /* progress survives a hard kill */
    if (!asmtest_dataflow_dr_available()) {
        printf("# SKIP dr-taint: DynamoRIO / taint client unavailable\n1..0\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "negative") == 0)
        test_negative();
    else
        test_seeded();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
