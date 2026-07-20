/*
 * test_branchtile.c — E6: the deterministic branchsnap TILED at checkpoints, merged into
 * the WindowHot sampled-endpoint surface (asmtest_hwtrace_tile_window_amd).
 *
 * WHAT IS UNDER TEST, precisely. Tiling plants a HW execution breakpoint at each of up to
 * 4 absolute checkpoints; each hit freezes the ~16-deep LBR and a BPF program snapshots
 * it. Those ISLANDS merge into the same ips[] endpoint stream the statistical survey
 * fills. The merged result is SAMPLED / PARTIAL COVERAGE — exact at the checkpoints,
 * blind between them — NOT an exact whole-window trace (a hardware dead end on AMD,
 * DECLINED). So the assertions below are about COVERAGE and PROVENANCE, never completeness.
 *
 * ANTI-VACUITY. Each claim carries an oracle that can actually fail:
 *   1. real checkpoint -> islands >= 1 AND ntiled == islands * lbr_depth. The DEPTH
 *      equality is the assertion, not "ntiled >= 1": one hit freezes the whole 16-deep
 *      stack, so a complete merge contributes exactly 16 endpoints. The weak form is
 *      satisfied by island[0] ALONE — and island[0] is present BY HARDWARE CONSTRUCTION
 *      (the breakpoint sits on the entry, so the newest retired edge is necessarily the
 *      call that reached it), so it can never fail and leaves [1..15] compared to
 *      nothing. Proven: dropping every other entry (i += 2) halves ntiled 16 -> 8 and
 *      every weak check stays green.
 *   2. NEGATIVE CONTROL: a never-executed checkpoint -> islands == 0 AND ntiled == 0.
 *      Without this, "islands > 0" could be an artifact of arming rather than evidence
 *      of execution — the same shape as a test that passes because nothing moved.
 *   3. the island prefix ips[0..ntiled) CONTAINS the cold leaf's entry PC. Deterministic,
 *      not statistical: the breakpoint sits ON that entry, so the newest retired LBR
 *      entry is necessarily the call that reached it (to == the entry).
 *   4. DIFFERENTIAL ORACLE: the same body, same period, tiled vs. UNTILED. The cold leaf
 *      runs ONCE among ~10^7 branches, so a survey at a realistic period is blind to it
 *      while tiling sees it every time. Coverage the sampler cannot reach is exactly the
 *      gap E6 exists to fill, and a merge that did nothing would collapse this to a tie.
 *
 * "covered OR truncated" — the repo's rule for LBR-derived coverage on small fixtures,
 * and it is applied here with the term that MATTERS: tile_truncated (the ISLAND merge
 * lost endpoints), NEVER the survey-wide truncated. The rule is sound only when the
 * truncation term is CAUSALLY TIED to the property asserted. `truncated` is owned by the
 * SAMPLER — it also fires when the sampler's 256 KiB ring runs near-full, which says
 * nothing whatever about whether this island kept its endpoints. Disjoining island
 * content with it is escapable by an unrelated producer, and it was: clobbering the
 * island data and growing the hot loop until the ring filled made the managed sibling
 * report GREEN while printing "covered? TILED=False". "We measured truncated=0" does not
 * rescue it — that holds only for today's fixture size, and a 20-30% branch-count drift
 * (a different Zen part, a JIT change) would pin it true forever and silently retire the
 * assert. Pick the causally-tied term instead of sizing the fixture and hoping.
 *
 * Self-skips (exit 0) without the BPF toolchain / CAP_BPF+CAP_PERFMON / AMD LbrExtV2 —
 * but the docker-hwtrace-codeimage lane HAS all three, so on a Zen 4/5 box it runs live.
 */
#define _GNU_SOURCE

#include "asmtest_hwtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)

int asmtest_amd_snapshot_available(void);
/* E6 producer: declared here, not in the public tier header — it is an additive producer
 * into the existing WindowHot surface, not a new API (mirroring how E5's
 * asmtest_hwtrace_sample_window_amd_weighted is wired). */
int asmtest_hwtrace_tile_window_amd(void (*run_fn)(void *), void *arg,
                                    int period, const uint64_t *checkpoints,
                                    int ncheckpoints, uint64_t *ips, size_t cap,
                                    size_t *nips, size_t *ntiled, int *islands,
                                    int *tile_truncated, int *truncated);
/* Runtime AMD branch-stack depth (CPUID 0x80000022 EBX; 16 on every shipping part) —
 * the exact number of endpoints ONE island must contribute. */
int asmtest_amd_lbr_depth(void);

static volatile long g_sink;

/* The COLD leaf: called exactly ONCE per window, among ~10^7 hot branches. This is the
 * "too small / too fast to be caught in-region" routine the sampled survey honestly
 * drops — the case the plan says the deterministic snapshot exists to reconstruct. */
__attribute__((noinline)) static long cold_leaf(long x) {
    g_sink += x ^ 0x5a5a;
    return (long)g_sink;
}

/* T7: a second cold leaf called immediately BEFORE cold_leaf. Its call/ret pair plants
 * two known retired edges 2-3 slots deep in the window frozen at cold_leaf's entry
 * breakpoint. Unlike island[0] (present BY HARDWARE CONSTRUCTION — the breakpoint sits
 * on the entry, so the newest retired edge is necessarily the call that reached it), the
 * preamble entry is present ONLY if those next-newest slots survived the #DB. That is the
 * discriminating non-eviction evidence for the CALL-target checkpoint shape — the third
 * boundary shape after the validated `ret` and tail-`jmp` exits — which the depth-equality
 * (check 1) cannot provide (16 stale endpoints also satisfy ntiled == islands * depth). */
__attribute__((noinline)) static long preamble_leaf(long x) {
    g_sink += x + 0x1234;
    return (long)g_sink;
}

/* Never called. Its entry is the NEGATIVE-CONTROL checkpoint: a real, armable,
 * breakpoint-able address that execution never reaches. */
__attribute__((noinline)) static long never_called(long x) {
    g_sink += x * 3;
    return (long)g_sink;
}

/* The HOT work: a branchy inner loop, called HOT_ITERS times, so the window is dominated
 * by branches that have nothing to do with the cold leaf. */
__attribute__((noinline)) static long hot_work(long x) {
    long a = 0;
    for (long i = 0; i < 64; i++) {
        a += (i ^ x);
        if (a & 1)
            a += 3;
    }
    return a;
}

#define HOT_ITERS 200000

static void body(void *arg) {
    (void)arg;
    long a = 0;
    for (long i = 0; i < HOT_ITERS; i++)
        a += hot_work(i);
    a += preamble_leaf(
        a); /* T7: plant known edges 2-3 slots before the checkpoint */
    a += cold_leaf(a); /* exactly once, at the very end */
    g_sink += a;
}

/* A realistic survey period. The default WindowHot period (16) samples so densely that
 * each 16-deep window nearly tiles the whole run — which would HIDE the gap tiling fills.
 * A real near-native survey uses a large period; at 50000 the sampler observes ~0.03% of
 * the branch history, and the one-shot cold leaf falls in the blind 99.97%. Choosing the
 * regime where the producers genuinely differ is the point of the differential. */
#define SURVEY_PERIOD 50000
#define IPS_CAP       65536

/* ASMTEST_TILE_REQUIRE=1 turns every self-skip below into a FAILURE. The self-skips are
 * correct on a host without the substrate — but on the lane built precisely to supply it
 * (docker-hwtrace-dotnet-amd: Zen 4/5 + CAP_BPF + CAP_PERFMON + a BPF-toolchain build) a
 * skip is not a pass, it is the feature silently gone. Same idea as CLEANROOM_ONLY=<lang>,
 * which fails a clean-room run whose binding self-skipped rather than let it pass vacuously.
 * Unset (every other lane) the skips stay skips. */
static int tile_required(void) {
    const char *e = getenv("ASMTEST_TILE_REQUIRE");
    return e != NULL && e[0] != '\0' && e[0] != '0';
}

/* Report a self-skip, and return the process exit code it should produce. */
static int skip_or_fail(const char *why) {
    if (tile_required()) {
        printf("not ok - branchtile: %s — but ASMTEST_TILE_REQUIRE=1 says this "
               "lane MUST be able to tile, so a skip here is a failure\n",
               why);
        return 1;
    }
    printf("# SKIP branchtile: %s\n", why);
    return 0;
}

static int contains(const uint64_t *ips, size_t n, uint64_t target) {
    for (size_t i = 0; i < n; i++)
        if (ips[i] == target)
            return 1;
    return 0;
}

/* One capture. `cps`/`ncp` NULL/0 == the plain untiled survey (the differential's control
 * arm), so both arms go through the SAME entry point and differ in exactly one input. */
struct cap_result {
    int rc;
    size_t nips;
    size_t ntiled;
    int islands;
    int tile_truncated; /* the ISLAND merge lost endpoints (the ONLY term an
                         * island-content assert may honestly disjoin with) */
    int truncated;      /* survey-wide prefix: ALSO fires on SAMPLER-ring loss,
                         * which says nothing about island content */
    int cold_in_tiles; /* cold_leaf entry present in the ISLAND prefix        */
    int cold_anywhere; /* cold_leaf entry present anywhere in ips[]           */
    int hot_anywhere; /* hot_work entry present anywhere (sanity: we sampled) */
    int preamble_in_tiles; /* T7: preamble_leaf entry present in the ISLAND
                            * prefix — the CALL-target non-eviction discriminator */
};

static void capture(const uint64_t *cps, int ncp, struct cap_result *out) {
    static uint64_t ips[IPS_CAP];
    memset(out, 0, sizeof *out);
    memset(ips, 0, sizeof ips);
    out->rc = asmtest_hwtrace_tile_window_amd(
        body, NULL, SURVEY_PERIOD, cps, ncp, ips, IPS_CAP, &out->nips,
        &out->ntiled, &out->islands, &out->tile_truncated, &out->truncated);
    if (out->rc != ASMTEST_HW_OK)
        return;
    uint64_t cold = (uint64_t)(uintptr_t)&cold_leaf;
    uint64_t hot = (uint64_t)(uintptr_t)&hot_work;
    uint64_t preamble = (uint64_t)(uintptr_t)&preamble_leaf;
    out->cold_in_tiles = contains(ips, out->ntiled, cold);
    out->cold_anywhere = contains(ips, out->nips, cold);
    out->hot_anywhere = contains(ips, out->nips, hot);
    out->preamble_in_tiles = contains(ips, out->ntiled, preamble);
}

#define TRIALS 5

int main(void) {
    printf("== branchtile-test (E6: branchsnap tiled at checkpoints -> "
           "WindowHot) ==\n");
    if (!asmtest_amd_snapshot_available())
        return skip_or_fail("substrate absent (needs AMD LbrExtV2 + perfmon_v2 "
                            "+ kernel >= 6.10)");

    const uint64_t cold_cp = (uint64_t)(uintptr_t)&cold_leaf;
    const uint64_t bogus_cp = (uint64_t)(uintptr_t)&never_called;
    int fails = 0;

    /* --- probe: is the tiled capture actually LIVE here? ----------------------- */
    struct cap_result probe;
    capture(&cold_cp, 1, &probe);
    if (probe.rc == ASMTEST_HW_ENOSYS)
        return skip_or_fail("built without the BPF toolchain");
    if (probe.rc != ASMTEST_HW_OK) {
        char msg[160];
        snprintf(msg, sizeof msg,
                 "survey unavailable (rc=%d; needs CAP_PERFMON / AMD branch "
                 "stack)",
                 probe.rc);
        return skip_or_fail(msg);
    }
    if (probe.islands == 0)
        /* The survey ran but no island merged. Two causes hide here: tiling could not
         * ARM (no CAP_BPF / no free debug register), or it armed and the checkpoint was
         * never REACHED. ASMTEST_AMD_DEBUG=1 prints "tile_begin: armed ..." and
         * separates them — the .NET sibling hit exactly the second case (a precode-stub
         * entry PC), which armed cleanly and never fired. */
        return skip_or_fail(
            "no island merged (islands=0): tiling could not arm (needs CAP_BPF "
            "+ "
            "CAP_PERFMON + a free debug register), or it armed and the "
            "checkpoint "
            "was never reached — ASMTEST_AMD_DEBUG=1 tells which");

    /* --- 1. a REAL checkpoint yields islands + a COMPLETE island ---------------- */
    /* `depth` is the runtime branch-stack depth (16 on every shipping AMD part). ONE
     * checkpoint hit freezes the whole stack, so a complete merge contributes EXACTLY
     * depth endpoints — assert that, not `ntiled >= 1`. The weak form is satisfied by
     * island[0] ALONE, and island[0] is present BY HARDWARE CONSTRUCTION (the breakpoint
     * sits on cold_leaf's entry, so the newest retired edge is necessarily the call that
     * reached it) — it can never fail, leaving entries [1..15] compared to nothing. That
     * is 15 of the 16 endpoints E6 exists to add, unverified. Proven: dropping every
     * other entry (`i += 2`) halves ntiled 16 -> 8 and the weak form stays green. */
    const int depth = asmtest_amd_lbr_depth();
    printf("branchtile arm: rc=%d islands=%d ntiled=%zu nips=%zu lbr_depth=%d "
           "tile_truncated=%d truncated=%d\n",
           probe.rc, probe.islands, probe.ntiled, probe.nips, depth,
           probe.tile_truncated, probe.truncated);
    if (depth <= 0) {
        printf("not ok - branchtile: asmtest_amd_lbr_depth() returned %d on a "
               "host that just captured an island\n",
               depth);
        fails++;
    } else if (probe.islands >= 1 &&
               probe.ntiled == (size_t)probe.islands * (size_t)depth)
        printf("ok - branchtile: %d island(s) merged EXACTLY %zu endpoints "
               "(islands * lbr_depth = %d * %d) — the whole frozen window, not "
               "just its newest edge\n",
               probe.islands, probe.ntiled, probe.islands, depth);
    else {
        printf(
            "not ok - branchtile: islands=%d merged ntiled=%zu, expected "
            "%zu (islands * lbr_depth=%d) — a partial island means endpoints "
            "were silently dropped from the merge\n",
            probe.islands, probe.ntiled, (size_t)probe.islands * (size_t)depth,
            depth);
        fails++;
    }

    /* --- 2. NEGATIVE CONTROL: a checkpoint that never executes ----------------- */
    struct cap_result neg;
    capture(&bogus_cp, 1, &neg);
    printf("branchtile negative-control (checkpoint on never_called): rc=%d "
           "islands=%d ntiled=%zu nips=%zu truncated=%d\n",
           neg.rc, neg.islands, neg.ntiled, neg.nips, neg.truncated);
    if (neg.rc == ASMTEST_HW_OK && neg.islands == 0 && neg.ntiled == 0)
        printf("ok - branchtile negative control: an armed-but-never-reached "
               "checkpoint merges NOTHING (islands track execution, not "
               "arming)\n");
    else {
        printf("not ok - branchtile negative control: unreached checkpoint "
               "still reported islands=%d ntiled=%zu\n",
               neg.islands, neg.ntiled);
        fails++;
    }

    /* --- 3. the island prefix names the checkpoint's own entry ----------------- */
    /* "covered OR truncated" — but disjoined with tile_truncated, NEVER with the
     * survey-wide truncated. The repo's rule is sound only when the truncation term is
     * CAUSALLY TIED to the property asserted: `truncated` ALSO fires when the SAMPLER's
     * ring goes near-full, which says nothing whatever about whether this island kept its
     * endpoints — a fixture that grew until the sampler lost records would satisfy this
     * assert forever with the island never checked again. tile_truncated means only "the
     * island merge lost endpoints", which is exactly the honest escape for this claim. */
    if (probe.cold_in_tiles || probe.tile_truncated)
        printf(
            "ok - branchtile: the island prefix ips[0..%zu) contains the "
            "checkpoint's entry PC 0x%llx (cold_in_tiles=%d "
            "tile_truncated=%d) — the frozen window's newest edge IS the call "
            "that reached it\n",
            probe.ntiled, (unsigned long long)cold_cp, probe.cold_in_tiles,
            probe.tile_truncated);
    else {
        printf(
            "not ok - branchtile: island prefix missing the checkpoint entry "
            "PC 0x%llx and the island merge lost nothing "
            "(tile_truncated=0)\n",
            (unsigned long long)cold_cp);
        fails++;
    }

    /* --- 3b. CALL-target non-eviction: the island keeps the PRE-CALL preamble --- */
    /* The third boundary shape (after `ret` and tail-`jmp`): a #DB execution breakpoint at
     * a CALL target (cold_leaf's entry) must NOT evict the 2-3 next-newest slots — the
     * call/ret edges of preamble_leaf, which ran immediately before — before
     * bpf_get_branch_snapshot() reads the frozen stack. Unlike island[0] (present by
     * hardware construction), preamble_leaf's entry is present ONLY if those slots
     * survived, so this is the discriminating non-eviction evidence the depth-equality
     * (check 1) cannot give. Disjoined with tile_truncated (the island merge lost
     * endpoints), NEVER the survey-wide truncated (causally untied — see the file header).
     * MUTATION EVIDENCE (documented one-off, never committed — Phase-9 style): in
     * btile_on_event (src/branchsnap.c), an env-gated `continue` on entries whose e.to
     * equals the preamble entry PC. OBSERVED 2026-07-20 on the Ryzen 9 9950X (Zen 5)
     * dev box (ASLR off for a stable PC): baseline 3b `ok` (preamble_in_tiles=1,
     * tile_truncated=0); with the drop armed on that PC, 3b flips to `not ok` (island
     * prefix missing the pre-call preamble entry) while the negative control (check 2)
     * stays green — 3b can fail. LIVE PASS confirmed the same day, unmutated:
     * make docker-hwtrace-codeimage (CAP_BPF+CAP_PERFMON, real LbrExtV2, lbr_depth=16). */
    if (probe.preamble_in_tiles || probe.tile_truncated)
        printf(
            "ok - branchtile: the island prefix contains preamble_leaf's entry "
            "PC 0x%llx (preamble_in_tiles=%d tile_truncated=%d) — the "
            "CALL-target "
            "#DB did NOT evict the 2-3 next-newest slots\n",
            (unsigned long long)(uintptr_t)&preamble_leaf,
            probe.preamble_in_tiles, probe.tile_truncated);
    else {
        printf(
            "not ok - branchtile: island prefix missing the pre-call "
            "preamble entry PC 0x%llx and the island merge lost nothing "
            "(tile_truncated=0) — the CALL-target #DB evicted branch history "
            "before the snapshot read (a FINDING, not a test bug)\n",
            (unsigned long long)(uintptr_t)&preamble_leaf);
        fails++;
    }

    /* --- 4. DIFFERENTIAL: tiled vs. untiled over the SAME body ----------------- */
    /* The cold leaf runs once among ~10^7 branches. Tiling sees it deterministically;
     * a survey at SURVEY_PERIOD is blind to it. If the merge were a no-op these two
     * arms would tie at zero — so the gap IS the feature. */
    int tiled_cov = 0, untiled_cov = 0, tiled_trunc = 0, untiled_trunc = 0;
    size_t tiled_nips_tot = 0, untiled_nips_tot = 0;
    int islands_tot = 0;
    for (int t = 0; t < TRIALS; t++) {
        struct cap_result a, b;
        capture(&cold_cp, 1, &a); /* tiled   */
        capture(NULL, 0, &b);     /* untiled */
        if (a.rc == ASMTEST_HW_OK) {
            tiled_cov += a.cold_anywhere;
            tiled_trunc += a.tile_truncated;
            tiled_nips_tot += a.nips;
            islands_tot += a.islands;
            /* Every trial's island must be COMPLETE, not just the probe's — a drop that
             * only bites under repetition (ringbuf pressure) would otherwise hide here. */
            if (depth > 0 && a.islands >= 1 &&
                a.ntiled != (size_t)a.islands * (size_t)depth &&
                !a.tile_truncated) {
                printf("not ok - branchtile: trial %d merged ntiled=%zu for "
                       "islands=%d (expected %zu) with tile_truncated=0\n",
                       t, a.ntiled, a.islands,
                       (size_t)a.islands * (size_t)depth);
                fails++;
            }
        }
        if (b.rc == ASMTEST_HW_OK) {
            untiled_cov += b.cold_anywhere;
            untiled_trunc += b.truncated;
            untiled_nips_tot += b.nips;
            /* The untiled arm must never fabricate an island. */
            if (b.islands != 0 || b.ntiled != 0) {
                printf("not ok - branchtile: untiled arm reported islands=%d "
                       "ntiled=%zu (must be 0)\n",
                       b.islands, b.ntiled);
                fails++;
            }
        }
    }
    printf("branchtile differential over %d trials @ period=%d: "
           "TILED cold-leaf covered %d/%d (islands=%d, %zu endpoints, "
           "tile_truncated %d/%d) vs UNTILED covered %d/%d (%zu endpoints, "
           "truncated %d/%d)\n",
           TRIALS, SURVEY_PERIOD, tiled_cov, TRIALS, islands_tot,
           tiled_nips_tot, tiled_trunc, TRIALS, untiled_cov, TRIALS,
           untiled_nips_tot, untiled_trunc, TRIALS);
    /* Assert the tiled arm covers the leaf EVERY trial (deterministic), in the
     * covered-or-TILE-truncated shape (never the survey-wide flag — see check 3); and
     * that it strictly beats the sampler, which is the claim E6 makes. untiled_cov is
     * expected 0 — a sampler that somehow caught the leaf in every trial would mean
     * SURVEY_PERIOD no longer selects the blind regime and the differential must be
     * re-tuned, so failing loudly there is correct. */
    if ((tiled_cov == TRIALS || tiled_trunc == TRIALS) &&
        tiled_cov > untiled_cov)
        printf("ok - branchtile differential: tiling covers the one-shot cold "
               "leaf %d/%d where the untiled survey covers it %d/%d — exact "
               "islands reach what sampling at period=%d cannot\n",
               tiled_cov, TRIALS, untiled_cov, TRIALS, SURVEY_PERIOD);
    else {
        printf("not ok - branchtile differential: tiled=%d/%d untiled=%d/%d "
               "(tiling must strictly out-cover the sampler on a one-shot "
               "routine)\n",
               tiled_cov, TRIALS, untiled_cov, TRIALS);
        fails++;
    }

    /* --- 5. tiling does not BREAK the survey it enriches ----------------------- */
    /* The tiled arm must still be a survey: the hot method has to appear in the
     * sampled remainder, proving the branch-stack drain kept working alongside the
     * breakpoints (the merge ADDS a producer; it must not displace one). */
    if (probe.hot_anywhere || probe.truncated)
        printf("ok - branchtile: the sampled producer still runs alongside "
               "tiling (hot_work endpoint present; %zu sampled endpoints "
               "after the %zu-endpoint island prefix)\n",
               probe.nips - probe.ntiled, probe.ntiled);
    else {
        printf("not ok - branchtile: tiling displaced the sampled survey "
               "(hot_work absent from %zu endpoints, truncated=0)\n",
               probe.nips);
        fails++;
    }

    if (fails > 0) {
        printf("not ok - branchtile: %d check(s) failed\n", fails);
        return 1;
    }
    printf("ok - branchtile: E6 tiling merges deterministic checkpoint islands "
           "into the WindowHot endpoint surface (SAMPLED/PARTIAL coverage — "
           "exact at checkpoints, blind between them; NOT a whole-window "
           "trace)\n");
    return 0;
}

#else
int main(void) {
    printf("# SKIP branchtile: x86-64 Linux only\n");
    return 0;
}
#endif
