// test_find.cpp — global find as search-as-measurement
// (docs/internal/gui/22-selection-and-search.md T3, F17). Null backend, model
// state (D4).
//
// Asserts: a query reports the match COUNT and the aggregate COST the model
// computes (summed hot-edge samples — the measurement payoff); matches are in
// STREAM ORDER, never relevance-ranked; Enter / Shift+Enter step the active index
// and target the right entity via the router; and the call TREE is untouched —
// find produces no tree hits, so no client-side tree narrowing was added and its
// surviving depths never lie (D7).
#include <cstdio>
#include <cstring>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "ui/find.h"
#include "views/hotedges.h"
#include "views/observer_draw.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}
static std::string gd(const char *name) {
    return std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
}
static void set_query(FindState &f, const char *q) {
    std::snprintf(f.query, sizeof f.query, "%s", q);
}

int main() {
    std::string err;
    auto rec = load_recording_file(gd("sum_via_rbx.asmtrace"), err);
    check("load", rec.has_value(), err.c_str());
    if (!rec) {
        std::fprintf(stderr, "%d find check(s) failed\n", failures);
        return 1;
    }
    Streams st = decode_streams(*rec);
    // A default Observer deck (its syscalls / disasm surfaces empty here) with two
    // hand-built hot edges — enough to exercise every find surface without linking
    // the heavy observer_build draw TU. The timeline surface reads `st` directly.
    ObserverState os;

    // Inject two hot edges with distinctive labels no timeline/disasm row carries,
    // so a query over them measures ONLY hot-edge samples — the search-as-
    // measurement payoff ("this symbol retires N samples across M sites").
    HotEdge e1;
    e1.from = "zzhotA";
    e1.to = "zzhotB";
    e1.from_addr = 0x4000;
    e1.count = 7;
    e1.rank = 1;
    HotEdge e2;
    e2.from = "zzhotC";
    e2.to = "zzhotB";
    e2.from_addr = 0x5000;
    e2.count = 13;
    e2.rank = 2;
    os.hotedges.edges = {e1, e2};

    // === the measurement: count + summed cost over hot edges =================
    {
        FindState f;
        set_query(f, "zzhotB"); // matches both edges' `to`
        find_run(f, st, &os);
        check("hotedge/count", f.hits.size() == 2, "both edges must match");
        check("hotedge/cost-sum", f.total_cost == 20,
              "aggregate cost sums the edge samples (7 + 13)");
        bool all_hot = true;
        for (const FindHit &h : f.hits)
            if (h.view != dt_view::hotedges)
                all_hot = false;
        check("hotedge/view", all_hot, "hot-edge hits carry the hotedges view");
    }
    {
        FindState f;
        set_query(f, "zzhotC"); // matches only e2
        find_run(f, st, &os);
        check("hotedge/one", f.hits.size() == 1 && f.total_cost == 13,
              "a single-edge query measures just that edge's samples");
    }

    // === the timeline surface: count, stream order, cost, cycling ============
    // Pick a real token from the recording's own disassembly so the query is not
    // brittle to the corpus.
    dt_timeline tl = dt_timeline_build(st);
    std::string token;
    for (const dt_timeline_row &r : tl.rows)
        if (!r.disasm.empty()) {
            token = r.disasm.substr(0, r.disasm.find(' '));
            break;
        }
    check("timeline/have-token", !token.empty(),
          "the recording must carry some disassembly to search");
    if (!token.empty()) {
        FindState f;
        set_query(f, token.c_str());
        find_run(f, st, &os); // hot-edge labels are zz*, so only timeline matches
        check("timeline/count", !f.hits.empty(), "the token must match rows");
        // Every timeline hit costs 1 step, so the aggregate equals the count.
        check("timeline/cost-is-count", f.total_cost == f.hits.size(),
              "timeline cost is one step per hit");
        // Matches are in STREAM ORDER (ascending step), never relevance-ranked.
        bool ordered = true;
        for (size_t i = 1; i < f.hits.size(); i++)
            if (f.hits[i - 1].step && f.hits[i].step &&
                *f.hits[i].step < *f.hits[i - 1].step)
                ordered = false;
        check("timeline/stream-order", ordered,
              "matches stay in stream order, not relevance-ranked");

        // Enter / Shift+Enter cycle the active index and target the right entity.
        const FindHit *h0 = find_cycle(f, true);
        check("cycle/enter-first", f.active == 0 && h0 != nullptr,
              "Enter selects the first match");
        if (f.hits.size() > 1) {
            find_cycle(f, true);
            check("cycle/enter-next", f.active == 1, "Enter advances");
            find_cycle(f, false);
            check("cycle/shift-enter-back", f.active == 0, "Shift+Enter retreats");
        }
        // Wrap: retreating from the first lands on the last.
        find_cycle(f, false);
        check("cycle/wraps", f.active == static_cast<int>(f.hits.size()) - 1,
              "cycling wraps around the match set");
        // The router link a hit navigates to carries the right view + step.
        if (h0) {
            dt_link l = find_hit_link(*h0, st.id);
            check("cycle/link", l.view == dt_view::timeline && l.rec == st.id &&
                                     l.step.has_value(),
                  "a timeline hit's link targets the timeline at its step");
        }
    }

    // === the call TREE stays engine-filtered — find never narrows it =========
    // Find has no tree surface at all, so no hit can name it; a client-side tree
    // filter would leave surviving depths claiming a parentage the engine never
    // emitted (tree.h, D7). This guards that the tree was left untouched.
    {
        FindState f;
        set_query(f, "zzhotB");
        find_run(f, st, &os);
        bool any_tree = false;
        for (const FindHit &h : f.hits)
            if (h.view == dt_view::tree)
                any_tree = true;
        check("tree/untouched", !any_tree,
              "find produces no tree hits — the tree stays engine-filtered");
    }

    // Empty query -> no hits, no cost (a find that measures nothing measures
    // nothing, truly).
    {
        FindState f;
        set_query(f, "");
        find_run(f, st, &os);
        check("empty/no-hits", f.hits.empty() && f.total_cost == 0,
              "an empty query matches nothing");
    }

    if (failures) {
        std::fprintf(stderr, "%d find check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_find: all checks passed\n");
    return 0;
}
