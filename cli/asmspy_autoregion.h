/* asmspy_autoregion.h — pick a data-flow region from statistical hot edges, so
 * `--dataflow <pid>` can trace what a process is ACTUALLY DOING without the
 * operator naming a function.
 *
 * Pure (no ptrace, no perf, no allocation, no I/O), so the decision is unit-
 * testable on ANY host (cli/test_autoregion.c) rather than only on an AMD IBS
 * box. That split is the point: the sampler that FEEDS this is AMD-only hardware
 * and self-skips elsewhere, but the ranking — the half that can actually be
 * wrong — is covered everywhere.
 *
 * Header-only + static inline, matching the other extracted decision modules
 * (asmspy_graphsort.h, asmspy_treefilter.h, asmspy_dataview.h, asmspy_logview.h).
 *
 * ---------------------------------------------------------------------------
 * THE RULE: rank the hottest ENTRY edge. Not the hottest edge, not the hottest PC.
 *
 * asmspy_engine_dataflow arms an int3 at the region's ENTRY and waits for a thread
 * to arrive. So the property a pick must have is ARRIVAL RATE, not residency — and
 * the only evidence of the right TYPE is a direct observation of that same event.
 * An IBS-Op edge whose `to_addr` equals a symbol's START is exactly that: the
 * branch that arrived at the entry retired. Everything else is a different event
 * wearing the same clothes:
 *
 *   - a RETURN lands mid-function by construction (and is tagged is_return);
 *   - a loop BACK-EDGE lands mid-function;
 *   - a PC histogram measures where time is SPENT, which is dominated by exactly
 *     the functions that entered once and never come back — `main`, an event loop.
 *     Feeding one of those to the producer is how you hang it.
 *
 * MEASURED (2026-07-17, Zen 5, a 3-level victim: main -> outer_cold -> mid_warm x2
 * -> leaf_hot x8): entry-edge counts reproduce the TRUE invocation ratios —
 * 3903/487 = 8.0x (the exact loop trip count), 487/237 = 2.05x (the exact call-site
 * count). And `main` had NO entry edge at all, because it was entered before we
 * attached. An entry edge is evidence of re-entry BY CONSTRUCTION, which is what
 * makes "hottest entry" and "the breakpoint fires promptly" nearly the same
 * predicate. Nearly: see the honest note on non-stationarity below.
 *
 * ---------------------------------------------------------------------------
 * THE VACUITY TRAP, and why `size > 0` is a correctness rule and not hygiene.
 *
 * asmspy_symtab_at's zero-size branch is `query == s->addr` — exact-start-only. So
 * for ANY symbol with unknown size, resolution succeeds ONLY at its entry, and the
 * test `to_addr == sym->addr` is TRUE BY CONSTRUCTION rather than by measurement.
 * Every zero-size symbol would look like a pure stream of entry arrivals and win on
 * a technicality. Requiring size > 0 removes them — and costs nothing, because a
 * region needs a real extent anyway (the producer takes (base, len), and
 * resolve_region already refuses an unsized symbol for the same reason).
 *
 * ---------------------------------------------------------------------------
 * HONEST SCOPE. This makes a hang IMPROBABLE, it does not BOUND it: the evidence
 * window ends milliseconds before the attach, so a target that changes phase in
 * that gap still would not arrive. What BOUNDS it is the producer's entry-wait
 * deadline (DFP_ENTRY_WAIT_MS), which turns that residual case from "hangs" into
 * "not seen entering". The ranking and the bound are complementary; neither alone
 * is enough, and the bound is the one that cannot be argued with.
 */
#ifndef ASMSPY_AUTOREGION_H
#define ASMSPY_AUTOREGION_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* One ranked candidate region: a function whose ENTRY was observed being branched
 * to. `addr`/`size` are what the data-flow engine takes as (base, len). */
typedef struct {
    uint64_t addr;               /* the symbol's entry == the region base     */
    uint64_t size;               /* its extent == the region len (always >0)  */
    const char *name;            /* borrowed from the symtab                  */
    const char *module;          /* borrowed; may be NULL                     */
    unsigned long long arrivals; /* entry samples, SUMMED over all call sites */
    unsigned sites;              /* distinct call sites seen arriving here    */
} asmspy_autocand_t;

/* What the caller must tell us about an address. Passed as a callback so this
 * header stays pure: the real resolver (asmspy_resolve: ELF -> JIT -> refresh)
 * lives in asmspy_proc.c and needs a live pid, while the unit test hands over a
 * hand-built table. Return 0 when `addr` resolves, filling in the four out
 * params (start, size, name, module); return non-0 when it does not.
 *
 * A resolved size may legitimately be 0 (an unsized symbol) — asmspy_autoregion_rank
 * drops those itself rather than making every implementation remember to. */
typedef int (*asmspy_auto_resolve_fn)(void *ctx, uint64_t addr, uint64_t *start,
                                      uint64_t *size, const char **name,
                                      const char **module);

/* Substring match with "no pattern matches everything" semantics — the same rule
 * --module= uses (asmspy_tf_match), deliberately: two module filters in one tool
 * that disagree about what "libc" matches would be worse than either. */
static inline int asmspy_ar_match(const char *hay, const char *needle) {
    if (!needle || !*needle)
        return 1;
    return hay && strstr(hay, needle) != NULL;
}

/* Rank the entry-arrival candidates in `edges` into `out` (descending arrivals),
 * returning how many were written (<= out_cap).
 *
 * An edge counts as an ENTRY ARRIVAL iff ALL of:
 *   - it is not a return          (is_return == 0)
 *   - its `to_addr` resolves      (resolve() == 0)
 *   - to_addr == the symbol start (an ENTRY, not a jump into the middle)
 *   - the symbol has size > 0     (see the vacuity note above)
 *   - the symbol's module passes `module_filter` (NULL/"" = everything)
 *
 * Arrivals are SUMMED per symbol across call sites: a function called from three
 * places is being entered at the sum of those rates, and it is one region either
 * way. `sites` counts how many distinct call sites contributed, which is how a
 * caller can tell "one hot loop calls it" from "the whole program calls it".
 *
 * Ties are broken by ADDRESS, ascending — never by input order, which for a
 * statistical sampler is a coin flip that would make the pick unreproducible run
 * to run on identical behaviour.
 *
 * out_cap sizes BOTH the output and the fold table. Accumulation is streaming,
 * so once the table is full a NEW symbol is dropped even though its SUMMED
 * arrivals could have overtaken an admitted one — an edge's own count says
 * nothing about its symbol's eventual total. A caller that must never lose a
 * candidate sizes out_cap by the EDGE count (each edge introduces at most one
 * candidate), as auto_pick does; a small fixed cap means "the first out_cap
 * symbols seen", not "the top out_cap". test_autoregion #10 pins this down.
 *
 * Pure: reads `edges` and whatever resolve() returns, writes only `out`. */
static inline size_t
asmspy_autoregion_rank(const asmspy_sample_edge_t *edges, size_t n,
                       asmspy_auto_resolve_fn resolve, void *ctx,
                       const char *module_filter, asmspy_autocand_t *out,
                       size_t out_cap) {
    size_t nout = 0;
    if (!edges || !resolve || !out || out_cap == 0)
        return 0;

    for (size_t i = 0; i < n; i++) {
        const asmspy_sample_edge_t *e = &edges[i];
        /* A return lands mid-function by construction; the sampler already tells
         * us, so we need not rediscover it from the address. */
        if (e->is_return)
            continue;

        uint64_t start = 0, size = 0;
        const char *name = NULL, *module = NULL;
        if (resolve(ctx, e->to_addr, &start, &size, &name, &module) != 0)
            continue;
        /* THE ENTRY TEST. A mid-function landing (a loop back-edge, a jump into a
         * hot path) is not an arrival the entry breakpoint can ever see. */
        if (e->to_addr != start)
            continue;
        /* Unsized: `to_addr == start` is true by construction for these (see the
         * header note), and there is no extent to hand the engine anyway. */
        if (size == 0)
            continue;
        if (!asmspy_ar_match(module, module_filter))
            continue;

        /* Fold into an existing candidate — two call sites, one region. */
        size_t j = 0;
        for (; j < nout; j++)
            if (out[j].addr == start)
                break;
        if (j < nout) {
            out[j].arrivals += e->count;
            out[j].sites++;
            continue;
        }
        if (nout == out_cap)
            continue; /* full: a NEW symbol is dropped — sound only when
                       * out_cap >= the edge count (see the doc comment); a
                       * late symbol could still out-SUM an admitted one */
        out[nout].addr = start;
        out[nout].size = size;
        out[nout].name = name;
        out[nout].module = module;
        out[nout].arrivals = e->count;
        out[nout].sites = 1;
        nout++;
    }

    /* Insertion sort: descending arrivals, ties by ascending address. nout is
     * bounded by out_cap (a handful), so the O(n^2) is free and the stability is
     * easier to reason about than qsort's. */
    for (size_t i = 1; i < nout; i++) {
        asmspy_autocand_t key = out[i];
        size_t j = i;
        while (j > 0 && (out[j - 1].arrivals < key.arrivals ||
                         (out[j - 1].arrivals == key.arrivals &&
                          out[j - 1].addr > key.addr))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return nout;
}

/* ---------------------------------------------------------------------------
 * THE PORTABLE RULE: rank RESIDENCY, and own the hazard out loud.
 *
 * The software-clock sampler (asmtest_swclock_survey_process) exists so --auto
 * is not AMD-only, but it delivers IPs, not edges — and a time-based IP
 * histogram measures where time is SPENT, which is dominated by exactly the
 * functions the entry rule above exists to reject: entered once, never return
 * (main, an event loop, auto_victim's grind_forever). There is no entry
 * evidence in this input BY CONSTRUCTION; no fold over it can recover the
 * entry rule.
 *
 * So this ranking is honest about being the WEAKER rule: its top candidate can
 * be a region whose entry breakpoint never fires. A caller MUST pair it with
 * the producer's bounded entry wait (which turns that case from "hangs" into
 * "not seen entering") and SHOULD walk the ranked candidates on that refusal —
 * the correct pick is usually a few rows down (the hot callee), and each
 * refusal is itself a truthful statement about the target.
 *
 * One raw input bucket: an address the target was observed EXECUTING, and how
 * many samples landed exactly there. */
typedef struct {
    uint64_t ip;
    unsigned long long count;
} asmspy_ip_hit_t;

/* Rank residency candidates from an IP histogram into `out` (descending
 * samples, ties by ascending address — the same determinism rule as the entry
 * ranking), returning how many were written (<= out_cap).
 *
 * An ip counts toward a symbol iff:
 *   - it resolves                  (resolve() == 0)
 *   - it lies INSIDE the symbol    (start <= ip < start + size — containment,
 *                                   NOT the entry test: residency accrues over
 *                                   the whole body)
 *   - the symbol has size > 0      (unsized symbols resolve exact-start-only,
 *                                   so containment would be vacuous — and there
 *                                   is no extent to hand the engine anyway)
 *   - its module passes `module_filter` (NULL/"" = everything)
 *
 * In `out`, `arrivals` carries SAMPLES (residency weight, not entry arrivals)
 * and `sites` counts DISTINCT SAMPLED OFFSETS inside the symbol — a body with
 * many hot offsets is a loop; a single hot offset is usually a stall point.
 *
 * out_cap sizes both the output and the fold table, with the same streaming
 * caveat as asmspy_autoregion_rank: size it by the bucket count (each bucket
 * introduces at most one candidate) to make the fold lossless.
 *
 * Pure: reads `hits` and whatever resolve() returns, writes only `out`. */
static inline size_t
asmspy_autoregion_rank_ip(const asmspy_ip_hit_t *hits, size_t n,
                          asmspy_auto_resolve_fn resolve, void *ctx,
                          const char *module_filter, asmspy_autocand_t *out,
                          size_t out_cap) {
    size_t nout = 0;
    if (!hits || !resolve || !out || out_cap == 0)
        return 0;

    for (size_t i = 0; i < n; i++) {
        const asmspy_ip_hit_t *hit = &hits[i];
        uint64_t start = 0, size = 0;
        const char *name = NULL, *module = NULL;
        if (resolve(ctx, hit->ip, &start, &size, &name, &module) != 0)
            continue;
        if (size == 0)
            continue; /* see the vacuity note on the entry rule */
        /* CONTAINMENT, not entry: a resolver that returns a symbol not
         * actually covering the ip (a gap answered with a neighbour) must not
         * be trusted — test_symtab pins the resolver, this pins the fold. */
        if (hit->ip < start || hit->ip >= start + size)
            continue;
        if (!asmspy_ar_match(module, module_filter))
            continue;

        size_t j = 0;
        for (; j < nout; j++)
            if (out[j].addr == start)
                break;
        if (j < nout) {
            out[j].arrivals += hit->count;
            out[j].sites++; /* one more distinct sampled offset */
            continue;
        }
        if (nout == out_cap)
            continue; /* full: lossless only when out_cap >= bucket count */
        out[nout].addr = start;
        out[nout].size = size;
        out[nout].name = name;
        out[nout].module = module;
        out[nout].arrivals = hit->count;
        out[nout].sites = 1;
        nout++;
    }

    for (size_t i = 1; i < nout; i++) {
        asmspy_autocand_t key = out[i];
        size_t j = i;
        while (j > 0 && (out[j - 1].arrivals < key.arrivals ||
                         (out[j - 1].arrivals == key.arrivals &&
                          out[j - 1].addr > key.addr))) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = key;
    }
    return nout;
}

#endif /* ASMSPY_AUTOREGION_H */
