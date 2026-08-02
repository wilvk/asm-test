// scene2d_draw.cpp — the ImGui half of the flat terrain surface (views/
// scene2d.h, 52-flat-terrain-surface.md). Draws only: every decision (cell
// colour, down-sample factor, path breaks, playhead grading) already lives in
// the plan. Picking is the one exception (T3) — it routes through the SAME
// scene3d::resolve_pick / resolve_pick_hint the 3D pane's GL path calls, so a
// flat pick and a 3D pick of the same cell can never disagree; those two
// functions are themselves engine-free (scene3d/pick.h), so this TU adds
// ImGui to the closure and nothing else.
#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <string>

#include "imgui.h"

#include "nav.h"
#include "scene3d/hud.h" // height_scale_note / overlay_encoding_swatches
#include "scene3d/pick.h"
#include "ui/theme.h"
#include "views/scene2d.h"

namespace asmdesk {

namespace {

// scene3d/scene.cpp's OWN tid_color palette (anonymous-namespace, GL-side),
// copied VERBATIM — the same mirrored-not-shared convention cell_paint's
// kKindHue table follows against kTerrainFrag. A trajectory's hue is a
// presentation nicety, not a fidelity fact, but using the SAME table keeps a
// thread's colour consistent between the two surfaces.
constexpr float kTidPalette[6][3] = {
    {0.95f, 0.85f, 0.25f}, {0.40f, 0.80f, 1.00f}, {0.55f, 0.95f, 0.45f},
    {1.00f, 0.55f, 0.55f}, {0.80f, 0.60f, 1.00f}, {0.35f, 0.95f, 0.85f}};

ImU32 tid_u32(int idx, float alpha) {
    const float *c = kTidPalette[((idx % 6) + 6) % 6];
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
}

void add_dashed_line(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col,
                     float thickness) {
    const float dash = 5.0f, gap = 4.0f;
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 0.5f)
        return;
    const float ux = dx / len, uy = dy / len;
    float t = 0.0f;
    bool on = true;
    while (t < len) {
        const float seg = on ? dash : gap;
        const float t2 = std::min(t + seg, len);
        if (on)
            dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t),
                       ImVec2(a.x + ux * t2, a.y + uy * t2), col, thickness);
        t = t2;
        on = !on;
    }
}

} // namespace

void draw_scene2d(const Scene2dPlan &plan, const space::TerrainModel &terr,
                  const space::TrajectorySet &traj,
                  const space::ConvergenceSet &conv, const std::string &rec,
                  uint64_t playhead_t, uint64_t follow_step,
                  const DataflowStream *df,
                  const std::function<void(const dt_link &)> &go) {
    if (!plan.has_plane) {
        ImGui::TextDisabled(
            "flat surface: no address-space regions in this recording");
        return;
    }
    if (plan.grid_w == 0 || plan.grid_h == 0)
        return;

    // T4 step 3: the SAME scale wording the 3D HUD's legend states (49-T3),
    // so the two surfaces never claim two different scales for one recording.
    if (plan.downsample > 1)
        ImGui::TextColored(dt_maybe_col(),
                           "down-sampled reading: 1 block = %u x %u cells",
                           plan.downsample, plan.downsample);
    ImGui::TextColored(
        dt_dim_col(), "%s",
        scene3d::height_scale_note(terr.max_full_heat(playhead_t)).c_str());

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 32.0f)
        avail.x = 32.0f;
    if (avail.y < 32.0f)
        avail.y = 32.0f;
    float cell_px = std::floor(std::min(avail.x / static_cast<float>(plan.grid_w),
                                        avail.y / static_cast<float>(plan.grid_h)));
    if (cell_px < 1.0f)
        cell_px = 1.0f;
    const float drawn_w = cell_px * static_cast<float>(plan.grid_w);
    const float drawn_h = cell_px * static_cast<float>(plan.grid_h);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    // --- cells --------------------------------------------------------------
    for (const Scene2dBlock &b : plan.blocks) {
        // Wholly padding: visibly NOT land (nothing drawn — the panel's own
        // background), never a dark filled cell (T4 step 1).
        if (b.total_cells > 0 && b.off_domain_cells == b.total_cells)
            continue;
        const ImVec2 p0(origin.x + static_cast<float>(b.bx) * cell_px,
                        origin.y + static_cast<float>(b.by) * cell_px);
        const ImVec2 p1(p0.x + cell_px, p0.y + cell_px);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(b.paint.r, b.paint.g, b.paint.b, b.paint.a));
        dl->AddRectFilled(p0, p1, col);
        if (b.paint.hatched && cell_px >= 3.0f)
            dl->AddLine(p0, p1, IM_COL32(0, 0, 0, 130));
        // Straddles the compacted-domain edge: some source cells are padding,
        // some are not — mark it rather than let the fold hide it.
        if (b.off_domain_cells > 0 && b.off_domain_cells < b.total_cells)
            dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 100));
    }

    // T4 step 1: region-transition boundaries between adjacent blocks — the
    // cheap, block-level substitute for tracing each region's true (non-
    // rectangular) footprint on the Hilbert-mapped plane.
    for (const Scene2dBlock &b : plan.blocks) {
        const size_t idx = static_cast<size_t>(b.by) * plan.grid_w + b.bx;
        if (b.bx + 1 < plan.grid_w) {
            const Scene2dBlock &rn = plan.blocks[idx + 1];
            if (rn.region != b.region) {
                const float x = origin.x + static_cast<float>(b.bx + 1) * cell_px;
                dl->AddLine(ImVec2(x, origin.y + static_cast<float>(b.by) * cell_px),
                           ImVec2(x, origin.y + static_cast<float>(b.by + 1) * cell_px),
                           IM_COL32(255, 255, 255, 70));
            }
        }
        if (b.by + 1 < plan.grid_h) {
            const Scene2dBlock &dn = plan.blocks[idx + plan.grid_w];
            if (dn.region != b.region) {
                const float y = origin.y + static_cast<float>(b.by + 1) * cell_px;
                dl->AddLine(ImVec2(origin.x + static_cast<float>(b.bx) * cell_px, y),
                           ImVec2(origin.x + static_cast<float>(b.bx + 1) * cell_px, y),
                           IM_COL32(255, 255, 255, 70));
            }
        }
    }

    // --- trajectories (T2 step 2/3/4) ---------------------------------------
    for (size_t i = 0; i < plan.paths.size(); i++) {
        const Scene2dPath &path = plan.paths[i];
        const int hue_idx = path.tid >= 0 ? path.tid : static_cast<int>(i);
        bool have_prev = false;
        ImVec2 prev{};
        bool prev_past = false;
        for (const Scene2dPathPoint &pt : path.points) {
            if (!pt.placed) {
                have_prev = false; // break the strip; never interpolate
                continue;
            }
            const ImVec2 cur(origin.x + pt.u * drawn_w, origin.y + pt.v * drawn_h);
            if (have_prev) {
                // 49's dim-never-discard rule, applied identically here: a
                // segment touching a not-yet-reached vertex is dimmed, not
                // omitted — it is real, recorded data.
                const float alpha =
                    (path.statistical ? 0.55f : 1.0f) *
                    ((prev_past || pt.past_playhead) ? 0.35f : 1.0f);
                const ImU32 col = tid_u32(hue_idx, alpha);
                if (path.statistical)
                    add_dashed_line(dl, prev, cur, col, 1.5f);
                else
                    dl->AddLine(prev, cur, col, 1.5f);
            }
            prev = cur;
            prev_past = pt.past_playhead;
            have_prev = true;
        }
    }

    // --- access marks + convergence hints, established colours (hud.cpp) ---
    {
        const std::vector<scene3d::OverlaySwatch> sw =
            scene3d::overlay_encoding_swatches();
        const ImU32 conv_col =
            sw.size() > 0
                ? ImGui::ColorConvertFloat4ToU32(
                      ImVec4(sw[0].rgb[0], sw[0].rgb[1], sw[0].rgb[2], 0.9f))
                : IM_COL32(255, 60, 220, 230);
        const ImU32 spur_col =
            sw.size() > 1
                ? ImGui::ColorConvertFloat4ToU32(
                      ImVec4(sw[1].rgb[0], sw[1].rgb[1], sw[1].rgb[2], 0.85f))
                : IM_COL32(215, 215, 230, 220);
        for (const Scene2dMark &m : plan.marks) {
            const ImVec2 c(origin.x + m.u * drawn_w, origin.y + m.v * drawn_h);
            dl->AddCircleFilled(c, m.convergence ? 3.0f : 2.0f,
                                m.convergence ? conv_col : spur_col);
        }
    }

    // T4 step 1: region labels, verbatim.
    for (const Scene2dRegionLabel &lbl : plan.region_labels) {
        const ImVec2 p(origin.x + lbl.u * drawn_w, origin.y + lbl.v * drawn_h);
        dl->AddText(p, IM_COL32(255, 255, 255, 210), lbl.text.c_str());
    }

    // --- hover + click (T3): the SAME pick router the GL path uses --------
    ImGui::InvisibleButton("scene2d-surface", ImVec2(drawn_w, drawn_h));
    if (ImGui::IsItemHovered()) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const float fx = (mp.x - origin.x) / drawn_w;
        const float fy = (mp.y - origin.y) / drawn_h;
        uint32_t cell = 0;
        if (scene2d_pick_cell(plan, fx, fy, &cell)) {
            scene3d::Pick pk;
            pk.kind = scene3d::Pick::Cell;
            pk.cell = cell;
            const scene3d::PickHint hint =
                scene3d::resolve_pick_hint(terr, traj, conv, pk, follow_step);
            if (!hint.empty) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(hint.what.c_str());
                if (!hint.where.empty())
                    ImGui::TextUnformatted(hint.where.c_str());
                if (!hint.quantity.empty())
                    ImGui::TextUnformatted(hint.quantity.c_str());
                if (!hint.fidelity.empty())
                    ImGui::TextColored(dt_warn_col(), "%s", hint.fidelity.c_str());
                const std::string click_line = hint.target.empty()
                                                   ? "click -> nothing here"
                                                   : "click -> " + hint.target;
                ImGui::TextColored(dt_dim_col(), "%s", click_line.c_str());
                ImGui::EndTooltip();
            }
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                std::optional<dt_link> link = scene3d::resolve_pick(
                    terr, traj, rec, pk, conv, follow_step, df);
                if (link && go)
                    go(*link);
            }
        }
    }
}

} // namespace asmdesk
