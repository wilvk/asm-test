// converge.cpp — cross-thread convergence detection (converge.h). Standard
// library + the pure T1/T3 models only (D4): no GL, no ImGui, no engine.
#include "space/converge.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace asmdesk::space {

namespace {

// The plane cell an address projects into, or ok=false when it maps to no region.
// The rounding of (u,v) back to (x,y) mirrors terrain.cpp::cell_of and the
// projection's own (x+0.5)/n, so a convergence cell and a terrain cell agree.
bool cell_of(const Projection &proj, uint32_t n, uint64_t addr,
             uint32_t *cell) {
    float u = 0, v = 0;
    if (!proj.project(addr, &u, &v))
        return false;
    uint32_t x = static_cast<uint32_t>(u * n);
    uint32_t y = static_cast<uint32_t>(v * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    *cell = y * n + x;
    return true;
}

} // namespace

ConvergenceSet detect_convergences(const TrajectorySet &ts,
                                   const Projection &proj,
                                   const ConvergeParams &params) {
    ConvergenceSet out;
    const uint32_t n = proj.order ? (uint32_t{1} << proj.order) : 0u;
    if (n == 0)
        return out; // an empty projection places nothing — no convergences

    // Gather placed PC vertices per cell. Only exact, address-placed paths: a
    // statistical residency layer and a relative-basis path are not per-thread
    // address trajectories, so a "shared cell" over them would be meaningless.
    struct V {
        int32_t tid;
        uint64_t t;
        uint64_t addr;
    };
    std::map<uint32_t, std::vector<V>> by_cell;
    for (const Trajectory &tr : ts.trajectories) {
        if (tr.flags & (TRAJ_STATISTICAL | TRAJ_RELATIVE_BASIS))
            continue;
        for (const TrajPoint &p : tr.points) {
            if (p.is_access)
                continue; // an access-mark spur is not a PC vertex
            uint32_t c = 0;
            if (!cell_of(proj, n, p.addr, &c))
                continue; // an unmapped address places no vertex
            by_cell[c].push_back({tr.tid, p.t, p.addr});
        }
    }

    // Per cell, keep the closest-in-time cross-tid crossing for each unordered tid
    // pair. Keyed (tid_a < tid_b, cell) so there is exactly one mark per pair per
    // cell — two threads that dwell in one cell make one hint, not a flood.
    std::map<std::tuple<int32_t, int32_t, uint32_t>, ConvergenceMark> best;
    for (auto &kv : by_cell) {
        const uint32_t cell = kv.first;
        std::vector<V> &vs = kv.second;
        // Sort by t so the window is a forward scan: once the gap exceeds the
        // window, no later vertex is closer to this i, so break.
        std::stable_sort(vs.begin(), vs.end(),
                         [](const V &a, const V &b) { return a.t < b.t; });
        for (size_t i = 0; i < vs.size(); i++) {
            for (size_t j = i + 1; j < vs.size(); j++) {
                uint64_t gap = vs[j].t - vs[i].t; // sorted: vs[j].t >= vs[i].t
                if (gap > params.window)
                    break;
                if (vs[i].tid == vs[j].tid)
                    continue; // same thread — not a cross-thread convergence
                int32_t ta = vs[i].tid, tb = vs[j].tid;
                uint64_t t_a = vs[i].t, t_b = vs[j].t;
                uint64_t a_a = vs[i].addr, a_b = vs[j].addr;
                if (tb < ta) {
                    std::swap(ta, tb);
                    std::swap(t_a, t_b);
                    std::swap(a_a, a_b);
                }
                auto key = std::make_tuple(ta, tb, cell);
                auto it = best.find(key);
                if (it == best.end() || gap < it->second.gap) {
                    ConvergenceMark m;
                    m.tid_a = ta;
                    m.tid_b = tb;
                    m.cell = cell;
                    m.t_a = t_a;
                    m.t_b = t_b;
                    m.addr_a = a_a;
                    m.addr_b = a_b;
                    m.gap = gap;
                    best[key] = m;
                }
            }
        }
    }

    out.marks.reserve(best.size());
    for (auto &kv : best)
        out.marks.push_back(kv.second); // map iterates keys in sorted order
    return out;
}

} // namespace asmdesk::space
