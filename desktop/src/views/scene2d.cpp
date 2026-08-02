// scene2d.cpp — the flat surface's pure model (views/scene2d.h).
#include "views/scene2d.h"

#include <algorithm>
#include <cstdint>

#include "space/projection.h" // region_style() — kTerrainFrag's own kindHue
                              // source of truth (its comment says so)

namespace asmdesk {

namespace {

float mix1(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

uint32_t ceil_div(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

} // namespace

CellPaint cell_paint(float height, uint32_t flags, uint8_t kind) {
    // Mirrors kTerrainFrag (embedded.h) EXACTLY, branch for branch, in the
    // SAME order: kind hue -> height mix -> churn -> stat -> unknown -> torn.
    // space::region_style() IS the kind-hue source of truth (kTerrainFrag's
    // own kindHue comment says so) — an out-of-range Region::Kind (including
    // kKindByCellNone, 255) falls through its switch to the same
    // {0.55,0.55,0.60,"unknown"} grey the shader's `k < 6u ? k : 5u` clamp
    // lands on, so no separate range check is needed here.
    const space::RegionStyle hot =
        space::region_style(static_cast<space::Region::Kind>(kind));
    const float h = clamp01(height);
    float base[3] = {mix1(0.08f, hot.r, h), mix1(0.10f, hot.g, h),
                     mix1(0.16f, hot.b, h)};
    if (flags & space::TF_CHURN) {
        base[0] = mix1(base[0], 0.2f, 0.5f);
        base[1] = mix1(base[1], 0.7f, 0.5f);
        base[2] = mix1(base[2], 1.0f, 0.5f);
    }
    if (flags & space::TF_STAT) {
        base[0] *= 0.6f;
        base[1] *= 0.6f;
        base[2] *= 0.6f;
    }
    if (flags & space::TF_UNKNOWN) {
        base[0] = mix1(base[0], 0.02f, 0.85f);
        base[1] = mix1(base[1], 0.02f, 0.85f);
        base[2] = mix1(base[2], 0.03f, 0.85f);
    }
    CellPaint p;
    p.r = base[0];
    p.g = base[1];
    p.b = base[2];
    p.a = 1.0f;
    if (flags & space::TF_TORN) {
        p.r = mix1(p.r, 1.0f, 0.7f);
        p.g = mix1(p.g, 0.15f, 0.7f);
        p.b = mix1(p.b, 0.15f, 0.7f);
    }
    // `hatched`/`why` grade by VISUAL dominance — the reverse of the mix
    // order above, since each later mix sits on top of the earlier ones:
    // torn's red gash reads over an unknown pit, which reads over a dimmed
    // statistical cell, which reads over the churn tint.
    p.hatched = (flags & (space::TF_TORN | space::TF_UNKNOWN)) != 0u;
    if (flags & space::TF_TORN)
        p.why = "torn: capture truncated/dropped (rubble)";
    else if (flags & space::TF_UNKNOWN)
        p.why = "fog-of-war: in-domain, no content reached yet";
    else if (flags & space::TF_STAT)
        p.why = "statistical: sampled residency";
    else if (flags & space::TF_CHURN)
        p.why = "churn: codeimage version changed within [0,t]";
    else
        p.why = "exact";
    return p;
}

Scene2dPlan build_scene2d_plan(const space::TerrainModel &terr,
                               const space::Terrain &slice,
                               const space::TrajectorySet &traj,
                               const space::ConvergenceSet &conv,
                               uint64_t playhead_t,
                               uint32_t max_blocks_per_edge) {
    Scene2dPlan plan;
    plan.w = terr.w;
    plan.h = terr.h;
    plan.has_plane =
        terr.w > 0 && terr.h > 0 &&
        slice.height.size() == static_cast<size_t>(terr.w) * terr.h &&
        slice.flags.size() == slice.height.size();
    if (!plan.has_plane)
        return plan;

    const uint32_t budget = max_blocks_per_edge > 0 ? max_blocks_per_edge : 1;
    const uint32_t longest = std::max(terr.w, terr.h);
    plan.downsample = std::max<uint32_t>(1, ceil_div(longest, budget));
    plan.grid_w = ceil_div(terr.w, plan.downsample);
    plan.grid_h = ceil_div(terr.h, plan.downsample);

    plan.blocks.reserve(static_cast<size_t>(plan.grid_w) * plan.grid_h);
    for (uint32_t by = 0; by < plan.grid_h; by++) {
        const uint32_t y0 = by * plan.downsample;
        const uint32_t y1 = std::min(y0 + plan.downsample, terr.h);
        for (uint32_t bx = 0; bx < plan.grid_w; bx++) {
            const uint32_t x0 = bx * plan.downsample;
            const uint32_t x1 = std::min(x0 + plan.downsample, terr.w);

            Scene2dBlock blk;
            blk.bx = bx;
            blk.by = by;
            float max_h = 0.0f;
            uint32_t or_flags = 0;
            // T2 step 5: the representative source cell for kind/region is
            // the block's top-left cell, UNLESS it is off-domain and a later
            // cell in the block is not — a block straddling the compacted-
            // domain edge should read as its real content, not as padding.
            uint32_t rep_cell = y0 * terr.w + x0;
            bool rep_is_padding = true;
            for (uint32_t y = y0; y < y1; y++) {
                for (uint32_t x = x0; x < x1; x++) {
                    const uint32_t cell = y * terr.w + x;
                    max_h = std::max(max_h, slice.height[cell]);
                    or_flags |= slice.flags[cell];
                    blk.total_cells++;
                    const bool padding =
                        cell >= terr.kind_by_cell.size() ||
                        terr.kind_by_cell[cell] == space::kKindByCellNone;
                    if (padding) {
                        blk.off_domain_cells++;
                    } else if (rep_is_padding) {
                        rep_cell = cell;
                        rep_is_padding = false;
                    }
                }
            }
            const uint8_t rep_kind = rep_cell < terr.kind_by_cell.size()
                                         ? terr.kind_by_cell[rep_cell]
                                         : space::kKindByCellNone;
            blk.paint = cell_paint(max_h, or_flags, rep_kind);
            if (!rep_is_padding) {
                const uint32_t rx = rep_cell % terr.w, ry = rep_cell / terr.w;
                float ru = (rx + 0.5f) / static_cast<float>(terr.w);
                float rv = (ry + 0.5f) / static_cast<float>(terr.h);
                uint64_t addr = 0;
                const space::Region *r = nullptr;
                if (terr.proj.unproject(ru, rv, &addr, &r) && r)
                    blk.region =
                        static_cast<int32_t>(r - terr.proj.regions.data());
            }
            plan.blocks.push_back(blk);
        }
    }

    // T2 steps 2-4: trajectories, clipped/graded to the SAME playhead the
    // slice was cut at (49's dim-never-discard rule, applied identically
    // here). PC vertices only; access marks are pulled out separately below,
    // exactly as scene.cpp's set_trajectories splits them into a second Line.
    plan.paths.reserve(traj.trajectories.size());
    for (const space::Trajectory &tr : traj.trajectories) {
        Scene2dPath path;
        path.tid = tr.tid;
        path.statistical = (tr.flags & space::TRAJ_STATISTICAL) != 0u;
        path.points.reserve(tr.points.size());
        for (const space::TrajPoint &pt : tr.points) {
            if (pt.is_access) {
                float u = 0, v = 0;
                if (terr.proj.project(pt.addr, &u, &v))
                    plan.marks.push_back({u, v, /*convergence=*/false});
                continue;
            }
            Scene2dPathPoint pp;
            pp.past_playhead = pt.t > playhead_t;
            float u = 0, v = 0;
            const bool projected = terr.proj.project(pt.addr, &u, &v);
            pp.placed = pt.placed && projected;
            if (pp.placed) {
                pp.u = u;
                pp.v = v;
            }
            path.points.push_back(pp);
        }
        // Pushed unconditionally, even if empty (an all-access-marks
        // trajectory) — the draw half's tid-colour fallback is the path's
        // own INDEX in this vector when tid < 0, mirroring scene.cpp's
        // tid_seq, which likewise increments once per trajectory regardless
        // of whether that trajectory emits a drawable line. Skipping empty
        // paths here would desync the two counters.
        plan.paths.push_back(std::move(path));
    }

    for (const space::ConvergenceMark &m : conv.marks) {
        const uint32_t x = m.cell % terr.w, y = m.cell / terr.w;
        const float u = (x + 0.5f) / static_cast<float>(terr.w);
        const float v = (y + 0.5f) / static_cast<float>(terr.h);
        plan.marks.push_back({u, v, /*convergence=*/true});
    }

    // T4 step 1: one label per region, anchored at the region's OWN base
    // address (always inside the region by construction) rather than a
    // traced boundary polygon — a region's true footprint on a Hilbert-
    // mapped plane is not a rectangle, and outlining it exactly is not what
    // this task's "S" effort buys. Scene2dBlock::region (above) is what lets
    // the draw half outline block-level region transitions cheaply instead.
    plan.region_labels.reserve(terr.proj.regions.size());
    for (const space::Region &r : terr.proj.regions) {
        float u = 0, v = 0;
        if (!terr.proj.project(r.base, &u, &v))
            continue;
        Scene2dRegionLabel lbl;
        lbl.u = u;
        lbl.v = v;
        lbl.text = r.label.empty() ? space::region_style(r.kind).name : r.label;
        plan.region_labels.push_back(std::move(lbl));
    }

    return plan;
}

bool scene2d_pick_cell(const Scene2dPlan &plan, float frac_x, float frac_y,
                       uint32_t *cell) {
    if (!plan.has_plane || plan.grid_w == 0 || plan.grid_h == 0)
        return false;
    if (frac_x < 0.0f || frac_x >= 1.0f || frac_y < 0.0f || frac_y >= 1.0f)
        return false;
    uint32_t bx =
        static_cast<uint32_t>(frac_x * static_cast<float>(plan.grid_w));
    uint32_t by =
        static_cast<uint32_t>(frac_y * static_cast<float>(plan.grid_h));
    bx = std::min(bx, plan.grid_w - 1);
    by = std::min(by, plan.grid_h - 1);
    // The SAME top-left-of-block arithmetic build_scene2d_plan used for its
    // representative cell (region/kind), run backwards from a click fraction.
    const uint32_t x = std::min(bx * plan.downsample, plan.w - 1);
    const uint32_t y = std::min(by * plan.downsample, plan.h - 1);
    *cell = y * plan.w + x;
    return true;
}

} // namespace asmdesk
