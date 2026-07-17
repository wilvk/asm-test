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

    printf("1..%d\n", checks);
    if (failures) {
        printf("# %d/%d FAILED\n", failures, checks);
        return 1;
    }
    printf("# all %d checks passed\n", checks);
    return 0;
}
