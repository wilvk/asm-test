// locate.cpp — the pure resolvers of locate.h. Standard library + nlohmann/json
// (via recording.h) + the space/ models only; no GL, no ImGui, no engine (D4).
#include "space/locate.h"

namespace asmdesk::space {

namespace {

// The SAME cell-centre-recovering arithmetic classify_cell (pick.cpp) and
// unproject() itself use: Projection::project always returns (x+0.5)/n, so
// truncating u*n recovers x exactly. Kept local rather than exposed on
// Projection — every other caller of this arithmetic (pick.cpp, goto.cpp)
// keeps its own copy too, per those files' own comments on why the two
// GL/engine-free halves cannot share one implementation across that boundary;
// this is the space/-only third copy of a two-line derivation, not a third
// implementation of anything stateful.
bool cell_of(const Projection &proj, uint64_t addr, uint32_t *cell) {
    float u = 0.0f, v = 0.0f;
    if (!proj.project(addr, &u, &v))
        return false;
    const uint32_t n = uint32_t(1) << proj.order;
    uint32_t x = static_cast<uint32_t>(u * n);
    uint32_t y = static_cast<uint32_t>(v * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    *cell = y * n + x;
    return true;
}

// The recording-wide basis an offset is stated against: the first `trace`
// event's own "basis" field (mirrors build_trajectories' identical scan), or
// "rel" when the recording carries `df_step` but no `trace` at all (df_step
// offsets are routine-relative by construction — the same fallback
// build_trajectories applies). Empty when neither stream exists, or the lone
// `trace` event omitted "basis" (the schema forbids defaulting it).
std::string recording_basis(const Recording &rec) {
    auto it = rec.by_kind.find("trace");
    if (it != rec.by_kind.end()) {
        for (const Event &e : it->second) {
            auto b = e.body.find("basis");
            if (b != e.body.end() && b->is_string())
                return b->get<std::string>();
        }
    }
    if (rec.by_kind.count("df_step") != 0)
        return "rel";
    return "";
}

// How many `trace` events (offset-bearing ones) carry this exact raw offset —
// used to grade ambiguity without re-deriving an address per candidate.
uint64_t count_trace_off(const Recording &rec, uint64_t off) {
    auto it = rec.by_kind.find("trace");
    if (it == rec.by_kind.end())
        return 0;
    uint64_t n = 0;
    for (const Event &e : it->second) {
        auto o = e.body.find("off");
        if (o != e.body.end() && o->is_number() && o->get<uint64_t>() == off)
            n++;
    }
    return n;
}

} // namespace

Located scene_locate_off(const Projection &proj, const Recording &rec,
                         uint64_t off) {
    Located out;
    const std::string basis = recording_basis(rec);
    if (basis.empty()) {
        out.reason = "the recording carries no trace or df_step stream to "
                     "resolve an offset's basis against";
        return out;
    }

    uint64_t addr = 0;
    if (basis == "abs") {
        addr = off;
    } else if (basis == "rel") {
        const Anchor anchor = resolve_anchor(proj.regions);
        if (!anchor.ok) {
            out.reason = anchor.reason;
            return out;
        }
        uint64_t abs = 0;
        if (!anchor.place(off, &abs)) {
            out.reason = "the offset is past the anchored span (the "
                         "producer's capture-window clamp)";
            return out;
        }
        addr = abs;
    } else {
        out.reason = "unrecognized recording basis \"" + basis + "\"";
        return out;
    }

    uint32_t cell = 0;
    if (!cell_of(proj, addr, &cell)) {
        out.reason = "the resolved address is not mapped by any region in "
                     "this recording";
        return out;
    }
    out.ok = true;
    out.cell = cell;
    out.addr = addr;
    out.how = "offset";
    out.ambiguous = count_trace_off(rec, off) > 1;
    return out;
}

bool StepAddrResolver::resolve(uint32_t i, uint64_t *addr, std::string *how,
                               std::string *fail_reason) {
    if (i >= df_.insn_off.size()) {
        if (fail_reason)
            *fail_reason = "step index is out of range for this pass";
        return false;
    }
    const uint64_t raw = df_.insn_off[i];
    const bool has_rbase = df_.rbase_present &&
                           static_cast<size_t>(i) < df_.insn_rbase.size() &&
                           df_.insn_rbase[static_cast<size_t>(i)] != 0;
    if (has_rbase) {
        *addr = df_.insn_rbase[static_cast<size_t>(i)] + raw;
        if (how)
            *how = "step->offset (wire rbase)";
        return true;
    }
    if (!anchor_computed_) {
        anchor_ = resolve_anchor(proj_.regions);
        anchor_computed_ = true;
    }
    if (!anchor_.ok) {
        if (fail_reason)
            *fail_reason = anchor_.reason;
        return false;
    }
    uint64_t abs = 0;
    if (!anchor_.place(raw, &abs)) {
        if (fail_reason)
            *fail_reason = "this step's offset is past the anchored span";
        return false;
    }
    *addr = abs;
    if (how)
        *how = "step->offset (derived anchor)";
    return true;
}

Located scene_locate_step(const Projection &proj, const DataflowStream &df,
                          uint32_t step) {
    Located out;
    if (step >= df.insn_off.size()) {
        out.reason = "step " + std::to_string(step) +
                     " is out of range for this dataflow pass (" +
                     std::to_string(df.insn_off.size()) + " step(s))";
        return out;
    }

    StepAddrResolver resolver(proj, df);
    uint64_t addr = 0;
    std::string how, fail_reason;
    if (!resolver.resolve(step, &addr, &how, &fail_reason)) {
        // resolve() already derived has_rbase and, on the no-rbase path,
        // already ran (and cached) resolve_anchor() to answer this same
        // question — fail_reason carries the answer straight through rather
        // than this function re-deriving has_rbase and re-scanning
        // resolve_anchor() a second time just to build the message.
        out.reason = fail_reason;
        return out;
    }

    uint32_t cell = 0;
    if (!cell_of(proj, addr, &cell)) {
        out.reason = "the resolved address is not mapped by any region in "
                     "this recording";
        return out;
    }
    out.ok = true;
    out.cell = cell;
    out.addr = addr;
    out.how = how;

    for (uint32_t i = 0; i < df.insn_off.size(); i++) {
        if (i == step)
            continue;
        uint64_t a2 = 0;
        if (resolver.resolve(i, &a2, nullptr) && a2 == addr) {
            out.ambiguous = true;
            break;
        }
    }
    return out;
}

} // namespace asmdesk::space
