// slice_view.cpp — the pure builder + dump of slice_view.h. No ImGui, no I/O.
#include "views/slice_view.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

// clang-format off
extern "C" {
#include "asmtest_valtrace.h"
#include "asmspy_dataview.h"
}
// clang-format on

namespace asmdesk {

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// The carried location's printable name, from the ONE implementation the TUI
// uses (cli/asmspy_dataview.h) rather than a second spelling of "reg#35".
std::string loc_str(const ValRec &l) {
    at_val_rec_t r{};
    r.kind = l.space == "reg"
                 ? AT_LOC_REG
                 : (l.space == "off" ? AT_LOC_MEM_OFF : AT_LOC_MEM_ABS);
    r.reg = l.reg;
    r.addr = l.addr;
    char buf[64];
    asmspy_df_loc_str(&r, buf, sizeof buf);
    return buf;
}

} // namespace

dt_slice_view dt_slice_view_build(const Streams &s,
                                  std::optional<uint32_t> selected) {
    dt_slice_view v;
    v.selected_step = selected;
    v.truncated = s.truncated;

    const DataflowStream &df = s.df;
    if (s.truncated || df.steps_missing > 0) {
        // Cones over a truncated stream are LOWER BOUNDS: an edge that was
        // never recorded cannot be followed, so a cone that looks small may
        // simply be a cone we could not see the rest of.
        v.banner = "cones incomplete: trace truncated — every cone here is a "
                   "LOWER BOUND on what actually produced or consumed the "
                   "value, not the whole story";
        if (df.steps_missing > 0)
            v.banner += " (" + std::to_string(df.steps_missing) +
                        " step(s) carry no df_step event)";
    }

    if (selected && *selected < df.nsteps) {
        v.back = dt_slice_backward(df.edges, df.nsteps, *selected);
        v.fwd = dt_slice_forward(df.edges, df.nsteps, *selected);
    }

    // Nodes: every step that carries an edge endpoint, plus the selection.
    // A step with no dependence at all is not a node — it is not part of any
    // cone and drawing it would only add noise to the layered picture.
    std::set<uint32_t> steps;
    for (const dt_edge &e : df.edges) {
        if (e.from_step < df.nsteps)
            steps.insert(e.from_step);
        if (e.to_step < df.nsteps)
            steps.insert(e.to_step);
    }
    if (selected && *selected < df.nsteps)
        steps.insert(*selected);

    int column = 0;
    for (uint32_t step : steps) {
        dt_slice_node n;
        n.step = step;
        n.off = step < df.insn_off.size() ? df.insn_off[step] : 0;
        const std::string &dis =
            step < df.disasm.size() ? df.disasm[step] : std::string();
        n.label = hex(n.off) + (dis.empty() ? "" : "  " + dis);
        n.column = column++;
        if (!selected) {
            n.style = dt_cone::none;
        } else if (*selected == step) {
            n.style = dt_cone::both;
        } else {
            bool b = v.back.contains(step), f = v.fwd.contains(step);
            n.style = b && f ? dt_cone::both
                             : (b ? dt_cone::back
                                  : (f ? dt_cone::fwd : dt_cone::dimmed));
        }
        v.nodes.push_back(n);
    }

    // Edges, with a deterministic arc lane: sort by (from, to), then give each
    // edge the LOWEST lane free over its whole [from, to] span. Greedy in a
    // fixed order == the same picture for the same recording, every time.
    std::vector<size_t> order(df.edges.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
        const dt_edge &a = df.edges[x], &b = df.edges[y];
        if (a.from_step != b.from_step)
            return a.from_step < b.from_step;
        return a.to_step < b.to_step;
    });
    // lane_busy[lane] = the set of steps that lane's arcs already span.
    std::vector<std::vector<std::pair<uint32_t, uint32_t>>> lanes;
    for (size_t idx : order) {
        const dt_edge &e = df.edges[idx];
        if (e.from_step >= df.nsteps || e.to_step >= df.nsteps)
            continue; // an out-of-range endpoint is not drawable
        uint32_t lo = std::min(e.from_step, e.to_step);
        uint32_t hi = std::max(e.from_step, e.to_step);
        size_t lane = 0;
        for (;; lane++) {
            if (lane == lanes.size()) {
                lanes.emplace_back();
                break;
            }
            bool clash = false;
            for (const auto &span : lanes[lane])
                if (!(hi < span.first || lo > span.second)) {
                    clash = true;
                    break;
                }
            if (!clash)
                break;
        }
        lanes[lane].push_back({lo, hi});
        dt_slice_edge se;
        se.from_step = e.from_step;
        se.to_step = e.to_step;
        se.lane = static_cast<int>(lane);
        se.loc = idx < df.edge_loc.size() ? loc_str(df.edge_loc[idx]) : "";
        v.edges.push_back(se);
    }
    return v;
}

std::optional<uint32_t> dt_slice_view_walk(const dt_slice_view &v,
                                           uint32_t from, bool forward) {
    const dt_slice &cone = forward ? v.fwd : v.back;
    std::optional<uint32_t> best;
    uint64_t best_dist = 0;
    for (uint32_t step : cone.steps) {
        if (forward ? step <= from : step >= from)
            continue;
        uint64_t d = forward ? step - from : from - step;
        if (!best || d < best_dist || (d == best_dist && step < *best)) {
            best = step;
            best_dist = d;
        }
    }
    return best;
}

std::string dt_slice_view_dump(const dt_slice_view &v) {
    static const char *kStyle[] = {"none", "back", "fwd", "both", "dimmed"};
    std::string s;
    if (!v.banner.empty())
        s += "banner=" + v.banner + "\n";
    s += "selected=" +
         (v.selected_step ? std::to_string(*v.selected_step)
                          : std::string("(none)")) +
         "\n";
    s += "nodes=" + std::to_string(v.nodes.size()) +
         " edges=" + std::to_string(v.edges.size()) + "\n";
    for (const dt_slice_node &n : v.nodes)
        s += "  " + std::to_string(n.step) + "@" + std::to_string(n.column) +
             " [" + kStyle[static_cast<int>(n.style)] + "] " + n.label + "\n";
    for (const dt_slice_edge &e : v.edges)
        s += "  " + std::to_string(e.from_step) + "->" +
             std::to_string(e.to_step) + " lane=" + std::to_string(e.lane) +
             " loc=" + e.loc + "\n";
    return s;
}

} // namespace asmdesk
