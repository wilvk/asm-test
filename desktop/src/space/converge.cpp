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

    // Gather placed PC vertices per cell, under the four-condition admission bar
    // (36 T5). A vertex is a legal input to the ONE projection this function does
    // only if: (S1) it is in the same address space — guaranteed structurally, T1's
    // anchor base comes from the SAME region vector build_projection consumed, and
    // shell.cpp hands the same Projection to both builders; (S2) it was individually
    // PLACED — a measured absolute vertex, or a rel offset the anchor placed
    // (base+off), NOT a raw offset left behind (the 4096-byte codeimage clamp makes
    // those common); (S3) it is per-thread — TRAJ_STATISTICAL stays excluded
    // unconditionally; (S4) it shares one clock family — the df_step fallback runs
    // only when the trace loop placed nothing, so one set has one PC source. An
    // ANCHORED rel path (base+off) meets all four and is two paths on one plane; an
    // unanchored rel path and the statistical layer do not.
    struct V {
        int32_t tid;
        uint64_t t;
        uint64_t addr;
    };
    std::map<uint32_t, std::vector<V>> by_cell;
    for (const Trajectory &tr : ts.trajectories) {
        // S3: never the statistical residency layer.
        if (tr.flags & TRAJ_STATISTICAL)
            continue;
        // The rel test, NARROWED not dropped: exclude a relative-basis path ONLY
        // when it was not anchored. Deleting the rel bit outright would readmit raw
        // offsets — exactly the false address-space claim this file forbids — so an
        // UNANCHORED rel path still cannot converge.
        if ((tr.flags & TRAJ_RELATIVE_BASIS) && !(tr.flags & TRAJ_ANCHORED))
            continue;
        for (const TrajPoint &p : tr.points) {
            if (p.is_access)
                continue; // an access-mark spur is not a PC vertex
            // S2: a raw offset the anchor could not place is not a real address —
            // never bucket it. Applied to EVERY admitted trajectory (placed
            // defaults true for a measured abs vertex) so the guard cannot rot.
            if (!p.placed)
                continue;
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
