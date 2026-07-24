// lineage.cpp — the pure selection/biography/audit engine of lineage.h.
#include "loom/lineage.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>

namespace asmdesk {

const char *const kLoomAuditTitle =
    "zeroization audit — bounded to the traced window";
const char *const kLoomAuditHover =
    "'clear' means *not overwritten within the traced window* — untraced state "
    "and post-trace writes are invisible";

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// Adjacency, built once per selection. The relation is exactly the one
// src/dataflow.c's slicer walks — this form only trades its O(V*E) rescan for
// an index, which is what makes a whole-run fabric selectable at interactive
// speed.
struct Adj {
    std::map<uint32_t, std::vector<uint32_t>> fwd, bwd;
    Adj(const asmtest_defuse_edge_t *e, size_t n) {
        for (size_t i = 0; i < n; i++) {
            fwd[e[i].from_step].push_back(e[i].to_step);
            bwd[e[i].to_step].push_back(e[i].from_step);
        }
    }
};

// BFS from `origin`, recording depth. `sign` is +1 walking consumers, -1
// walking producers. Depths already present are never overwritten: the FIRST
// time BFS reaches a step is its generation.
void walk(const std::map<uint32_t, std::vector<uint32_t>> &adj, uint32_t origin,
          int sign, uint32_t nsteps, std::map<uint32_t, int> &depth) {
    std::deque<uint32_t> q;
    q.push_back(origin);
    while (!q.empty()) {
        uint32_t s = q.front();
        q.pop_front();
        auto it = adj.find(s);
        if (it == adj.end())
            continue;
        // Deterministic order: the edge array's order is a producer detail.
        std::vector<uint32_t> next = it->second;
        std::sort(next.begin(), next.end());
        next.erase(std::unique(next.begin(), next.end()), next.end());
        for (uint32_t t : next) {
            if (t >= nsteps || depth.count(t) != 0)
                continue;
            depth[t] = depth[s] + sign;
            q.push_back(t);
        }
    }
}

// Is there a memory READ at this step, on this fabric? (A register span defined
// by such a step was born of a LOAD.)
bool step_loads(const loom_fabric_t &f, uint32_t step) {
    // The fabric keeps no record array, so "did this step read memory?" is
    // answered from the spans/hops it does keep: a hop into `step` whose
    // producing span lives on a memory band IS a load.
    for (const loom_hop_t &h : f.hops)
        if (h.to_span != kLoomNoSpan && f.spans[h.to_span].t_write == step &&
            f.lanes[f.spans[h.from_span].lane].kind == loom_lane_kind::mem_band)
            return true;
    return false;
}

const std::vector<uint32_t> &sysv_args() {
    static const std::vector<uint32_t> a = {39, 43, 40, 38, 106, 107};
    return a;
}

} // namespace

bool loom_select(const loom_fabric_t &f, const asmtest_defuse_edge_t *e,
                 size_t n, uint32_t lane, uint32_t step,
                 loom_selection_t *out) {
    if (out == nullptr || lane >= f.lanes.size())
        return false;
    *out = loom_selection_t{};
    uint32_t span = f.resident(lane, step);
    if (span == kLoomNoSpan)
        return false;
    out->origin_span = span;
    out->origin_step = f.spans[span].t_write;

    if (f.spans[span].born_untraced) {
        out->born_untraced = true;
        out->note = "born of untraced state — this worldline has no producer "
                    "inside the recorded window, so it has no ancestry to walk";
        return true;
    }

    // Each direction gets its OWN visited set. Sharing one would let a step
    // reached as an ancestor block the descendant walk from expanding through
    // it — which silently drops part of the closure, and drops exactly the
    // steps a cyclic def-use graph (any loop) makes reachable both ways.
    // test_loom_parity pins the union against src/dataflow.c's slicer.
    Adj adj(e, n);
    std::map<uint32_t, int> back, fwd;
    back[out->origin_step] = 0;
    fwd[out->origin_step] = 0;
    walk(adj.bwd, out->origin_step, -1, f.steps, back);
    walk(adj.fwd, out->origin_step, +1, f.steps, fwd);

    std::map<uint32_t, int> depth = back;
    for (const auto &kv : fwd) {
        auto it = depth.find(kv.first);
        // Reachable both ways: the NEARER ring wins, ancestors on a tie. The
        // rule only has to be deterministic — a step's generation is "how far
        // the walk had to go", and both answers are true.
        if (it == depth.end() || std::abs(kv.second) < std::abs(it->second))
            depth[kv.first] = kv.second;
    }

    for (const auto &kv : depth) {
        out->steps.push_back(kv.first);
        out->generation.push_back(kv.second);
        out->gen_lo = std::min(out->gen_lo, kv.second);
        out->gen_hi = std::max(out->gen_hi, kv.second);
    }
    return true;
}

std::vector<uint32_t> loom_generation(const loom_selection_t &sel, int gen) {
    std::vector<uint32_t> v;
    for (size_t i = 0; i < sel.steps.size(); i++)
        if (sel.generation[i] == gen)
            v.push_back(sel.steps[i]);
    return v;
}

void loom_gen_step(loom_selection_t &sel, int delta) {
    sel.gen_view =
        std::max(sel.gen_lo, std::min(sel.gen_hi, sel.gen_view + delta));
}

// ---------------------------------------------------------------------------
// Biography
// ---------------------------------------------------------------------------

std::vector<loom_bio_row_t> loom_biography(const loom_fabric_t &f,
                                           const asmtest_defuse_edge_t *e,
                                           size_t n, uint32_t span) {
    std::vector<loom_bio_row_t> rows;
    if (span >= f.spans.size())
        return rows;
    const loom_span_t &s = f.spans[span];
    const loom_lane_t &lane = f.lanes[s.lane];

    auto at = [&](uint32_t step) -> std::string {
        if (step < f.prov.disasm.size() && !f.prov.disasm[step].empty())
            return f.prov.disasm[step];
        // D10: no recorded disassembly. The step index is a fact; an invented
        // mnemonic would not be.
        return "step " + std::to_string(step) + " (no recorded disassembly)";
    };
    auto value = [&](const loom_span_t &x) {
        return x.value_valid ? hex(x.value)
                             : std::string("(value never captured)");
    };

    // --- birth --------------------------------------------------------------
    std::string birth;
    if (s.born_untraced) {
        bool is_arg = lane.kind == loom_lane_kind::reg &&
                      std::find(sysv_args().begin(), sysv_args().end(),
                                lane.reg) != sysv_args().end();
        birth = is_arg
                    ? "entry-seeded argument in " + lane.name + " = " + value(s)
                    : "born of untraced state on " + lane.name +
                          " — provenance starts at instrumentation";
    } else if (lane.kind == loom_lane_kind::mem_band) {
        birth = "store to " + lane.name + " at " + at(s.t_write) + ", " +
                std::to_string(s.hi - s.lo) + " bytes at " + hex(s.lo) + " = " +
                value(s);
    } else if (step_loads(f, s.t_write)) {
        birth = "load into " + lane.name + " at " + at(s.t_write) + " = " +
                value(s);
    } else if (f.is_knot(s.t_write)) {
        birth = "ALU write to " + lane.name + " at " + at(s.t_write) +
                " [knot: this step both reads and writes] = " + value(s);
    } else {
        birth =
            "write to " + lane.name + " at " + at(s.t_write) + " = " + value(s);
    }
    rows.push_back({"birth", birth});

    // --- hops ---------------------------------------------------------------
    for (const loom_hop_t &h : f.hops) {
        if (h.from_span != span)
            continue;
        const asmtest_defuse_edge_t &edge = e[h.edge];
        std::string to = h.to_span == kLoomNoSpan
                             ? std::string("(read, defines nothing)")
                             : f.lanes[f.spans[h.to_span].lane].name;
        rows.push_back({"hop", "read at " + at(edge.to_step) + " -> " + to});
    }

    // --- escapes ------------------------------------------------------------
    // A consumer that writes a memory band is where a value leaves the register
    // file — the single most useful thing to know about a secret.
    for (const loom_hop_t &h : f.hops) {
        if (h.from_span != span || h.to_span == kLoomNoSpan)
            continue;
        const loom_span_t &t = f.spans[h.to_span];
        if (f.lanes[t.lane].kind == loom_lane_kind::mem_band)
            rows.push_back({"escape", "escapes to memory at step " +
                                          std::to_string(t.t_write) + " (" +
                                          f.lanes[t.lane].name + ")"});
    }

    // --- provenance ---------------------------------------------------------
    rows.push_back(
        {"producer",
         "producer: " +
             (f.prov.producer.empty() ? std::string("(unnamed)")
                                      : f.prov.producer) +
             (f.prov.exact ? " — exact, replay" : " — statistical") +
             (f.prov.isolated_guest ? "; isolated guest (emulator replay, "
                                      "not silicon)"
                                    : "")});

    rows.push_back(
        {"window",
         f.prov.truncated
             ? "the recorded window is TRUNCATED: everything above is a lower "
               "bound, and a later write may exist that was never recorded"
             : "bounded to the " + std::to_string(f.steps) +
                   " recorded steps: nothing here says what happened before or "
                   "after them"});
    return rows;
}

// ---------------------------------------------------------------------------
// Zeroization audit
// ---------------------------------------------------------------------------

size_t loom_audit(const loom_fabric_t &f, const asmtest_defuse_edge_t *e,
                  size_t n, uint32_t birth_step, uint32_t playhead,
                  std::vector<loom_lit_t> *lit) {
    if (lit == nullptr)
        return 0;
    lit->clear();

    Adj adj(e, n);
    std::map<uint32_t, int> depth;
    depth[birth_step] = 0;
    walk(adj.fwd, birth_step, +1, f.steps, depth);

    for (uint32_t lane = 0; lane < f.lanes.size(); lane++) {
        uint32_t span = f.resident(lane, playhead);
        if (span == kLoomNoSpan)
            continue;
        const loom_span_t &s = f.spans[span];
        // A born-of-untraced-state resident is never lit: the fabric does not
        // know where it came from, and "descends from the secret" is a claim
        // about provenance.
        if (s.born_untraced || depth.count(s.t_write) == 0)
            continue;
        loom_lit_t row;
        row.lane = lane;
        row.span = span;
        row.hollow = !s.value_valid;
        lit->push_back(row);
    }
    return lit->size();
}

size_t loom_audit_lanes(const loom_fabric_t &f, const asmtest_defuse_edge_t *e,
                        size_t n, uint32_t birth_step, uint32_t playhead,
                        std::vector<uint32_t> *lit_lanes) {
    if (lit_lanes == nullptr)
        return 0;
    std::vector<loom_lit_t> rows;
    loom_audit(f, e, n, birth_step, playhead, &rows);
    lit_lanes->clear();
    for (const loom_lit_t &r : rows)
        lit_lanes->push_back(r.lane);
    return lit_lanes->size();
}

} // namespace asmdesk
