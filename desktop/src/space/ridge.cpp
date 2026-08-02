// ridge.cpp — the dominant-path ridge of ridge.h. Standard library + the doc/
// and space/ models only; no GL, no ImGui, no engine (D4).
#include "space/ridge.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "space/stepplace.h"

namespace asmdesk::space {

float ridge_brightness(double fraction) {
    if (fraction < 0.0)
        fraction = 0.0;
    if (fraction > 1.0)
        fraction = 1.0;
    // Two straight segments meeting exactly at the split, so 0.5 maps to
    // kRidgeSplitBrightness on the nose and a test can pin it by name.
    if (fraction <= 0.5) {
        const double t = fraction / 0.5;
        return static_cast<float>(kRidgeMinBrightness +
                                  (kRidgeSplitBrightness -
                                   kRidgeMinBrightness) *
                                      t);
    }
    const double t = (fraction - 0.5) / 0.5;
    return static_cast<float>(kRidgeSplitBrightness +
                              (1.0 - kRidgeSplitBrightness) * t);
}

PathRidge build_path_ridge(const TraceStream &trace, const Projection &proj,
                           bool truncated) {
    PathRidge out;
    out.truncated = truncated || trace.truncated ||
                    trace.insns_total > trace.insns.size() ||
                    trace.blocks_total > trace.blocks.size();

    if (!trace.basis_error.empty()) {
        out.disabled_reason = trace.basis_error;
        return out;
    }
    if (trace.insns.empty()) {
        out.disabled_reason =
            "this recording carries no ordered instruction stream — block "
            "transitions are OBSERVED from that stream, and this layer will "
            "not derive them from block addresses alone";
        return out;
    }
    if (trace.blocks.empty()) {
        out.disabled_reason =
            "this recording names no basic blocks — there are no forks to "
            "aggregate, and inferring block boundaries from the instruction "
            "stream would be a static guess (the exact failure mode "
            "2026-07-17-blockstep-reconstruction-defects.md records)";
        return out;
    }

    // --- offset -> absolute address, in the recording's own stated basis ----
    // The SAME two rungs scene_locate_off applies, using the basis TraceStream
    // already decoded rather than re-deriving one: "abs" takes the offset as
    // the address; "rel" routes through the single-codeimage Anchor, which
    // refuses rather than guessing. A recording that states no basis at all is
    // refused — the schema forbids defaulting it.
    Anchor anchor;
    bool need_anchor = false;
    if (trace.basis == "abs") {
        // nothing to resolve
    } else if (trace.basis == "rel") {
        anchor = resolve_anchor(proj.regions);
        need_anchor = true;
        if (!anchor.ok) {
            out.disabled_reason = anchor.reason;
            return out;
        }
    } else {
        out.disabled_reason =
            trace.basis.empty()
                ? std::string("this recording's `trace` events state no "
                              "address basis, and the schema forbids "
                              "defaulting one")
                : ("unrecognized recording basis \"" + trace.basis + "\"");
        return out;
    }
    auto to_abs = [&](uint64_t off, uint64_t *abs) {
        if (!need_anchor) {
            *abs = off;
            return true;
        }
        return anchor.place(off, abs);
    };

    const std::set<uint64_t> block_starts(trace.blocks.begin(),
                                          trace.blocks.end());

    // --- the transition histogram, purely OBSERVED --------------------------
    // Walk the ordered instruction stream. Whenever an instruction IS a
    // recorded block start, that is an entry into that block; if we were
    // already inside one, the pair is an observed transition. A self-loop
    // (re-entering the same block) is recorded as one, because it is one.
    //
    // Nothing here maps a non-block-start instruction to a block: that would
    // need block LENGTHS the wire does not carry, and "the greatest recorded
    // start below this address" would fabricate membership for an instruction
    // in a block the recording never opened.
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> counts;
    std::map<uint64_t, uint64_t> from_total;
    std::map<std::pair<uint64_t, uint64_t>, uint32_t> first_visit;
    std::set<uint64_t> entered;
    bool have_cur = false;
    uint64_t cur = 0;
    uint32_t visit_seq = 0;
    for (uint64_t off : trace.insns) {
        if (block_starts.count(off) == 0) {
            if (!have_cur)
                out.unattributed_insns++;
            continue;
        }
        entered.insert(off);
        if (have_cur) {
            const auto key = std::make_pair(cur, off);
            if (counts[key]++ == 0)
                first_visit[key] = visit_seq++;
            from_total[cur]++;
        }
        cur = off;
        have_cur = true;
    }
    for (uint64_t b : trace.blocks)
        if (entered.count(b) == 0)
            out.blocks_unvisited++;

    if (counts.empty()) {
        out.enabled = true;
        // Not a refusal: the recording is real and simply never left a block
        // (a single-block trace). The cap below still states the unknown
        // continuation.
    } else {
        out.enabled = true;
    }

    // --- the modal successor per block --------------------------------------
    std::map<uint64_t, uint64_t> best;
    for (const auto &kv : counts) {
        auto it = best.find(kv.first.first);
        if (it == best.end() || kv.second > it->second)
            best[kv.first.first] = kv.second;
    }

    // Placement goes through T1's shared address route (place_address), so the
    // ridge and every other causal layer agree about where an address is.
    std::map<uint64_t, StepPlace> placed;
    auto place = [&](uint64_t off) -> const StepPlace & {
        auto it = placed.find(off);
        if (it != placed.end())
            return it->second;
        StepPlace sp;
        uint64_t abs = 0;
        if (to_abs(off, &abs))
            sp = place_address(proj, abs);
        else
            sp.why = "this block offset is past the anchored span";
        return placed.emplace(off, sp).first->second;
    };

    for (const auto &kv : counts) {
        const uint64_t from = kv.first.first, to = kv.first.second;
        const StepPlace &a = place(from);
        const StepPlace &b = place(to);
        if (!a.placed || !b.placed) {
            out.off_plane++;
            continue; // an unplaceable endpoint draws no segment, ever
        }
        RidgeSegment seg;
        seg.from_addr = a.addr;
        seg.to_addr = b.addr;
        seg.count = kv.second;
        seg.from_total = from_total[from];
        seg.fraction = seg.from_total > 0
                           ? static_cast<double>(seg.count) / seg.from_total
                           : 0.0;
        seg.modal = seg.count == best[from];
        seg.self_loop = from == to;
        seg.ua = a.u;
        seg.va = a.v;
        seg.ub = b.u;
        seg.vb = b.v;
        seg.height =
            static_cast<float>(std::log1p(static_cast<double>(seg.count)));
        seg.visit = first_visit[kv.first];
        out.segments.push_back(seg);
    }
    std::stable_sort(out.segments.begin(), out.segments.end(),
                     [](const RidgeSegment &a, const RidgeSegment &b) {
                         return a.visit < b.visit;
                     });

    // --- forks: a block with two or more OBSERVED successors ----------------
    std::map<uint64_t, uint32_t> succ_count;
    for (const auto &kv : counts)
        succ_count[kv.first.first]++;
    for (const auto &kv : succ_count) {
        if (kv.second < 2)
            continue;
        const StepPlace &a = place(kv.first);
        if (!a.placed)
            continue; // already counted in off_plane by the segment loop
        RidgeFork f;
        f.addr = a.addr;
        f.u = a.u;
        f.v = a.v;
        f.total = from_total[kv.first];
        f.successors = kv.second;
        f.modal_fraction =
            f.total > 0 ? static_cast<double>(best[kv.first]) / f.total : 0.0;
        f.leaving_mass = 1.0 - f.modal_fraction;
        out.forks.push_back(f);
    }

    // --- caps: entered, never observed to leave -----------------------------
    // The ridge STOPS here and says so. It never wraps to offset 0 and never
    // joins to the next block by address adjacency.
    for (uint64_t b : entered) {
        if (from_total.count(b) != 0)
            continue;
        const StepPlace &a = place(b);
        if (!a.placed) {
            out.off_plane++;
            continue;
        }
        RidgeCap c;
        c.addr = a.addr;
        c.u = a.u;
        c.v = a.v;
        out.caps.push_back(c);
    }

    return out;
}

} // namespace asmdesk::space
