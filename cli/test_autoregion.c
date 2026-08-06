/* test_autoregion.c — unit tests for the --dataflow --auto region picker
 * (cli/asmspy_autoregion.h).
 *
 * This is the half of auto-targeting that can be WRONG in a way no live test
 * would catch: the sampler that feeds it is AMD-IBS hardware and self-skips on
 * every other host (including every GitHub runner), so a rule verified only
 * end-to-end would be verified nowhere most of the time. The ranking is pure, so
 * it is covered HERE, on any host, with hand-built edges — and the live smoke is
 * then only responsible for the wiring.
 *
 * The cases are chosen so each one FAILS for a distinct reason. Every one of them
 * is a way the picker returns a plausible-looking answer that hangs the producer
 * or traces the wrong code, rather than erroring.
 */
#include "asmspy.h"
#include "asmspy_autoregion.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            printf("not ok - %s\n", (msg));                                    \
            failures++;                                                        \
        } else                                                                 \
            printf("ok - %s\n", (msg));                                        \
    } while (0)

/* A hand-built symbol table. Static buffers so no free() is ever needed —
 * the same trick test_symtab.c uses to keep the table hand-writable. */
typedef struct {
    uint64_t start, size;
    const char *name, *module;
} tsym;

static tsym g_syms[] = {
    {0x1000, 0x40, "leaf_hot", "victim"},     /* sized, in the exe        */
    {0x2000, 0x80, "mid_warm", "victim"},     /* sized, in the exe        */
    {0x3000, 0x20, "outer_cold", "victim"},   /* sized, in the exe        */
    {0x4000, 0x00, "unsized_stub", "victim"}, /* SIZE 0 — the trap      */
    {0x5000, 0x30, "memcpy", "libc.so.6"},    /* sized, a LIBRARY         */
};

/* Mirrors asmspy_symtab_at's real semantics, INCLUDING the one that matters:
 * a zero-size symbol resolves ONLY at its exact start (asmspy_proc.c's sym_at).
 * Getting this wrong in the fake would hide the very trap the header guards. */
static int t_resolve(void *ctx, uint64_t addr, uint64_t *start, uint64_t *size,
                     const char **name, const char **module) {
    (void)ctx;
    for (size_t i = 0; i < sizeof g_syms / sizeof g_syms[0]; i++) {
        const tsym *s = &g_syms[i];
        int hit = s->size ? (addr >= s->start && addr < s->start + s->size)
                          : (addr == s->start);
        if (hit) {
            *start = s->start;
            *size = s->size;
            *name = s->name;
            *module = s->module;
            return 0;
        }
    }
    return -1;
}

static asmspy_sample_edge_t mk(uint64_t from, uint64_t to, unsigned long long c,
                               unsigned is_ret) {
    asmspy_sample_edge_t e;
    memset(&e, 0, sizeof e);
    e.from_addr = from;
    e.to_addr = to;
    e.count = c;
    e.is_return = is_ret;
    return e;
}

int main(void) {
    asmspy_autocand_t out[8];

    /* ---- 1. The core rule, on the MEASURED shape ------------------------
     * From the live Zen 5 capture: leaf_hot's entry dominates, mid_warm is
     * entered from TWO call sites, outer_cold from one. The mid-function
     * landings (a return, a loop back-edge) must not count as arrivals. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x2017, 0x1000, 3903, 0), /* mid_warm+0x17 -> leaf_hot ENTRY  */
            mk(0x3015, 0x2000, 249, 0),  /* outer_cold+0x15 -> mid_warm site1 */
            mk(0x3009, 0x2000, 238, 0),  /* outer_cold+0x9  -> mid_warm site2 */
            mk(0x9036, 0x3000, 237, 0),  /* main+0x36 -> outer_cold ENTRY    */
            mk(0x1011, 0x201c, 3744, 1), /* leaf_hot -> mid_warm+0x1c RETURN */
            mk(0x2023, 0x2014, 3377, 0), /* mid_warm back-edge (mid-function) */
        };
        size_t n = asmspy_autoregion_rank(e, 6, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 3,
              "rank: exactly 3 entry candidates (return + back-edge excluded)");
        CHECK(n >= 1 && out[0].addr == 0x1000 && out[0].arrivals == 3903,
              "rank: leaf_hot wins on arrivals (3903)");
        CHECK(n >= 2 && out[1].addr == 0x2000 && out[1].arrivals == 487,
              "rank: mid_warm SUMS its two call sites (249+238=487)");
        CHECK(n >= 2 && out[1].sites == 2,
              "rank: mid_warm credits 2 call sites");
        CHECK(n >= 3 && out[2].addr == 0x3000 && out[2].arrivals == 237,
              "rank: outer_cold third (237)");
        /* The ratios the live capture showed, reproduced by the ranker. */
        CHECK(out[0].arrivals / out[1].arrivals == 8,
              "rank: leaf_hot/mid_warm == 8x (the real loop trip count)");
    }

    /* ---- 2. A RETURN must never be an arrival --------------------------
     * A return lands mid-function, so it cannot be an entry — but is_return is
     * the cheap discriminator and it must actually be honoured. If it were not,
     * the highest-count edge in a real capture is often a return. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x9999, 0x1000, 10, 1), /* a "return" that lands ON an entry */
        };
        size_t n = asmspy_autoregion_rank(e, 1, t_resolve, NULL, NULL, out, 8);
        CHECK(
            n == 0,
            "return: an is_return edge is never an arrival, even at an entry");
    }

    /* ---- 3. Mid-function landings are not entries ----------------------- */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x9999, 0x1004, 5000, 0), /* into leaf_hot's BODY, huge count */
        };
        size_t n = asmspy_autoregion_rank(e, 1, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 0, "entry test: a mid-function landing is not an arrival");
    }

    /* ---- 4. THE VACUITY TRAP: zero-size symbols -------------------------
     * sym_at resolves an unsized symbol ONLY at its start, so `to == start` is
     * true BY CONSTRUCTION for it. Without the size>0 rule this candidate looks
     * like a perfect stream of arrivals and wins outright. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x9999, 0x4000, 99999, 0), /* unsized_stub entry, huge count */
            mk(0x9999, 0x1000, 1, 0),     /* leaf_hot entry, tiny count     */
        };
        size_t n = asmspy_autoregion_rank(e, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 1 && out[0].addr == 0x1000,
              "zero-size: an unsized symbol is dropped despite the highest "
              "count");
    }

    /* ---- 5. Unresolved addresses --------------------------------------- */
    {
        asmspy_sample_edge_t e[] = {mk(0x9999, 0xdead0000, 500, 0)};
        size_t n = asmspy_autoregion_rank(e, 1, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 0,
              "unresolved: an address no symbol covers is not a candidate");
    }

    /* ---- 6. The module filter -----------------------------------------
     * A raw hot endpoint lands in libc far more often than in the operator's
     * code. Without a filter, auto-targeting reliably picks memcpy. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x9999, 0x5000, 9000, 0), /* memcpy [libc] — the usual winner */
            mk(0x9999, 0x1000, 10, 0),   /* leaf_hot [victim]                */
        };
        size_t n = asmspy_autoregion_rank(e, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 2 && out[0].addr == 0x5000,
              "no filter: libc's memcpy legitimately outranks the app (the "
              "problem)");

        n = asmspy_autoregion_rank(e, 2, t_resolve, NULL, "victim", out, 8);
        CHECK(n == 1 && out[0].addr == 0x1000,
              "module filter: --module=victim excludes libc's memcpy");

        n = asmspy_autoregion_rank(e, 2, t_resolve, NULL, "libc", out, 8);
        CHECK(n == 1 && out[0].addr == 0x5000,
              "module filter: --module=libc keeps only memcpy (filter really "
              "filters)");
    }

    /* ---- 7. Ties break by address, never by input order -----------------
     * A statistical sampler emits edges in an order that is a coin flip; a tie
     * broken by that order makes the pick unreproducible on identical behaviour. */
    {
        asmspy_sample_edge_t e1[] = {mk(0x9999, 0x2000, 100, 0),
                                     mk(0x9999, 0x1000, 100, 0)};
        asmspy_sample_edge_t e2[] = {mk(0x9999, 0x1000, 100, 0),
                                     mk(0x9999, 0x2000, 100, 0)};
        size_t a = asmspy_autoregion_rank(e1, 2, t_resolve, NULL, NULL, out, 8);
        uint64_t first_a = out[0].addr;
        size_t b = asmspy_autoregion_rank(e2, 2, t_resolve, NULL, NULL, out, 8);
        uint64_t first_b = out[0].addr;
        CHECK(
            a == 2 && b == 2 && first_a == 0x1000 && first_b == 0x1000,
            "ties: equal arrivals break by ADDRESS, independent of edge order");
    }

    /* ---- 8. Degenerate inputs ------------------------------------------ */
    {
        asmspy_sample_edge_t e[] = {mk(0x9999, 0x1000, 1, 0)};
        CHECK(asmspy_autoregion_rank(NULL, 0, t_resolve, NULL, NULL, out, 8) ==
                  0,
              "degenerate: NULL edges yields nothing");
        CHECK(asmspy_autoregion_rank(e, 0, t_resolve, NULL, NULL, out, 8) == 0,
              "degenerate: zero edges yields nothing (the idle-target case)");
        CHECK(asmspy_autoregion_rank(e, 1, t_resolve, NULL, NULL, out, 0) == 0,
              "degenerate: zero out_cap yields nothing");
        CHECK(asmspy_autoregion_rank(e, 1, NULL, NULL, NULL, out, 8) == 0,
              "degenerate: NULL resolver yields nothing");
    }

    /* ---- 9. out_cap is respected -------------------------------------- */
    {
        asmspy_sample_edge_t e[] = {mk(0x9999, 0x1000, 30, 0),
                                    mk(0x9999, 0x2000, 20, 0),
                                    mk(0x9999, 0x3000, 10, 0)};
        size_t n = asmspy_autoregion_rank(e, 3, t_resolve, NULL, NULL, out, 2);
        CHECK(n == 2, "out_cap: never writes more than out_cap candidates");
    }

    /* ---- 10. A full fold table drops candidates; sizing by edge count is
     * the fix, and it is the CALLER's job (auto_pick sizes by the window's
     * edge count) --------------------------------------------------------
     * Accumulation is streaming: mid_warm's FIRST edge loses to leaf_hot, but
     * its SUM wins. With out_cap=1 the fold never admits it — the "a
     * lower-ranked candidate cannot win anyway" fallacy this test pins down —
     * while out_cap >= the edge count restores the true winner. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x9990, 0x1000, 5, 0), /* leaf_hot: one site, 5 arrivals    */
            mk(0x9991, 0x2000, 3, 0), /* mid_warm site 1: 3                */
            mk(0x9992, 0x2000, 4, 0), /* mid_warm site 2: 4 — sum 7 wins */
        };
        size_t n = asmspy_autoregion_rank(e, 3, t_resolve, NULL, NULL, out, 1);
        CHECK(n == 1 && out[0].addr == 0x1000,
              "cap-full: out_cap=1 drops mid_warm and leaf_hot wins on a "
              "technicality (the documented degradation)");
        n = asmspy_autoregion_rank(e, 3, t_resolve, NULL, NULL, out, 3);
        CHECK(n == 2 && out[0].addr == 0x2000 && out[0].arrivals == 7,
              "cap-full: out_cap >= edge count restores the true winner "
              "(mid_warm, 3+4=7)");
    }

    /* ==== The PORTABLE rule: asmspy_autoregion_rank_ip ==================
     * Residency ranking over software-clock IP buckets. The cases mirror the
     * entry rule's where the semantics coincide and DIVERGE where they must —
     * ending with the disagreement that defines the hazard. */

    /* ---- 11. Containment attribution, summing, sites, order ------------ */
    {
        asmspy_ip_hit_t h[] = {
            {0x2010, 40}, /* mid_warm, offset 0x10  */
            {0x2030, 30}, /* mid_warm, offset 0x30 — sums to 70, 2 offsets */
            {0x1005, 50}, /* leaf_hot, one offset                          */
            {0x0900, 25}, /* resolves to NOTHING: dropped                  */
        };
        size_t n =
            asmspy_autoregion_rank_ip(h, 4, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 2, "rank_ip: 2 candidates (the unresolvable ip dropped)");
        CHECK(n >= 1 && out[0].addr == 0x2000 && out[0].arrivals == 70,
              "rank_ip: mid_warm wins on SUMMED residency (40+30=70) — "
              "mid-body ips count, unlike the entry rule");
        CHECK(n >= 1 && out[0].sites == 2,
              "rank_ip: sites counts DISTINCT sampled offsets (2)");
        CHECK(n >= 2 && out[1].addr == 0x1000 && out[1].arrivals == 50,
              "rank_ip: leaf_hot second (50)");
    }

    /* ---- 12. The zero-size trap holds here too -------------------------- */
    {
        asmspy_ip_hit_t h[] = {
            {0x4000, 99}, /* unsized_stub's exact start — resolvable! */
            {0x1000, 1},  /* leaf_hot entry, 1 sample                 */
        };
        size_t n =
            asmspy_autoregion_rank_ip(h, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 1 && out[0].addr == 0x1000,
              "rank_ip: an unsized symbol cannot win on its exact-start "
              "resolution technicality (same vacuity rule as rank)");
    }

    /* ---- 13. Module filter, same substring semantics -------------------- */
    {
        asmspy_ip_hit_t h[] = {
            {0x5010, 90}, /* memcpy [libc.so.6]  */
            {0x1010, 10}, /* leaf_hot [victim]   */
        };
        size_t n =
            asmspy_autoregion_rank_ip(h, 2, t_resolve, NULL, "victim", out, 8);
        CHECK(n == 1 && out[0].addr == 0x1000,
              "rank_ip: --module=victim excludes the hotter libc symbol");
    }

    /* ---- 14. Determinism: ties break by ascending address --------------- */
    {
        asmspy_ip_hit_t h[] = {
            {0x3008, 5}, /* outer_cold */
            {0x1008, 5}, /* leaf_hot — equal residency */
        };
        size_t n =
            asmspy_autoregion_rank_ip(h, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(n == 2 && out[0].addr == 0x1000 && out[1].addr == 0x3000,
              "rank_ip: equal counts order by ascending address, not input "
              "order (a statistical sampler's input order is a coin flip)");
    }

    /* ---- 15. THE DISAGREEMENT — the hazard as a tested contract ---------
     * auto_victim's shape, hand-built: grind (mid_warm here) burns its own
     * cycles and calls leaf_hot from the loop. The ENTRY rule must pick the
     * callee (its entry is arrived at constantly); the RESIDENCY rule must
     * pick the griper whose entry breakpoint can never fire again. BOTH
     * pickers seeing the same behaviour and answering differently is not a
     * bug in either — it is why the sw path returns a candidate LIST and the
     * caller walks it on "not seen entering". If this check ever fails,
     * rank_ip has quietly become an entry ranker (or vice versa) and the
     * candidate walk's reason to exist should be re-examined. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x2020, 0x1000, 500, 0), /* mid_warm's loop -> leaf_hot ENTRY */
            mk(0x2028, 0x2014, 400, 0), /* mid_warm's own back-edge          */
        };
        asmspy_ip_hit_t h[] = {
            {0x2018, 700}, /* time accrues in mid_warm's OWN loop body */
            {0x1004, 300}, /* some in the callee                        */
        };
        size_t ne = asmspy_autoregion_rank(e, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(ne >= 1 && out[0].addr == 0x1000,
              "disagreement: the ENTRY rule picks the hot CALLEE (leaf_hot)");
        size_t ni =
            asmspy_autoregion_rank_ip(h, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(ni >= 1 && out[0].addr == 0x2000,
              "disagreement: the RESIDENCY rule picks the looping CALLER — "
              "the documented hazard the candidate walk exists for");
        CHECK(ni >= 2 && out[1].addr == 0x1000,
              "disagreement: the correct pick is next in the sw ranking, "
              "which is what makes the walk land it");
    }

    /* ==== asmspy_edge_drill: resolve ONE selected hot edge to a region ====
     * Mirrors the entry ranker's resolve callback + vacuity rule, but drill !=
     * rank: it accepts a MID-function to_addr (the operator picked a concrete
     * edge) and tries to_addr THEN from_addr. */
    {
        uint64_t base, size;
        const char *name, *module;

        /* 1. to_addr inside a sized symbol wins, even when from_addr ALSO
         *    resolves to a different sized symbol — the try-order discriminator:
         *    swap the drill to from-first and BOTH of these checks fail. */
        asmspy_sample_edge_t e1 =
            mk(0x2010, 0x1010, 5, 0); /* mid_warm->leaf_hot */
        int rc = asmspy_edge_drill(&e1, t_resolve, NULL, &base, &size, &name,
                                   &module);
        CHECK(rc == 0 && base == 0x1000,
              "edge_drill: to_addr's containing function wins (leaf_hot)");
        CHECK(rc == 0 && size == 0x40 && strcmp(name, "leaf_hot") == 0,
              "edge_drill: fills size + name from the to_addr symbol");

        /* 2. to_addr unresolved -> the from_addr fallback fires. */
        asmspy_sample_edge_t e2 = mk(0x2010, 0xdead0000, 5, 0);
        rc = asmspy_edge_drill(&e2, t_resolve, NULL, &base, &size, &name,
                               &module);
        CHECK(rc == 0 && base == 0x2000 && strcmp(name, "mid_warm") == 0,
              "edge_drill: unresolved to_addr falls back to from_addr");

        /* 3. THE VACUITY CONTROL: to_addr at a zero-size symbol's start,
         *    from_addr unresolved -> -1 (a zero-size symbol must not win on the
         *    exact-start technicality). Delete the size>0 guard and this fails. */
        asmspy_sample_edge_t e3 =
            mk(0xdead0000, 0x4000, 5, 0); /* to unsized_stub */
        rc = asmspy_edge_drill(&e3, t_resolve, NULL, &base, &size, &name,
                               &module);
        CHECK(
            rc == -1,
            "edge_drill: a zero-size symbol does not qualify (vacuity) -> -1");

        /* 4. neither endpoint resolves -> -1. */
        asmspy_sample_edge_t e4 = mk(0xdead0000, 0xbeef0000, 5, 0);
        rc = asmspy_edge_drill(&e4, t_resolve, NULL, &base, &size, &name,
                               &module);
        CHECK(rc == -1, "edge_drill: neither endpoint resolves -> -1");

        /* 5. drill != rank: a MID-function to_addr (start + 4) still names its
         *    function. The rank rejects this (to != start); the drill takes it. */
        asmspy_sample_edge_t e5 = mk(0x9999, 0x1004, 5, 0); /* leaf_hot + 4 */
        rc = asmspy_edge_drill(&e5, t_resolve, NULL, &base, &size, &name,
                               &module);
        CHECK(rc == 0 && base == 0x1000 && strcmp(name, "leaf_hot") == 0,
              "edge_drill: a mid-function to_addr still names its function "
              "(drill != rank)");
    }

    /* ==== 16. asmspy_autoregion_walk: the candidate walk as a pure decision ===
     * The advance/exhausted/stop policy the two inline walks (cmd_dataflow, the
     * serve SM_AUTO loop) now share. These checks carry the real burden: the walk
     * only ever RUNS on a live AMD/perf host, but its logic is what decides
     * whether --auto recovers a main-shaped mispick or gives up — and it is what
     * the IBS path silently skipped by passing ncand == 0. If you revert either
     * call site to its inline `attempt + 1 < ncand` guard, this stays green only
     * because the expression is the same; delete the size>0-style ncand==0 guard
     * and #5 catches the underflow a `ncand - 1` rewrite would introduce. */
    {
        /* not seen entering, candidates remain -> ADVANCE */
        CHECK(asmspy_autoregion_walk(1, 0, 3) == ASMSPY_WALK_ADVANCE,
              "walk: NEVER_RAN with candidates left -> advance");
        CHECK(asmspy_autoregion_walk(1, 1, 3) == ASMSPY_WALK_ADVANCE,
              "walk: NEVER_RAN mid-list -> advance to the next");

        /* any other outcome is final, no matter how many candidates remain */
        CHECK(asmspy_autoregion_walk(0, 0, 3) == ASMSPY_WALK_STOP,
              "walk: a captured (not-NEVER_RAN) outcome -> stop, even with "
              "candidates left");

        /* not seen entering, none left -> EXHAUSTED (a distinct end, reported
         * as 'every candidate tried', never as a bare 'never ran') */
        CHECK(asmspy_autoregion_walk(1, 2, 3) == ASMSPY_WALK_EXHAUSTED,
              "walk: NEVER_RAN on the last candidate -> exhausted");
        CHECK(strstr(asmspy_autoregion_walk_reason(ASMSPY_WALK_EXHAUSTED),
                     "every ranked candidate") != NULL,
              "walk: the exhausted reason says the list was exhausted, not "
              "'never ran'");

        /* a single-candidate list never advances (there is nowhere to go) */
        CHECK(asmspy_autoregion_walk(1, 0, 1) == ASMSPY_WALK_EXHAUSTED,
              "walk: a single-candidate list never advances -> exhausted");

        /* ncand == 0 is a LEGAL 'no list' input: `attempt + 1 < ncand` is
         * `1 < 0` (false) — no advance, no underflow. This is the exact
         * expression that hid the IBS gap (the IBS path returned one pick and
         * left ncand == 0, so its walk never fired). */
        CHECK(asmspy_autoregion_walk(1, 0, 0) == ASMSPY_WALK_EXHAUSTED,
              "walk: ncand==0 (no list) -> exhausted, no underflow (the IBS-gap "
              "expression)");
        CHECK(asmspy_autoregion_walk(0, 0, 0) == ASMSPY_WALK_STOP,
              "walk: ncand==0 with a captured outcome -> stop");
    }

    /* ==== 17. rank + walk COMPOSED — the recovery 39 T2 gave the IBS path =====
     * The entry rule hands back a RANKED list, and the caller walks it on
     * NEVER_RAN. Before T2 the IBS/entry path returned only cands[0] (ncand==0),
     * so this recovery could not run on the STRONG sampler at all. Here two
     * functions are both entered (leaf_hot more than mid_warm); rank orders them,
     * and if the top pick is not seen entering (a phase change between the sample
     * window and the attach) the walk advances to the second — landing a real
     * region instead of giving up. This is exactly what the adapter now enables;
     * revert auto_pick to a single pick and the ncand it produces is 0, so the
     * walk (asmspy_autoregion_walk) returns EXHAUSTED on the first refusal. */
    {
        asmspy_sample_edge_t e[] = {
            mk(0x3000, 0x1000, 900, 0), /* -> leaf_hot ENTRY, many arrivals   */
            mk(0x3008, 0x2000, 100, 0), /* -> mid_warm ENTRY, fewer arrivals  */
        };
        size_t nc = asmspy_autoregion_rank(e, 2, t_resolve, NULL, NULL, out, 8);
        CHECK(nc == 2 && out[0].addr == 0x1000 && out[1].addr == 0x2000,
              "T2: the entry ranker returns a 2-deep ORDERED list (leaf_hot, "
              "then mid_warm) — the list the IBS walk now has to work with");
        /* top pick not seen entering, one candidate left -> advance to it */
        CHECK(asmspy_autoregion_walk(/*never_ran=*/1, /*attempt=*/0, (int)nc) ==
                  ASMSPY_WALK_ADVANCE,
              "T2: NEVER_RAN on the top entry pick walks to the 2nd (the "
              "recovery the IBS path lacked when it returned a single pick)");
        /* the walked-to candidate is the second-ranked region, ready to arm */
        CHECK(out[1].addr == 0x2000 && out[1].size == 0x80,
              "T2: the 2nd candidate carries a real (base,len) to arm next");
    }

    /* ==== 18. the EMPTY-WINDOW RETRY reuses the walk (39 T3) ==================
     * An idle sample window (the sampler ran, nothing qualified) is a retry, not
     * a verdict: the picker re-samples up to a bound before reporting NEVER_RAN.
     * The decision is the SAME pure walk — "empty window" is the advance
     * condition, the window try is `attempt`, and the retry bound is `ncand`. So
     * the retry loop cannot spin forever (it EXHAUSTS) and cannot give up early
     * (it ADVANCEs while windows remain). AUTO_WINDOW_TRIES is 3 in asmspy.c; the
     * literal here mirrors it. */
    {
        const int tries = 3; /* == AUTO_WINDOW_TRIES */
        CHECK(asmspy_autoregion_walk(/*empty=*/1, 0, tries) ==
                  ASMSPY_WALK_ADVANCE,
              "T3: an empty first window re-samples (advance), not a verdict");
        CHECK(asmspy_autoregion_walk(1, 1, tries) == ASMSPY_WALK_ADVANCE,
              "T3: still re-sampling while windows remain");
        CHECK(asmspy_autoregion_walk(1, tries - 1, tries) ==
                  ASMSPY_WALK_EXHAUSTED,
              "T3: the last empty window stops (exhausted) — a bounded retry, "
              "never an unbounded spin");
        CHECK(
            asmspy_autoregion_walk(/*empty=*/0, 0, tries) == ASMSPY_WALK_STOP,
            "T3: a window that qualified a region stops the retry immediately");
    }

    /* 2026-08-06 plan, Task 5: the chain advances on REFUSAL.
     *
     * The old rule picked a sampler from whether its SUBSTRATE existed
     * (asmtest_ibs_available(), a CPUID + sysfs probe) and stopped on the first
     * refusal. On an AMD box with perf_event_paranoid=4 that means IBS is
     * chosen, refused, and the run ends -- with a working software path, and
     * later a working ptrace path, never tried. */
    CHECK(asmspy_autoregion_sampler_next(ASMSPY_SMP_IBS, 1) == ASMSPY_SMP_SW,
          "a refused IBS falls through to the software sampler");
    CHECK(asmspy_autoregion_sampler_next(ASMSPY_SMP_SW, 1) == ASMSPY_SMP_PTRACE,
          "a refused software sampler falls through to the ptrace sampler");
    CHECK(asmspy_autoregion_sampler_next(ASMSPY_SMP_PTRACE, 1) ==
              ASMSPY_SMP_NONE,
          "the ptrace sampler is last: nothing is left to fall through to");
    /* No IBS substrate: the chain must SKIP it rather than try and fail, so a
     * non-AMD host does not pay a refusal to learn what CPUID already said. */
    CHECK(asmspy_autoregion_sampler_next(ASMSPY_SMP_NONE, 0) == ASMSPY_SMP_SW,
          "with no IBS substrate the chain starts at the software sampler");
    CHECK(asmspy_autoregion_sampler_next(ASMSPY_SMP_NONE, 1) == ASMSPY_SMP_IBS,
          "with an IBS substrate the chain still PREFERS it -- entry evidence "
          "is stronger than residency, and this task changes the fallback, "
          "not the preference");

    /* 2026-08-06 plan, Task 5 fix (coordinator review): asmspy_autoregion_chain_advance
     * pins WHEN the chain walks, separate from asmspy_autoregion_sampler_next's
     * WHICH. Both call sites used to inline `ncand >= 0 || !chain_auto` as
     * their break condition -- a single `>=` -> `>` typo (or a re-widened
     * window-loop guard) would silently let an EMPTY window advance the
     * chain, and every check above would stay green, since they only cover
     * ORDER, never WHEN. */
    CHECK(asmspy_autoregion_chain_advance(-1, 1) != 0,
          "chain_advance: a self-skip (ncand < 0) advances when AUTO");
    CHECK(asmspy_autoregion_chain_advance(0, 1) == 0,
          "chain_advance: an empty window (ncand == 0) does NOT advance -- "
          "the window loop retries the SAME sampler instead");
    CHECK(asmspy_autoregion_chain_advance(1, 1) == 0,
          "chain_advance: a real pick (ncand > 0) does NOT advance -- "
          "nothing is left to do");
    CHECK(asmspy_autoregion_chain_advance(-1, 0) == 0,
          "chain_advance: an explicit --sampler=ibs/sw never advances, even "
          "on a self-skip");
    CHECK(asmspy_autoregion_chain_advance(0, 0) == 0,
          "chain_advance: explicit sampler, empty window -> never advances");
    CHECK(asmspy_autoregion_chain_advance(1, 0) == 0,
          "chain_advance: explicit sampler, real pick -> never advances");

    printf("1..%d\n", checks);
    if (failures) {
        printf("# %d/%d FAILED\n", failures, checks);
        return 1;
    }
    printf("# all %d checks passed\n", checks);
    return 0;
}
