// pick.cpp — the GL-free half of colour-ID picking (pick.h): id decode + the
// resolution of a pick to a 04 deep-link. Standard library + nav.h + the space/
// models only; no GL, no ImGui, no engine (D4).
#include "scene3d/pick.h"

namespace asmdesk::scene3d {

Pick decode_pick(uint32_t id, uint32_t n) {
    Pick p;
    if (id == 0)
        return p; // background
    const uint64_t ncells = static_cast<uint64_t>(n) * n;
    const uint32_t d = id - 1u;
    if (d < ncells) {
        p.kind = Pick::Cell;
        p.cell = d;
    } else {
        p.kind = Pick::Vertex;
        p.vertex = static_cast<uint64_t>(d) - ncells;
    }
    return p;
}

std::vector<PickVertex> pick_vertex_order(const space::TrajectorySet &traj) {
    std::vector<PickVertex> out;
    for (const space::Trajectory &tr : traj.trajectories) {
        const bool st = (tr.flags & space::TRAJ_STATISTICAL) != 0;
        for (const space::TrajPoint &pt : tr.points)
            if (!pt.is_access)
                out.push_back({tr.tid, pt.t, st});
    }
    return out;
}

std::optional<dt_link> resolve_pick(const space::TerrainModel &terr,
                                    const space::TrajectorySet &traj,
                                    const std::string &rec, const Pick &p) {
    if (p.kind == Pick::None)
        return std::nullopt;

    if (p.kind == Pick::Cell) {
        const uint32_t n = terr.w;
        if (n == 0)
            return std::nullopt;
        const uint32_t x = p.cell % n, y = p.cell / n;
        if (y >= n)
            return std::nullopt;

        // The region + address under the cell centre. unproject mirrors
        // terrain.cpp's own (x+0.5)/n rounding, so a pick lands on the same region
        // T2 placed; r == nullptr means a padding cell beyond the compacted domain.
        const float u = (x + 0.5f) / static_cast<float>(n);
        const float v = (y + 0.5f) / static_cast<float>(n);
        uint64_t addr = 0;
        const space::Region *r = nullptr;
        const bool have_region = terr.proj.unproject(u, v, &addr, &r) && r;

        // Is there EXACT content here? An exact code hit or a `mem` data access.
        // A cell holds at most one region byte (regions are non-overlapping in the
        // compacted domain), so code and data are mutually exclusive.
        const space::TerrainModel::CodeCell *code = nullptr;
        for (const space::TerrainModel::CodeCell &cc : terr.code)
            if (cc.cell == p.cell) {
                code = &cc;
                break;
            }
        const space::TerrainModel::DataCell *data = nullptr;
        if (!code)
            for (const space::TerrainModel::DataCell &dc : terr.data)
                if (dc.cell == p.cell) {
                    data = &dc;
                    break;
                }

        dt_link link;
        link.rec = rec;

        // Exact code: the trace canvas at the offset, UNLESS the region churned
        // within the recording — then the codeimage-versioned disasm pane (08-T7),
        // because the bytes at that offset differ by trace time and only disasm
        // resolves "which version at time t". The offset is addr - base: the
        // recording's basis is region-relative (offsets from the region base; an
        // abs recording's region base is the codeimage base), the same key the
        // canvas and the disasm pane already use.
        if (code) {
            if (!have_region)
                return std::nullopt; // a lit code cell must map to a region
            const bool churned = code->churn_step != UINT64_MAX;
            link.view = churned ? dt_view::disasm : dt_view::canvas;
            link.off = addr - r->base;
            return link;
        }

        // A data access (rich `mem` rung) -> the slice explorer at the step whose
        // access LAST hit this cell (`steps` is ascending; back() is most recent):
        // the dataflow reader for "what touched this address".
        if (data && !data->steps.empty()) {
            link.view = dt_view::slice;
            link.step = static_cast<uint32_t>(data->steps.back());
            return link;
        }

        // No exact content. A statistical-only cell (survey residency, TF_STAT) ->
        // the hot-edge view (08-T4), NEVER the exact slice explorer: a sampled
        // residency is not an exact density and must not open an exact reader (the
        // "statistical is never exact" invariant).
        if (terr.has_stat && p.cell < terr.stat.flags.size() &&
            (terr.stat.flags[p.cell] & space::TF_STAT)) {
            link.view = dt_view::hotedges;
            return link;
        }

        // A bare region cell with neither exact nor statistical content: open the
        // canvas at its offset. A cell mapping to no region at all is padding.
        if (!have_region)
            return std::nullopt;
        link.view = dt_view::canvas;
        link.off = addr - r->base;
        return link;
    }

    // Kind::Vertex: replay the canonical PC-vertex order to recover its (step,
    // fidelity). A statistical residency vertex -> the hot-edge view (08-T4), never
    // the exact operand timeline; an exact PC vertex -> the operand timeline at that
    // step.
    std::vector<PickVertex> order = pick_vertex_order(traj);
    if (p.vertex >= order.size())
        return std::nullopt;
    const PickVertex &pv = order[p.vertex];
    dt_link link;
    link.rec = rec;
    if (pv.statistical) {
        link.view = dt_view::hotedges;
        return link;
    }
    link.view =
        dt_view::timeline; // the Loom / operand timeline reads this vertex
    link.step = static_cast<uint32_t>(pv.t);
    return link;
}

} // namespace asmdesk::scene3d
