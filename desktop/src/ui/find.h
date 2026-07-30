// find.h — global find as SEARCH-AS-MEASUREMENT
// (docs/internal/gui/22-selection-and-search.md T3, F17).
//
// Ctrl+F opens a find that highlights EVERY hit (it never hides rows — that is
// the fidelity distinction from a filter, D7), reports the match COUNT and the
// aggregate COST, and cycles matches with Enter / Shift+Enter. "How much does
// mnemonic/address/symbol X occur, and at what cost" is a measurement an expert
// could not answer before without manual scrolling.
//
// The model is PURE — a function of the active recording's decoded streams and
// its Observer deck — so it is unit-tested with no draw (D4). Matches are kept in
// STREAM ORDER per surface (like the syscalls filter, syscalls.h:100-105), never
// relevance-ranked: "these rows matched, in the order they occurred" is a
// different and more truthful claim than "these are the most relevant".
//
// It deliberately does NOT search the call TREE: a client-side match over an
// engine-unfiltered tree would leave surviving depths claiming a parentage the
// engine never emitted (tree.h:1-13, D7). The tree stays engine-filtered.
#ifndef ASMDESK_UI_FIND_H
#define ASMDESK_UI_FIND_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "nav.h"
#include "views/hotedges.h"
#include "views/observer_draw.h" // ObserverState (syscalls / hotedges / disasm)
#include "views/syscalls.h"
#include "views/timeline.h"

namespace asmdesk {

// One hit and the "how much" it contributes. `cost` is a step (timeline / disasm
// / syscall = 1) or a summed sample weight (hot-edge = its `count`), so the total
// is the search-as-measurement payoff ("this mnemonic retires N samples across M
// sites"). The hit carries enough to build a dt_link and drive the ONE spine.
struct FindHit {
    dt_view view = dt_view::timeline;
    std::optional<uint32_t> step;
    std::optional<uint64_t> off;
    uint64_t cost = 0;
    std::string label; // the matched text, for the find bar's list
};

struct FindState {
    char query[96] = {0};
    bool open = false;      // the find bar is showing (Ctrl+F opens it)
    bool focus_query = false; // ask the draw to focus the input this frame
    std::string last_query; // the query `hits` was computed for (recompute guard)
    std::vector<FindHit> hits; // in stream order, never relevance-ranked
    int active = -1;           // the cycled hit; -1 = none yet
    uint64_t total_cost = 0;   // aggregate over every hit (the measurement)
};

namespace detail {

inline std::string find_lower(const std::string &s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return o;
}
inline bool find_contains(const std::string &hay_lower,
                          const std::string &needle_lower) {
    return needle_lower.empty() ||
           hay_lower.find(needle_lower) != std::string::npos;
}
inline std::string find_hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

} // namespace detail

// Compute matches + aggregate cost over the active recording (`a`) and its
// Observer deck (`obs`, may be null). Case-insensitive substring against: the
// timeline rows' disasm / annotation / offset; the disasm listing's recorded
// text / addresses; the syscalls' order-preserving `line` (via the shared
// obs_syscall_filter_indices); and the hot-edge from/to labels (summing `count`).
// The call tree is intentionally absent (D7). Idempotent, and cheap enough to run
// on a query change rather than per frame.
inline void find_run(FindState &f, const Streams &a, const ObserverState *obs) {
    f.hits.clear();
    f.total_cost = 0;
    f.active = -1;
    f.last_query = f.query;
    const std::string q = detail::find_lower(f.query);
    if (q.empty())
        return;

    // Timeline — step order. Reuse the real builder so the searched text is
    // exactly what the pane shows (disasm + the value annotation).
    dt_timeline tl = dt_timeline_build(a);
    for (const dt_timeline_row &r : tl.rows) {
        if (r.missing)
            continue;
        const std::string hay = detail::find_lower(
            r.disasm + " " + r.ann + " " + detail::find_hex(r.off));
        if (detail::find_contains(hay, q)) {
            FindHit h;
            h.view = dt_view::timeline;
            h.step = r.step;
            h.off = r.off;
            h.cost = 1;
            h.label = r.disasm.empty() ? detail::find_hex(r.off) : r.disasm;
            f.hits.push_back(std::move(h));
            f.total_cost += 1;
        }
    }

    if (obs != nullptr) {
        // Disasm — recorded text by ascending offset (a std::map already sorts).
        for (const auto &kv : obs->disasm.recorded_disasm) {
            const std::string hay =
                detail::find_lower(kv.second + " " + detail::find_hex(kv.first));
            if (detail::find_contains(hay, q)) {
                FindHit h;
                h.view = dt_view::disasm;
                h.off = kv.first;
                h.cost = 1;
                h.label = kv.second.empty() ? detail::find_hex(kv.first)
                                            : kv.second;
                f.hits.push_back(std::move(h));
                f.total_cost += 1;
            }
        }

        // Syscalls — reuse the order-preserving filter so a find over the syscall
        // stream and its own type-to-narrow agree on what "matched" means.
        for (size_t i : obs_syscall_filter_indices(obs->syscalls, f.query)) {
            FindHit h;
            h.view = dt_view::syscalls;
            h.off = i; // the row index, a stable locator for the syscall stream
            h.cost = 1;
            h.label = obs->syscalls.rows[i].line;
            f.hits.push_back(std::move(h));
            f.total_cost += 1;
        }

        // Hot edges — rank order; the "how much" is the summed sample count, the
        // measurement this whole surface exists for.
        for (const HotEdge &e : obs->hotedges.edges) {
            const std::string hay = detail::find_lower(e.from + " " + e.to);
            if (detail::find_contains(hay, q)) {
                FindHit h;
                h.view = dt_view::hotedges;
                h.off = e.from_addr;
                h.cost = e.count;
                h.label = e.from + " -> " + e.to;
                f.hits.push_back(std::move(h));
                f.total_cost += e.count;
            }
        }
    }
}

// Advance (Enter) / retreat (Shift+Enter) the active match, wrapping. Returns the
// now-active hit, or nullptr when there are none. Cycling drives NAVIGATION (the
// ONE spine, dt_nav_go) — never the undo stack (T4's boundary).
inline const FindHit *find_cycle(FindState &f, bool forward) {
    if (f.hits.empty()) {
        f.active = -1;
        return nullptr;
    }
    const int n = static_cast<int>(f.hits.size());
    if (f.active < 0)
        f.active = forward ? 0 : n - 1;
    else
        f.active = ((f.active + (forward ? 1 : -1)) % n + n) % n;
    return &f.hits[static_cast<size_t>(f.active)];
}

// The dt_link a hit navigates to (the same textual form a pasted deep link uses).
inline dt_link find_hit_link(const FindHit &h, const std::string &rec) {
    dt_link l;
    l.rec = rec;
    l.view = h.view;
    l.step = h.step;
    // The syscall/hot-edge locators are not code offsets, so only the timeline /
    // disasm surfaces carry `off` into the link (the view switch is enough for the
    // stream surfaces, which have no per-offset spine position).
    if (h.view == dt_view::timeline || h.view == dt_view::disasm)
        l.off = h.off;
    return l;
}

} // namespace asmdesk
#endif // ASMDESK_UI_FIND_H
