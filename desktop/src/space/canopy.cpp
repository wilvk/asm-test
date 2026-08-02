// canopy.cpp — the per-module residency skyline of canopy.h.
#include "space/canopy.h"

#include <algorithm>
#include <cmath>

namespace asmdesk::space {

std::vector<ModuleCanopy> build_module_canopies(const TerrainModel &model,
                                                uint64_t t) {
    std::vector<ModuleCanopy> out;
    const size_t nregions = model.proj.regions.size();
    if (nregions == 0 || model.w == 0)
        return out;

    out.resize(nregions);
    for (size_t i = 0; i < nregions; ++i)
        out[i].region = i;
    std::vector<ModuleCanopy> stat_out(nregions);
    for (size_t i = 0; i < nregions; ++i)
        stat_out[i].region = i;
    std::vector<char> stat_seen(nregions, 0);

    // One whole-plane sweep (the same cost class as build_terrain's own
    // kind_by_cell pass) resolves every cell's owning region ONCE; per-cell
    // code_at/stat lookups are the existing O(log n) index reads.
    for (uint32_t y = 0; y < model.h; ++y) {
        for (uint32_t x = 0; x < model.w; ++x) {
            const size_t cell = static_cast<size_t>(y) * model.w + x;
            if (cell >= model.kind_by_cell.size() ||
                model.kind_by_cell[cell] == kKindByCellNone)
                continue; // off-domain: no region owns this cell at all

            const float u = (x + 0.5f) / static_cast<float>(model.w);
            const float v = (y + 0.5f) / static_cast<float>(model.h);
            uint64_t addr = 0;
            const Region *r = nullptr;
            if (!model.proj.unproject(u, v, &addr, &r) || r == nullptr)
                continue;
            const size_t ri = static_cast<size_t>(r - model.proj.regions.data());
            if (ri >= nregions)
                continue; // defensive: unproject always returns a real Region*

            ModuleCanopy &mc = out[ri];
            mc.cells_mapped++;
            mc.min_u = std::min(mc.min_u, u);
            mc.min_v = std::min(mc.min_v, v);
            mc.max_u = std::max(mc.max_u, u);
            mc.max_v = std::max(mc.max_v, v);

            if (const TerrainModel::CodeCell *cc = model.code_at(
                    static_cast<uint32_t>(cell))) {
                const size_t hits = static_cast<size_t>(
                    std::upper_bound(cc->steps.begin(), cc->steps.end(), t) -
                    cc->steps.begin());
                if (hits > 0) {
                    mc.cells_hit++;
                    // raw_heat sums the RAW hit count, never full_heat's
                    // ALREADY-log-scaled sibling — step 2's own anti-
                    // regression bar (log(sum) != sum(log)).
                    mc.raw_heat += static_cast<double>(hits);
                }
            }

            if (model.has_stat && cell < model.stat.flags.size() &&
                (model.stat.flags[cell] & TF_STAT) != 0u) {
                // The per-cell raw count is not retained by the stat layer —
                // only its log1p height is (build_stat) — so it is recovered
                // by inverting log1p, exactly the approximation pick.cpp's
                // resolve_pick_hint already uses for the same reason (D7:
                // stated as approximate, never a fabricated precise count).
                const double approx =
                    std::expm1(static_cast<double>(model.stat.height[cell]));
                stat_out[ri].cells_mapped = mc.cells_mapped; // mirrors footprint
                stat_out[ri].min_u = mc.min_u;
                stat_out[ri].min_v = mc.min_v;
                stat_out[ri].max_u = mc.max_u;
                stat_out[ri].max_v = mc.max_v;
                stat_out[ri].cells_hit++;
                stat_out[ri].raw_heat += std::max(0.0, approx);
                stat_seen[ri] = 1;
            }
        }
    }

    for (ModuleCanopy &mc : out) {
        if (mc.cells_mapped == 0)
            continue; // never happens for a region actually in proj.regions,
                       // guarded rather than assumed (this builder's own bar)
        mc.height = std::log1p(mc.raw_heat);
        mc.torn = model.torn && mc.cells_hit > 0;
    }
    // Drop the (impossible, but checked) zero-footprint placeholders rather
    // than emit a canopy for a region this plane never actually placed.
    out.erase(std::remove_if(out.begin(), out.end(),
                             [](const ModuleCanopy &mc) {
                                 return mc.cells_mapped == 0;
                             }),
             out.end());

    // The statistical entries: only for a region with actual survey
    // residency in its footprint — never a zero-heat placeholder (that
    // "mapped but cold" fact belongs to the exact rung alone, T2's own fog-
    // of-war territory, not this survey-only canopy).
    for (size_t i = 0; i < nregions; ++i) {
        if (!stat_seen[i])
            continue;
        ModuleCanopy &mc = stat_out[i];
        mc.height = std::log1p(mc.raw_heat);
        mc.statistical = true;
        out.push_back(mc);
    }
    return out;
}

} // namespace asmdesk::space
