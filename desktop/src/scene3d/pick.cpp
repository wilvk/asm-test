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
    for (const space::Trajectory &tr : traj.trajectories)
        for (const space::TrajPoint &pt : tr.points)
            if (!pt.is_access)
                out.push_back({tr.tid, pt.t});
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
        uint32_t x = p.cell % n, y = p.cell / n;
        if (y >= n)
            return std::nullopt;
        // Cell centre -> the address it holds. unproject mirrors terrain.cpp's
        // own (x+0.5)/n rounding, so a pick lands on the same region T2 placed.
        float u = (x + 0.5f) / static_cast<float>(n);
        float v = (y + 0.5f) / static_cast<float>(n);
        uint64_t addr = 0;
        const space::Region *r = nullptr;
        if (!terr.proj.unproject(u, v, &addr, &r) || !r)
            return std::nullopt; // a padding cell beyond the compacted domain

        dt_link link;
        link.rec = rec;
        link.view = dt_view::canvas; // 3D to find, 2D to read: open the canvas
        // The recording's basis is region-relative (offsets from the region base
        // — the golden corpus and codeimage bases both are), so the canvas offset
        // is addr - base. An abs recording's region base is the codeimage base,
        // which makes this the same offset the canvas already keys on.
        link.off = addr - r->base;
        return link;
    }

    // Kind::Vertex: replay the canonical PC-vertex order to recover its step.
    std::vector<PickVertex> order = pick_vertex_order(traj);
    if (p.vertex >= order.size())
        return std::nullopt;
    dt_link link;
    link.rec = rec;
    link.view =
        dt_view::slice; // a trajectory vertex reads on the slice explorer
    link.step = static_cast<uint32_t>(order[p.vertex].t);
    return link;
}

} // namespace asmdesk::scene3d
