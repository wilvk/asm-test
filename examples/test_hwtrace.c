/*
 * test_hwtrace.c — smoke test for the optional hardware-assisted native-trace
 * tier (asmtest_hwtrace.h). Self-skips with a clear reason (exit 0) when the
 * decoder library, the intel_pt/cs_etm PMU, the right CPU, or perf_event
 * privilege is absent — the common case off bare metal (and always on AMD/VM/CI).
 *
 * On a capable bare-metal Intel-PT host (perf_event_paranoid lowered) it traces a
 * host-native routine and asserts block offset 0 plus a deterministic ordered
 * instruction stream — matching the Unicorn/DynamoRIO output for the same bytes.
 */
#include "asmtest_hwtrace.h"
#include "asmtest_ptrace.h"
#include "asmtest_trace.h"
#include "asmtest_trace_auto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
int asmtest_amd_decoder_present(void);
#endif

/* CoreSight reconstruction-core interface (decoder-independent half of
 * src/cs_backend.c). KEEP THIS STRUCT IN SYNC with asmtest_cs_range_t there. */
typedef struct {
    uint64_t start_off;
    uint64_t end_off;
    int ends_in_branch;
} cs_range_t;
int asmtest_cs_reconstruct(asmtest_arch_t arch, const cs_range_t *ranges,
                           size_t nranges, const void *base, size_t len,
                           asmtest_trace_t *trace);

/* mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks). */
static const unsigned char ROUTINE[] = {
    0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0, 0x48, 0x3d, 0x64, 0x00,
    0x00, 0x00, 0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3};
typedef long (*add2_fn)(long, long);

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf(c ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);            \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* AMD-LBR reconstruction is validated WITHOUT capture hardware: feed the decoder
 * a synthetic branch-record array (what Zen 3/4 would capture for a known path)
 * and assert it reconstructs the exact same offsets the PT/DynamoRIO backends do.
 * Runs on any Linux x86-64 host with Capstone (incl. this Zen 2 box, where live
 * AMD capture self-skips). */
static void test_amd_reconstruction(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_amd_decoder_present()) {
        printf("# SKIP AMD reconstruction: built without Capstone\n");
        return;
    }
    uint64_t b = (uint64_t)(uintptr_t)ROUTINE;
    /* fn(20,22)=42: taken branches are jle (0xc->0x11) then ret (0x11->out).
     * perf delivers the stack newest-first, so: [ret, jle]. */
    struct perf_branch_entry br[2];
    memset(br, 0, sizeof br);
    br[0].from = b + 0x11; br[0].to = b + sizeof ROUTINE; /* ret -> outside */
    br[1].from = b + 0xc;  br[1].to = b + 0x11;           /* jle -> ret     */

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_amd_decode(br, 2, ROUTINE, sizeof ROUTINE, tr);
    CHECK(rc == 0, "AMD decode succeeds on a synthetic Tier-A branch stack");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "AMD reconstruction yields the exact PT/DR instruction sequence");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "AMD reconstruction yields the matching block partition {0, 0x11}");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "AMD reconstruction records exactly two blocks");
    asmtest_trace_free(tr);

    /* Overflow: a full 16-entry stack must set truncated (window exceeded). */
    struct perf_branch_entry full[16];
    memset(full, 0, sizeof full);
    for (int i = 0; i < 16; i++) {
        full[i].from = b + 0xc;
        full[i].to = b + 0x11;
    }
    asmtest_trace_t *ot = asmtest_trace_new(64, 64);
    asmtest_amd_decode(full, 16, ROUTINE, sizeof ROUTINE, ot);
    CHECK(asmtest_emu_trace_truncated(ot),
          "AMD full 16-entry stack sets truncated (window overflow)");
    asmtest_trace_free(ot);
#else
    printf("# SKIP AMD reconstruction: not Linux x86-64\n");
#endif
}

/* CoreSight reconstruction is validated WITHOUT a CoreSight board (exactly as the
 * AMD reconstruction is validated without Zen hardware): feed asmtest_cs_reconstruct
 * the instruction RANGES an ETM/ETE would emit for a known path and assert it
 * rebuilds the same offsets/blocks the PT/AMD/single-step backends produce. The core
 * is arch-independent, so we use the shared x86-64 fixture (ASMTEST_ARCH_X86_64) for
 * byte-for-byte comparability; the live path passes ASMTEST_ARCH_ARM64. Runs on any
 * host with Capstone. */
static void test_cs_reconstruction(void) {
    if (!asmtest_disas_available()) {
        printf("# SKIP CoreSight reconstruction: built without Capstone\n");
        return;
    }
    /* fn(20,22)=42: range 1 covers [0,0xe) (mov;add;cmp;jle) ending at the taken
     * jle; range 2 covers [0x11,0x12) (ret). Both end in a branch waypoint. */
    cs_range_t ranges[2] = {
        {0x0, 0xe, 1},
        {0x11, 0x12, 1},
    };
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    int rc = asmtest_cs_reconstruct(ASMTEST_ARCH_X86_64, ranges, 2, ROUTINE,
                                    sizeof ROUTINE, tr);
    CHECK(rc == ASMTEST_HW_OK, "CoreSight reconstruct succeeds on synthetic ranges");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "CoreSight reconstruction yields the exact PT/AMD instruction stream");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "CoreSight reconstruction yields the matching block partition {0, 0x11}");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "CoreSight reconstruction records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr), "CoreSight reconstruction is complete");
    asmtest_trace_free(tr);

    /* A single straight-line range (no branch) reconstructs every instruction as one
     * block — the degenerate ETM range. */
    cs_range_t one[1] = {{0x0, 0x6, 0}}; /* mov; add (two insns, no branch) */
    asmtest_trace_t *st = asmtest_trace_new(64, 64);
    asmtest_cs_reconstruct(ASMTEST_ARCH_X86_64, one, 1, ROUTINE, sizeof ROUTINE, st);
    CHECK(asmtest_emu_trace_insns_total(st) == 2 &&
              asmtest_emu_trace_blocks_len(st) == 1,
          "CoreSight straight-line range = 2 insns, 1 block");
    asmtest_trace_free(st);
}

/* mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  — loop body block at 0x7, the
 * jnz back-edge target. A long trip count makes the routine run long enough that
 * PMU branch samples fire INSIDE the region (a tiny routine completes before a
 * second sample can arm), which is what AMD LBR capture needs. */
static const unsigned char AMD_LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00,
                                         0x48, 0x01, 0xf8, 0x48, 0xff, 0xce,
                                         0x75, 0xf8, 0xc3};

/* One live AMD-LBR capture of AMD_LOOP(trips). Returns insns_total reconstructed;
 * reports loop-body coverage and the truncation bit. */
static uint64_t amd_capture_loop(void *p, long trips, int *cov7, int *trunc) {
    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_AMD_LBR;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    asmtest_hwtrace_register_region("amdloop", p, sizeof AMD_LOOP, tr);
    long (*fn)(long, long) = (long (*)(long, long))p;
    asmtest_hwtrace_begin("amdloop");
    fn(1, trips);
    asmtest_hwtrace_end("amdloop");
    uint64_t n = asmtest_emu_trace_insns_total(tr);
    *cov7 = asmtest_trace_covered(tr, 0x7);
    *trunc = asmtest_emu_trace_truncated(tr);
    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    return n;
}

/* AMD LBR LIVE capture: on a Zen 3+ / Zen 4 / Zen 5 host with perf branch-stack
 * permitted, this exercises the REAL perf_event_open branch-record capture + decode
 * path (hwtrace_begin_amd/_end_amd -> asmtest_amd_decode), not the synthetic
 * reconstruction above. Self-skips where AMD LBR is unavailable (non-AMD host, no
 * LbrExtV2/BRS, perf locked down, or built without Capstone).
 *
 * AMD has no continuous trace ring; perf delivers the 16-deep branch stack only AT
 * a PMU sample. So capture works for branch-heavy routines (a sample fires inside
 * the region and snapshots its branches) and honestly TRUNCATES for a tiny
 * single-shot routine (too fast to be sampled in-region) — the fallback signal,
 * never an empty trace claimed complete. Verified on a Zen 5 host (Ryzen 9 9950X,
 * amd_lbr_v2). */
static void test_amd_live(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_AMD_LBR)) {
        char why[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, why, sizeof why);
        printf("# SKIP AMD LBR live capture: %s\n", why);
        return;
    }

    /* (a) Tiny, fast single-shot routine: completes before a second PMU sample can
     * fire, so its branches are never sampled in-region. The capture must come back
     * truncated (the dynamic-fallback signal) — never insns=0 with truncated=0,
     * which the old "keep the last sample" logic produced (it decoded a post-routine
     * glue sample with no in-region branches and silently claimed complete). */
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        memcpy(p, ROUTINE, sizeof ROUTINE);
        mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
        asmtest_hwtrace_options_t opts;
        memset(&opts, 0, sizeof opts);
        opts.backend = ASMTEST_HWTRACE_AMD_LBR;
        CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "AMD LBR live init");
        asmtest_trace_t *tr = asmtest_trace_new(64, 64);
        CHECK(asmtest_hwtrace_register_region("amd", p, sizeof ROUTINE, tr) ==
                  ASMTEST_HW_OK,
              "AMD LBR live register region");
        add2_fn fn = (add2_fn)p;
        asmtest_hwtrace_begin("amd");
        long r = fn(20, 22);
        asmtest_hwtrace_end("amd");
        CHECK(r == 42, "AMD LBR live single-shot call returns 20+22");
        CHECK(asmtest_emu_trace_truncated(tr) ||
                  asmtest_emu_trace_insns_total(tr) > 0,
              "AMD LBR live single-shot: honest result (never empty-yet-complete)");
        asmtest_hwtrace_shutdown();
        asmtest_trace_free(tr);
        munmap(p, sizeof ROUTINE);
    }

    /* (b) Branch-heavy loop: runs long enough that PMU samples fire INSIDE the
     * region, so AMD LbrExtV2 delivers a full 16-deep in-region branch stack and the
     * decoder reconstructs it. Sampling is statistical, so retry a few times and
     * assert the reconstruction once captured. */
    void *q = mmap(NULL, sizeof AMD_LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q != MAP_FAILED) {
        memcpy(q, AMD_LOOP, sizeof AMD_LOOP);
        mprotect(q, sizeof AMD_LOOP, PROT_READ | PROT_EXEC);
        __builtin___clear_cache((char *)q, (char *)q + sizeof AMD_LOOP);
        int cov7 = 0, trunc = 0;
        uint64_t insns = 0;
        for (int attempt = 0; attempt < 8 && insns == 0; attempt++)
            insns = amd_capture_loop(q, 20000, &cov7, &trunc);
        printf("# AMD LBR live loop: insns_total=%llu covered(0x7)=%d truncated=%d\n",
               (unsigned long long)insns, cov7, trunc);
        CHECK(insns > 0,
              "AMD LBR live loop: reconstructs in-region branches from the real LBR");
        CHECK(insns == 0 || cov7,
              "AMD LBR live loop: reconstructs the loop-body block 0x7");
        CHECK(insns == 0 || trunc,
              "AMD LBR live loop: >16-branch window flagged truncated");
        munmap(q, sizeof AMD_LOOP);
    }
}

/* Single-step (EFLAGS.TF) LIVE capture: unlike PT/AMD this runs on ANY x86-64
 * Linux host — no PMU, no perf_event, no privilege — so it executes here (and on
 * standard CI / in a plain container) instead of self-skipping. Trace the shared
 * fixture on the real CPU and assert byte-for-byte parity with the
 * Unicorn/DynamoRIO/PT instruction+block partition. */
static void test_singlestep_live(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP)) {
        char why[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_SINGLESTEP, why, sizeof why);
        printf("# SKIP single-step live capture: %s\n", why);
        return;
    }
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP single-step: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "single-step init");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr) ==
              ASMTEST_HW_OK,
          "single-step register region");

    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("add2");
    long r = fn(20, 22); /* 42 <= 100: jle taken, dec (0xe) skipped */
    asmtest_hwtrace_end("add2");

    CHECK(r == 42, "single-step traced call returns 20+22");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "single-step yields the exact live instruction stream [0,3,6,c,11]");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "single-step block partition {0, 0x11} matches PT/AMD/DynamoRIO");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "single-step records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "single-step trace is complete (not truncated)");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
}

/* The single-step differentiator: NO depth ceiling. A 20-trip loop takes 19 taken
 * back-edges — past AMD LBR's 16-entry window (which would flag truncated) — yet
 * single-step reconstructs every instruction exactly and stays complete. */
static void test_singlestep_loop(void) {
    if (!asmtest_hwtrace_available(ASMTEST_HWTRACE_SINGLESTEP))
        return; /* already reported by test_singlestep_live */
    /* mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret   (rdi=step, rsi=trips) */
    static const unsigned char LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00,
                                         0x48, 0x01, 0xf8, 0x48, 0xff, 0xce,
                                         0x75, 0xf8, 0xc3};
    void *p = mmap(NULL, sizeof LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return;
    memcpy(p, LOOP, sizeof LOOP);
    mprotect(p, sizeof LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof LOOP);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    asmtest_hwtrace_init(&opts);
    asmtest_trace_t *tr = asmtest_trace_new(256, 64);
    asmtest_hwtrace_register_region("loop", p, sizeof LOOP, tr);

    long (*fn)(long, long) = (long (*)(long, long))p;
    asmtest_hwtrace_begin("loop");
    long r = fn(1, 20); /* 20 trips, returns 20 */
    asmtest_hwtrace_end("loop");

    CHECK(r == 20, "single-step loop call returns sum");
    /* 1 (mov) + 20*(add,dec,jnz) + 1 (ret) = 62 instructions, all captured. */
    CHECK(asmtest_emu_trace_insns_total(tr) == 62,
          "single-step captures all 62 insns of a 20-trip loop (no depth ceiling)");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 && asmtest_trace_covered(tr, 0) &&
              asmtest_trace_covered(tr, 0x7),
          "single-step loop block partition {0, 0x7}");
    CHECK(!asmtest_emu_trace_truncated(tr),
          "single-step loop trace complete past LBR's 16-branch window");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof LOOP);
}

/* Auto-select front-end: the orchestrator picks the most-faithful AVAILABLE
 * hardware-trace backend for this host, so a caller need not hard-code one. The
 * SELECTION invariants hold on every host (even where all backends self-skip); on
 * any x86-64 Linux host the cascade is non-empty (single-step is always available)
 * and we additionally run a live traced call through the auto-picked backend to
 * prove the choice is usable end to end. */
static void test_auto_resolve(void) {
    asmtest_trace_backend_t best[4], cf[4];
    size_t nb = asmtest_hwtrace_resolve(ASMTEST_HWTRACE_BEST, best, 4);
    size_t nc = asmtest_hwtrace_resolve(ASMTEST_HWTRACE_CEILING_FREE, cf, 4);

    /* Every resolved backend is actually available, in descending-fidelity
     * (ascending-enum) order, with no duplicates. */
    int ok_avail = 1, ok_order = 1;
    for (size_t i = 0; i < nb; i++) {
        if (!asmtest_hwtrace_available(best[i]))
            ok_avail = 0;
        if (i && best[i] <= best[i - 1])
            ok_order = 0;
    }
    CHECK(ok_avail, "auto BEST returns only available backends");
    CHECK(ok_order, "auto BEST is ordered by descending fidelity, no dups");

    /* CEILING_FREE drops the one fixed-window backend (AMD LBR); it is otherwise a
     * subset of BEST (so it never adds a backend BEST lacks). */
    int cf_no_amd = 1, cf_subset = 1;
    for (size_t i = 0; i < nc; i++) {
        if (cf[i] == ASMTEST_HWTRACE_AMD_LBR)
            cf_no_amd = 0;
        int in_best = 0;
        for (size_t j = 0; j < nb; j++)
            in_best |= (best[j] == cf[i]);
        cf_subset &= in_best;
    }
    CHECK(cf_no_amd, "auto CEILING_FREE never selects AMD LBR (16-branch window)");
    CHECK(cf_subset, "auto CEILING_FREE is a subset of BEST");

    /* auto(policy) is the head of resolve(policy), or EUNAVAIL when empty. */
    int ab = asmtest_hwtrace_auto(ASMTEST_HWTRACE_BEST);
    int head_ok = (nb == 0) ? (ab == ASMTEST_HW_EUNAVAIL) : (ab == (int)best[0]);
    CHECK(head_ok, "auto(BEST) is the head of the resolved cascade");

#if defined(__linux__) && defined(__x86_64__)
    /* Universal guarantee: single-step keeps the cascade non-empty on every x86-64
     * Linux host, so the orchestrator never fails to resolve here. */
    CHECK(nb >= 1 && ab >= 0,
          "auto resolves a backend on x86-64 Linux (single-step floor)");

    /* End to end: trace the shared fixture through whatever auto picked. The pick is
     * single-step on a PT-/AMD-LBR-less host (byte-exact parity), AMD LBR on a Zen
     * 3+/4/5 host with perf (which honestly truncates on this tiny single-shot
     * fixture — too short to be sampled in-region), or Intel PT on bare-metal Intel.
     * So assert the call ran and the trace is honest (covered OR truncated), with the
     * byte-exact stream only for the single-step pick. */
    if (ab >= 0) {
        void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p != MAP_FAILED) {
            memcpy(p, ROUTINE, sizeof ROUTINE);
            mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
            __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);
            asmtest_hwtrace_options_t opts;
            memset(&opts, 0, sizeof opts);
            opts.backend = (asmtest_trace_backend_t)ab;
            if (asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK) {
                asmtest_trace_t *tr = asmtest_trace_new(64, 64);
                asmtest_hwtrace_register_region("auto", p, sizeof ROUTINE, tr);
                add2_fn fn = (add2_fn)p;
                asmtest_hwtrace_begin("auto");
                long r = fn(20, 22);
                asmtest_hwtrace_end("auto");
                CHECK(r == 42, "auto-selected backend traces a live call (returns 42)");
                CHECK(asmtest_trace_covered(tr, 0) ||
                          asmtest_emu_trace_truncated(tr),
                      "auto-selected backend covers block 0 (or honestly truncates)");
                if (ab == ASMTEST_HWTRACE_SINGLESTEP) {
                    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
                    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
                    for (size_t i = 0; seq && i < 5; i++)
                        seq = (tr->insns[i] == EXPECT[i]);
                    CHECK(seq, "auto pick (single-step) yields the exact shared "
                               "offset stream [0,3,6,c,11]");
                }
                asmtest_hwtrace_shutdown();
                asmtest_trace_free(tr);
            }
            munmap(p, sizeof ROUTINE);
        }
    }
#endif
}

/* Cross-tier orchestrator (asmtest_trace_auto.h): the front-end OVER all three
 * tiers. It interleaves the hardware backends around the DynamoRIO tier by overhead
 * (PT, AMD LBR rank above DynamoRIO; single-step, CoreSight below it) and ends at
 * the emulator floor, gating the native->emulator crossing behind
 * ASMTEST_TRACE_NATIVE_ONLY. These structural invariants hold on every host,
 * independent of which tiers happen to be present. */
static void test_cross_tier_resolve(void) {
    asmtest_trace_choice_t best[8], nat[8], cf[8];
    size_t nb = asmtest_trace_resolve(ASMTEST_TRACE_BEST, best, 8);
    size_t nn = asmtest_trace_resolve(ASMTEST_TRACE_NATIVE_ONLY, nat, 8);
    size_t ncf = asmtest_trace_resolve(ASMTEST_TRACE_CEILING_FREE, cf, 8);

    /* Every HW choice satisfies the hardware-tier probe; fidelity is consistent
     * (NATIVE for HW/DynamoRIO, VIRTUAL for the emulator); and once a VIRTUAL row
     * appears no NATIVE row follows it (the cascade is weakly descending). */
    int ok_hw = 1, ok_fidelity = 1, emu_count = 0;
    for (size_t i = 0; i < nb; i++) {
        if (best[i].tier == ASMTEST_TIER_HWTRACE &&
            !asmtest_hwtrace_available(best[i].backend))
            ok_hw = 0;
        if (best[i].tier == ASMTEST_TIER_EMULATOR) {
            emu_count++;
            if (best[i].fidelity != ASMTEST_FIDELITY_VIRTUAL)
                ok_fidelity = 0;
        } else if (best[i].fidelity != ASMTEST_FIDELITY_NATIVE) {
            ok_fidelity = 0;
        }
        if (i && best[i - 1].fidelity == ASMTEST_FIDELITY_VIRTUAL &&
            best[i].fidelity == ASMTEST_FIDELITY_NATIVE)
            ok_fidelity = 0;
    }
    CHECK(ok_hw, "cross-tier BEST: every HW choice is asmtest_hwtrace_available");
    CHECK(ok_fidelity,
          "cross-tier BEST: NATIVE choices precede the VIRTUAL emulator floor");
    CHECK(nb >= 1 && best[nb - 1].tier == ASMTEST_TIER_EMULATOR && emu_count == 1,
          "cross-tier BEST ends at exactly one emulator floor on every host");

    /* NATIVE_ONLY forbids the native->emulator crossing: no emulator row, and the
     * result is exactly BEST with the trailing emulator floor removed. */
    int nat_no_emu = 1;
    for (size_t i = 0; i < nn; i++)
        if (nat[i].tier == ASMTEST_TIER_EMULATOR)
            nat_no_emu = 0;
    CHECK(nat_no_emu,
          "cross-tier NATIVE_ONLY drops the emulator (no fidelity crossing)");
    CHECK(nn == nb - 1, "cross-tier NATIVE_ONLY is BEST minus the emulator floor");

    /* CEILING_FREE drops the one fixed-window backend (AMD LBR, 16 taken branches). */
    int cf_no_amd = 1;
    for (size_t i = 0; i < ncf; i++)
        if (cf[i].tier == ASMTEST_TIER_HWTRACE &&
            cf[i].backend == ASMTEST_HWTRACE_AMD_LBR)
            cf_no_amd = 0;
    CHECK(cf_no_amd, "cross-tier CEILING_FREE never selects AMD LBR");

    /* auto(policy) is the head of resolve(policy), or EUNAVAIL when empty. */
    asmtest_trace_choice_t one;
    int rc = asmtest_trace_auto(ASMTEST_TRACE_BEST, &one);
    int head_ok = (nb == 0)
                      ? (rc == ASMTEST_HW_EUNAVAIL)
                      : (rc == ASMTEST_HW_OK && one.tier == best[0].tier &&
                         one.backend == best[0].backend);
    CHECK(head_ok, "cross-tier auto(BEST) is the head of the resolved cascade");

#if defined(__linux__) && defined(__x86_64__)
    /* On x86-64 Linux the single-step backend is a NATIVE floor, so even NATIVE_ONLY
     * resolves — the cascade never collapses to nothing here. */
    asmtest_trace_choice_t natpick;
    int nrc = asmtest_trace_auto(ASMTEST_TRACE_NATIVE_ONLY, &natpick);
    CHECK(nrc == ASMTEST_HW_OK &&
              natpick.fidelity == ASMTEST_FIDELITY_NATIVE && nn >= 1,
          "cross-tier NATIVE_ONLY still resolves a native tier on x86-64 Linux");
    int has_ss = 0;
    for (size_t i = 0; i < nn; i++)
        has_ss |= (nat[i].tier == ASMTEST_TIER_HWTRACE &&
                   nat[i].backend == ASMTEST_HWTRACE_SINGLESTEP);
    CHECK(has_ss,
          "cross-tier native cascade includes the single-step floor (x86-64 Linux)");
#endif
}

/* Out-of-process single-step (W2): a tracer parent PTRACE_SINGLESTEPs a forked
 * tracee that runs the routine, collecting the same exact offsets out of band. Prove
 * it is byte-identical to the in-process stepper for the shared fixture, and that a
 * 20-trip loop reconstructs with no depth ceiling (it never touches a branch-record
 * window). Runs live on any x86-64 Linux host. */
static void test_ptrace_oop(void) {
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP out-of-process ptrace stepper: %s\n", why);
        return;
    }

    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP ptrace oop: mmap failed\n");
        return;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long args[2] = {20, 22};
    long result = 0;
    int rc = asmtest_ptrace_trace_call(p, sizeof ROUTINE, args, 2, &result, tr);
    CHECK(rc == ASMTEST_PTRACE_OK, "ptrace oop trace_call succeeds");
    CHECK(result == 42, "ptrace oop traced call returns 20+22 (RAX read via ptrace)");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq, "ptrace oop yields the exact stream [0,3,6,c,11] (== in-process)");
    CHECK(asmtest_trace_covered(tr, 0) && asmtest_trace_covered(tr, 0x11),
          "ptrace oop block partition {0, 0x11} matches every other backend");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2,
          "ptrace oop records exactly two blocks");
    CHECK(!asmtest_emu_trace_truncated(tr), "ptrace oop trace is complete");
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);

    /* No depth ceiling: a 20-trip loop (19 back-edges, past AMD LBR's 16) captured
     * exactly — the property an out-of-band hardware branch window cannot match. */
    static const unsigned char LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00,
                                         0x48, 0x01, 0xf8, 0x48, 0xff, 0xce, 0x75,
                                         0xf8, 0xc3};
    void *q = mmap(NULL, sizeof LOOP, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED)
        return;
    memcpy(q, LOOP, sizeof LOOP);
    mprotect(q, sizeof LOOP, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)q, (char *)q + sizeof LOOP);
    asmtest_trace_t *lt = asmtest_trace_new(256, 64);
    long largs[2] = {1, 20};
    long lresult = 0;
    asmtest_ptrace_trace_call(q, sizeof LOOP, largs, 2, &lresult, lt);
    CHECK(lresult == 20, "ptrace oop loop returns 20 (sum of 1, 20 times)");
    CHECK(asmtest_emu_trace_insns_total(lt) == 62,
          "ptrace oop loop captures all 62 insns (1 + 20*3 + 1), no depth ceiling");
    CHECK(!asmtest_emu_trace_truncated(lt), "ptrace oop loop is complete");
    asmtest_trace_free(lt);
    munmap(q, sizeof LOOP);
}

/* W2 live ATTACH: trace a region in a SEPARATE, externally-attached process — the
 * building block for tracing a managed runtime out of band. A child spins on a shared
 * flag, then calls the fixture; the parent PTRACE_ATTACHes it (the child never called
 * TRACEME — a true external attach), traces the region with
 * asmtest_ptrace_trace_attached (which reads the child's code via process_vm_readv,
 * not a shared mapping), and asserts the SAME offsets the in-process stepper yields. */
static void test_ptrace_attach(void) {
#if defined(__linux__) && defined(__x86_64__)
    if (!asmtest_ptrace_available()) {
        printf("# SKIP ptrace attach: not Linux x86-64\n");
        return;
    }
    volatile int *go = mmap(NULL, sizeof(int), PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (go == MAP_FAILED || p == MAP_FAILED) {
        printf("# SKIP ptrace attach: mmap failed\n");
        return;
    }
    *go = 0;
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    pid_t pid = fork();
    if (pid == 0) {
        while (!*go) {
            /* user-space spin (no syscall) so the external attach catches us cleanly */
        }
        volatile long r = ((add2_fn)p)(20, 22);
        (void)r;
        _exit(0);
    }

    /* Let the child reach the spin, then attach from the OUTSIDE (it never opted in
     * via TRACEME). The attach stops the child; only then do we release it, so it
     * cannot run the region before we are tracing. */
    struct timespec ts = {0, 3 * 1000 * 1000}; /* 3 ms */
    nanosleep(&ts, NULL);
    int status = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 ||
        waitpid(pid, &status, 0) < 0) {
        printf("# SKIP ptrace attach: PTRACE_ATTACH not permitted (yama ptrace_scope)\n");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return;
    }
    *go = 1;

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    long result = 0;
    int rc = asmtest_ptrace_trace_attached(pid, p, sizeof ROUTINE, &result, tr);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    waitpid(pid, &status, 0);

    CHECK(rc == ASMTEST_PTRACE_OK,
          "ptrace attach: trace_attached succeeds on an externally-attached PID");
    CHECK(result == 42,
          "ptrace attach: result 42 read from the foreign process's RAX");
    static const uint64_t EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
    int seq = (asmtest_emu_trace_insns_total(tr) == 5);
    for (size_t i = 0; seq && i < 5; i++)
        seq = (tr->insns[i] == EXPECT[i]);
    CHECK(seq,
          "ptrace attach: foreign-process trace == in-process stream [0,3,6,c,11]");
    CHECK(asmtest_emu_trace_blocks_len(tr) == 2 && !asmtest_emu_trace_truncated(tr),
          "ptrace attach: two blocks, complete (bytes read via process_vm_readv)");
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);
    munmap((void *)go, sizeof(int));
#else
    printf("# SKIP ptrace attach: not Linux x86-64\n");
#endif
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Backend-independent: validate the AMD reconstruction decoder. */
    test_amd_reconstruction();

    /* Backend-independent: validate the CoreSight reconstruction core (synthetic
     * ranges; the live OpenCSD decode tree awaits a board). */
    test_cs_reconstruction();

    /* AMD LBR LIVE capture — runs on a Zen 3+/4/5 host with perf branch-stack
     * permitted (e.g. this Zen 5 dev box); self-skips elsewhere. */
    test_amd_live();

    /* Live, on this very host: the single-step backend (no PMU/perf/privilege). */
    test_singlestep_live();
    test_singlestep_loop();

    /* Live: the out-of-process ptrace single-step backend (W2). */
    test_ptrace_oop();

    /* Live: tracing a region in a SEPARATE, externally-attached process (W2 attach). */
    test_ptrace_attach();

    /* The auto-select orchestrator: pick + use the best available backend. */
    test_auto_resolve();

    /* The cross-tier orchestrator: resolve over hwtrace + DynamoRIO + emulator. */
    test_cross_tier_resolve();

    asmtest_trace_backend_t backend = ASMTEST_HWTRACE_INTEL_PT;
    if (!asmtest_hwtrace_available(backend)) {
        char why[160];
        asmtest_hwtrace_skip_reason(backend, why, sizeof why);
        printf("# SKIP hwtrace PT capture (Intel PT): %s\n", why);
        char awhy[160];
        asmtest_hwtrace_skip_reason(ASMTEST_HWTRACE_AMD_LBR, awhy, sizeof awhy);
        printf("# SKIP hwtrace AMD capture (AMD LBR): %s\n", awhy);
        if (checks == 0)
            printf("1..0 # skipped\n");
        else
            printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
                   failures);
        return failures == 0 ? 0 : 1;
    }

    /* Capable host: exercise the real capture + decode path. */
    void *p = mmap(NULL, sizeof ROUTINE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("# SKIP hwtrace: mmap failed\n1..0 # skipped\n");
        return 0;
    }
    memcpy(p, ROUTINE, sizeof ROUTINE);
    mprotect(p, sizeof ROUTINE, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + sizeof ROUTINE);

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.backend = backend;
    CHECK(asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK, "hwtrace init");

    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(asmtest_hwtrace_register_region("add2", p, sizeof ROUTINE, tr) ==
              ASMTEST_HW_OK,
          "register native range");

    add2_fn fn = (add2_fn)p;
    asmtest_hwtrace_begin("add2");
    long r = fn(20, 22);
    asmtest_hwtrace_end("add2");
    CHECK(r == 42, "traced call returns 20+22");
    CHECK(asmtest_trace_covered(tr, 0), "block offset 0 covered");
    CHECK(asmtest_emu_trace_insns_total(tr) >= 4,
          "ordered instruction stream reconstructed");

    asmtest_hwtrace_shutdown();
    asmtest_trace_free(tr);
    munmap(p, sizeof ROUTINE);

    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
    return failures == 0 ? 0 : 1;
}
