// blameforest.cpp — the blame convergence forest of blameforest.h. Standard
// library + the doc/ and space/ models only; no GL, no ImGui, no engine (D4).
#include "space/blameforest.h"

#include <algorithm>
#include <map>
#include <set>

#include "space/stepplace.h"

namespace asmdesk::space {

BlameForest build_blame_forest(const std::vector<BlameAttr> &blame,
                               const DataflowStream &df,
                               const Projection &proj, bool truncated) {
    BlameForest out;
    out.truncated = truncated;
    if (blame.empty()) {
        out.disabled_reason =
            "this recording carries no `blame` attributions — there are no "
            "cones to overlap, and this layer will not derive any";
        return out;
    }
    out.enabled = true;
    out.cones = static_cast<uint32_t>(blame.size());
    out.single_cone = blame.size() <= 1;

    // ONE placer for the whole layer (57 T1): every step->place conversion in
    // this file goes through it, and every refusal it makes is countable. A
    // sink's `off` is placed through its own STEP, which is the recording's
    // own resolution order (37's wire rbase, else 36's derived anchor) — the
    // offset and the step are two views of the same fact, and going through
    // the step is what keeps this layer on the ONE address route rather than
    // re-deriving a basis of its own.
    StepPlacer placer(proj, df);

    // step -> the number of DISTINCT cones containing it. A step repeated
    // within one cone counts once for that cone.
    std::map<uint32_t, uint32_t> weight;
    std::set<uint32_t> sink_steps;

    for (const BlameAttr &b : blame) {
        {
            const StepPlace sp = placer.at(b.step);
            BlameSink sink;
            sink.step = b.step;
            sink.off = b.off;
            sink.born_untraced = b.born_untraced;
            if (sp.placed) {
                sink.cell = sp.cell;
                sink.u = sp.u;
                sink.v = sp.v;
                out.sinks.push_back(sink);
            } else {
                // Counted by the placer (off_plane below); no beacon.
                out.sinks.push_back(sink); // kept so the cone is still stated
            }
            sink_steps.insert(b.step);
        }
        if (b.born_untraced) {
            // THE VERDICT, not an absence. Its cone is the sink alone, so it
            // contributes NOTHING to any step's weight — not even its own.
            // A born-untraced value "converging" with another would be a pure
            // artifact of this function.
            out.born_untraced++;
            continue;
        }
        std::set<uint32_t> seen;
        for (uint32_t s : b.cone)
            seen.insert(s);
        for (uint32_t s : seen)
            weight[s]++;
    }

    for (const auto &kv : weight) {
        const StepPlace sp = placer.at(kv.first);
        if (!sp.placed)
            continue; // counted by the placer; emits no beacon
        BlameConvergence c;
        c.step = kv.first;
        c.weight = kv.second;
        c.cell = sp.cell;
        c.u = sp.u;
        c.v = sp.v;
        c.is_sink = sink_steps.count(kv.first) != 0;
        out.producers.push_back(c);
        out.max_weight = std::max(out.max_weight, c.weight);
    }

    out.off_plane = placer.unplaced();
    out.off_plane_note = placer.note();
    return out;
}

} // namespace asmdesk::space
