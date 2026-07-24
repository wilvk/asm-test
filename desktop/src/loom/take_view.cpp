// take_view.cpp — the pure fork-UX model of take_view.h. No engine, no ImGui.
#include "loom/take_view.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

namespace asmdesk {

const char *const kLoomAlignedEndToEnd = "aligned end-to-end";
const char *const kLoomDashedTailHover = "unaligned — never drawn as agreement";

namespace {

std::string hex(uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    return b;
}

// A write's identity across two fabrics. Lane INDICES are per-fabric (only
// touched lanes materialize, so the two decks need not line up); the place a
// write happened is the durable key.
struct WKey {
    int kind;   // 0 reg, 1 mem
    uint64_t a; // reg container id, or the write's low byte
    uint64_t b; // 0, or the write's byte width
    bool operator<(const WKey &o) const {
        return kind != o.kind ? kind < o.kind : a != o.a ? a < o.a : b < o.b;
    }
};

WKey key_of(const loom_fabric_t &f, const loom_span_t &s) {
    const loom_lane_t &l = f.lanes[s.lane];
    if (l.kind == loom_lane_kind::reg)
        return WKey{0, l.reg, 0};
    return WKey{1, s.lo, s.hi - s.lo};
}

// The write records at one step, keyed by place.
std::map<WKey, const loom_span_t *> writes_at(const loom_fabric_t &f,
                                              uint32_t step) {
    std::map<WKey, const loom_span_t *> m;
    for (const loom_span_t &s : f.spans) {
        if (s.born_untraced || s.t_write != step)
            continue;
        m[key_of(f, s)] = &s; // the last write at a step wins, as in the fabric
    }
    return m;
}

// Forward closure over a take's def-use edges from a seed set.
std::set<uint32_t> forward(const asmtest_defuse_edge_t *e, size_t n,
                           uint32_t nsteps, const std::set<uint32_t> &seed) {
    std::map<uint32_t, std::vector<uint32_t>> adj;
    for (size_t i = 0; i < n; i++)
        adj[e[i].from_step].push_back(e[i].to_step);
    std::set<uint32_t> seen;
    std::deque<uint32_t> q;
    for (uint32_t s : seed)
        if (s < nsteps && seen.insert(s).second)
            q.push_back(s);
    while (!q.empty()) {
        uint32_t s = q.front();
        q.pop_front();
        auto it = adj.find(s);
        if (it == adj.end())
            continue;
        for (uint32_t t : it->second)
            if (t < nsteps && seen.insert(t).second)
                q.push_back(t);
    }
    return seen;
}

} // namespace

const char *take_verdict_name(take_verdict v) {
    switch (v) {
    case take_verdict::neutral:
        return "neutral";
    case take_verdict::dim:
        return "dim";
    case take_verdict::hot:
        return "hot";
    }
    return "?";
}

loom_take_view_t
loom_take_view(const loom_fabric_t &base, const loom_fabric_t &take,
               const asmtest_defuse_edge_t *take_edges, size_t n_take_edges,
               const loom_edit_desc_t &edit, const std::string &fault,
               const std::string &err) {
    loom_take_view_t v;
    v.node.label = "(edited fact → new fabric)";
    v.node.edit = edit.label;
    v.node.fault = fault;
    v.node.err = err;
    v.node.disclosure =
        "forks re-run the emulator replay — an explicit crossing of the "
        "native→virtual line; never evidence about a live process or silicon "
        "timing";

    // --- alignment: 04's shared-prefix helper, over the two insn_off arrays --
    v.div = dt_first_divergence(base.insn_off, take.insn_off,
                                base.prov.truncated, take.prov.truncated);
    v.prefix = v.div.diverged
                   ? v.div.step
                   : static_cast<uint32_t>(
                         std::min(base.insn_off.size(), take.insn_off.size()));
    v.node.alignment =
        v.div.diverged
            ? "patient zero at step " + std::to_string(v.div.step) + ": base " +
                  hex(v.div.off_a) + " vs take " + hex(v.div.off_b)
            : std::string(kLoomAlignedEndToEnd);
    if (!v.div.diverged && v.div.bounded)
        v.node.alignment +=
            " (bounded — at least one side was truncated, so agreement past "
            "the recorded window was never observed)";

    // --- the cone of the edited fact ---------------------------------------
    std::set<uint32_t> seed;
    if (edit.code_patch) {
        // Seed at the first ALIGNED step whose instruction bytes differ. The
        // step's own offset is the key: a patch that changed nothing the run
        // executed seeds nothing, and the cone is honestly empty.
        for (uint32_t s = 0; s < v.prefix && s < take.insn_off.size(); s++) {
            uint64_t off = take.insn_off[s];
            if (off == UINT64_MAX || off >= edit.base_code.size() ||
                off >= edit.take_code.size())
                continue;
            // Compare from the instruction's offset to the next step's, which
            // is this instruction's length for a straight-line step.
            uint64_t end = (s + 1 < take.insn_off.size() &&
                            take.insn_off[s + 1] != UINT64_MAX &&
                            take.insn_off[s + 1] > off)
                               ? take.insn_off[s + 1]
                               : off + 1;
            end = std::min<uint64_t>(
                end, std::min(edit.base_code.size(), edit.take_code.size()));
            bool differ = false;
            for (uint64_t b = off; b < end; b++)
                if (edit.base_code[b] != edit.take_code[b])
                    differ = true;
            if (differ) {
                seed.insert(s);
                break;
            }
        }
    } else {
        // Every step that reads the edited argument register with NO in-trace
        // producer edge into that read — i.e. every step that consumed the
        // entry value directly.
        for (const loom_read_t &r : take.reads) {
            if (r.has_producer)
                continue;
            const loom_lane_t &l = take.lanes[r.lane];
            if (l.kind == loom_lane_kind::reg && l.reg == edit.arg_reg)
                seed.insert(r.step);
        }
    }
    std::set<uint32_t> cone =
        forward(take_edges, n_take_edges, take.steps, seed);
    v.cone.assign(cone.begin(), cone.end());

    // --- the three-way verdict, inside prefix ∩ cone -----------------------
    for (uint32_t s = 0; s < base.steps; s++) {
        loom_take_step_t row;
        row.step = s;
        row.unaligned = s >= v.prefix;
        row.in_cone = cone.count(s) != 0;
        if (row.in_cone && !row.unaligned) {
            auto A = writes_at(base, s);
            auto B = writes_at(take, s);
            size_t matched = 0, comparable = 0, equal = 0;
            bool different = false;
            for (const auto &kv : A) {
                auto it = B.find(kv.first);
                if (it == B.end())
                    continue;
                matched++;
                if (!kv.second->value_valid || !it->second->value_valid)
                    continue; // equality of unknown values is never claimed
                comparable++;
                if (kv.second->value == it->second->value)
                    equal++;
                else
                    different = true;
            }
            row.no_writes = matched == 0;
            if (different)
                row.verdict = take_verdict::hot;
            else if (matched > 0 && comparable == matched && equal == matched)
                row.verdict = take_verdict::dim;
            else
                row.verdict = take_verdict::neutral;
        }
        v.steps.push_back(row);
    }
    return v;
}

void loom_take_plan(const loom_take_view_t &v, const loom_view_t &cam,
                    std::vector<loom_prim_t> *out) {
    if (out == nullptr)
        return;
    const double spp = cam.steps_per_px > 0 ? cam.steps_per_px : 1.0;
    auto x_of = [&](double step) {
        return static_cast<float>((step - cam.step0) / spp);
    };
    auto push = [&](loom_prim k, float x0, float y0, float x1, float y1,
                    uint32_t a, std::string text) {
        loom_prim_t p;
        p.kind = k;
        p.x0 = x0;
        p.y0 = y0;
        p.x1 = x1;
        p.y1 = y1;
        p.a = a;
        p.text = std::move(text);
        out->push_back(std::move(p));
    };

    // Patient zero — and ONLY when the offsets actually diverged. An argument
    // edit on straight-line code diverges in values, not offsets; faking a
    // patient zero there would point at an instruction that did nothing wrong.
    if (v.div.diverged) {
        float x = x_of(v.div.step);
        push(
            loom_prim::patient_zero, x, 0, x, cam.px_h, v.div.step,
            "patient zero: base " +
                [&] {
                    char b[32];
                    std::snprintf(b, sizeof b, "0x%llx",
                                  (unsigned long long)v.div.off_a);
                    return std::string(b);
                }() +
                " vs take " +
                [&] {
                    char b[32];
                    std::snprintf(b, sizeof b, "0x%llx",
                                  (unsigned long long)v.div.off_b);
                    return std::string(b);
                }());
    }

    for (const loom_take_step_t &s : v.steps) {
        float x0 = x_of(s.step), x1 = x_of(s.step + 1);
        if (x1 < 0 || x0 > cam.px_w)
            continue;
        if (s.unaligned) {
            push(loom_prim::take_dashed_tail, x0, 0, x1, cam.lane_h, s.step,
                 kLoomDashedTailHover);
            continue;
        }
        if (!s.in_cone)
            continue;
        if (s.verdict == take_verdict::dim)
            push(loom_prim::take_dim, x0, 0, x1, cam.lane_h, s.step,
                 std::string());
        else if (s.verdict == take_verdict::hot)
            push(loom_prim::take_hot, x0, 0, x1, cam.lane_h, s.step,
                 std::string());
        // neutral: nothing is drawn but the cone outline the caller owns —
        // there is no claim to make.
    }

    if (!v.node.fault.empty())
        push(loom_prim::fault_card, cam.px_w - 260, 4, cam.px_w - 4, 64, 0,
             v.node.fault);
}

std::string loom_take_dump(const loom_take_view_t &v) {
    std::string o;
    o += "take " + v.node.edit + "\n";
    o += "alignment " + v.node.alignment + "\n";
    o += "prefix " + std::to_string(v.prefix) + "\n";
    o += "cone {";
    for (size_t i = 0; i < v.cone.size(); i++)
        o += (i ? "," : "") + std::to_string(v.cone[i]);
    o += "}\n";
    for (const loom_take_step_t &s : v.steps) {
        o += "step " + std::to_string(s.step) + " " +
             take_verdict_name(s.verdict);
        if (s.in_cone)
            o += " cone";
        if (s.unaligned)
            o += " unaligned";
        if (s.no_writes)
            o += " no_writes";
        o += "\n";
    }
    if (!v.node.fault.empty())
        o += "fault " + v.node.fault + "\n";
    if (!v.node.err.empty())
        o += "err " + v.node.err + "\n";
    return o;
}

} // namespace asmdesk
