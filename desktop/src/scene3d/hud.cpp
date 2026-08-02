// hud.cpp — the ImGui HUD of hud.h. ImGui + the pure space/ models only: no GL,
// no engine (D4). Renders into the caller's current ImGui frame.
#include "scene3d/hud.h"

#include <cstdio>
#include <string>

#include "imgui.h"

#include "space/projection.h"
#include "ui/theme.h"
#include "ui/timepos.h" // one time-position widget, continuous scrub (24 T4)

namespace asmdesk::scene3d {

namespace {

// The whole HUD chip axis is now the shared palette (ui/theme.h T1) — the HUD
// had drifted its own green, amber, red and grey; an "ok" chip, a "TRUNCATED /
// TORN" chip and a dim aside must each read the same here as in every 2D pane.
const ImVec4 kOk = dt_good_col();
const ImVec4 kWarn = dt_warn_col();
const ImVec4 kBad = dt_refuse_col();
const ImVec4 kDim = dt_dim_col();

// A small pill of coloured text; chips sit on one line separated by a bullet.
void chip(const ImVec4 &col, const char *text, bool &first) {
    if (!first)
        ImGui::SameLine(0, 8), ImGui::TextDisabled("|"), ImGui::SameLine(0, 8);
    ImGui::TextColored(col, "%s", text);
    first = false;
}

// Map a PlacementChip severity to its shared-palette colour.
const ImVec4 &sev_col(PlacementChip::Sev s) {
    return s == PlacementChip::Bad    ? kBad
           : s == PlacementChip::Warn ? kWarn
                                      : kOk;
}

} // namespace

std::vector<PlacementChip> placement_chips(const space::TerrainModel &terr,
                                           const space::TrajectorySet &traj) {
    std::vector<PlacementChip> out;
    // Terrain height placement (36 T3/T4): the height field could not be placed,
    // or a df_step residency rung fed it (never block coverage).
    if (!terr.anchor_error.empty())
        out.push_back({PlacementChip::Bad, "HEIGHTS NOT PLACED"});
    if (terr.height_source == "df_step")
        out.push_back(
            {PlacementChip::Warn,
             "heights: single-step residency (df_step), not block coverage"});
    // Trajectory PC-path placement (36 T2/T4): not placed at all, a partial
    // placement (the 4096-byte codeimage clamp), or a fully derived placement.
    if (traj.pc_points > 0) {
        if (traj.pc_placed == 0) {
            out.push_back({PlacementChip::Bad, "PATH NOT PLACED"});
        } else if (traj.pc_placed < traj.pc_points) {
            char buf[96];
            std::snprintf(buf, sizeof buf,
                          "%llu of %llu path vertices off-plane",
                          (unsigned long long)(traj.pc_points - traj.pc_placed),
                          (unsigned long long)traj.pc_points);
            out.push_back({PlacementChip::Warn, buf});
        } else if (traj.anchored) {
            // 37 T5: grade the placement by HOW it was resolved — a wire-STATED
            // base (df_step.rbase) and a DERIVED single-codeimage-span anchor are
            // different claims and must not share a label.
            const char *label =
                traj.anchor_source == "wire"
                    ? "rel: span stated on the wire (rbase)"
                : traj.anchor_source == "mixed"
                    ? "rel: span from the wire (rbase) and the codeimage "
                      "anchor "
                      "(mixed)"
                    : "rel: anchored to the codeimage span (derived placement)";
            out.push_back({PlacementChip::Warn, label});
        }
    }
    return out;
}

std::vector<EncodingSwatch> terrain_encoding_swatches() {
    // Mirrors kTerrainFrag's branches (scene3d/shaders/embedded.h) VERBATIM —
    // the same keep-in-sync convention TORN/STAT/CHURN's C++ bit values
    // already follow against the GLSL constants. TF_READ/TF_WRITE are
    // deliberately absent: the terrain shader does not branch on them.
    return {
        {space::TF_CHURN,
         "churn: codeimage version changed within [0,t] (scaffold)",
         {0.2f, 0.7f, 1.0f}}, // kTerrainFrag CHURN: mix(base, this, 0.5)
        {space::TF_TORN,
         "torn: capture truncated/dropped (rubble, a known lower bound)",
         {1.0f, 0.15f, 0.15f}}, // kTerrainFrag torn gash: mix(base, this, 0.7)
        {space::TF_STAT,
         "statistical: sampled residency (separate ghost-fog layer)",
         {0.55f, 0.55f, 0.60f}}, // kStatFrag's grey
        {space::TF_UNKNOWN,
         "fog-of-war: in-domain, no content reached yet (dark pit)",
         {0.02f, 0.02f, 0.03f}}, // kTerrainFrag UNKNOWN: mix(base, this, 0.85)
    };
}

std::string height_scale_note(uint64_t max_full_heat) {
    if (max_full_heat == 0)
        return "height: log(1 + access count) — no data at this slice";
    char buf[112];
    std::snprintf(buf, sizeof buf,
                  "height: log(1 + access count) — brightest band this slice "
                  "= %llu",
                  static_cast<unsigned long long>(max_full_heat));
    return buf;
}

std::vector<uint64_t> trajectory_axis_ticks(uint64_t nsteps, int max_ticks) {
    std::vector<uint64_t> out;
    if (nsteps == 0)
        return out;
    if (max_ticks < 2)
        max_ticks = 2;
    uint64_t step = nsteps / static_cast<uint64_t>(max_ticks - 1);
    if (step == 0)
        step = 1;
    for (uint64_t t = 0; t <= nsteps; t += step)
        out.push_back(t);
    if (out.back() != nsteps)
        out.push_back(nsteps);
    return out;
}

std::string vertical_axes_note() {
    return "two vertical meanings share this screen axis: terrain height = "
          "access density (log), path height = trace time (steps)";
}

void draw_trajectory_ruler(ImDrawList *draw_list, const Camera &cam,
                           ImVec2 origin, ImVec2 size, uint64_t nsteps,
                           float traj_scale) {
    if (draw_list == nullptr || nsteps == 0 || size.x <= 0.0f ||
        size.y <= 0.0f)
        return;
    float m[16];
    cam.mvp(m, size.x / size.y); // the SAME aspect the viewport rendered at
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(dt_dim_col());
    ImVec2 prev{};
    bool have_prev = false;
    for (uint64_t t : trajectory_axis_ticks(nsteps)) {
        const float world[4] = {0.0f, static_cast<float>(t) * traj_scale,
                                0.0f, 1.0f};
        float clip[4];
        for (int r = 0; r < 4; r++)
            clip[r] = m[0 * 4 + r] * world[0] + m[1 * 4 + r] * world[1] +
                      m[2 * 4 + r] * world[2] + m[3 * 4 + r] * world[3];
        if (clip[3] <= 0.0f) { // behind the camera: cannot place this tick
            have_prev = false;
            continue;
        }
        const float ndc_x = clip[0] / clip[3], ndc_y = clip[1] / clip[3];
        const ImVec2 p(origin.x + (ndc_x * 0.5f + 0.5f) * size.x,
                       origin.y + (1.0f - (ndc_y * 0.5f + 0.5f)) * size.y);
        draw_list->AddCircleFilled(p, 2.5f, col);
        char buf[24];
        std::snprintf(buf, sizeof buf, "%llu",
                     static_cast<unsigned long long>(t));
        draw_list->AddText(ImVec2(p.x + 4.0f, p.y - 6.0f), col, buf);
        if (have_prev)
            draw_list->AddLine(prev, p, col, 1.0f);
        prev = p;
        have_prev = true;
    }
}

PlacementChip basis_chip(const space::TerrainModel &terr,
                         const space::TrajectorySet &traj) {
    if (!terr.basis_error.empty())
        return {PlacementChip::Bad, "EXACT TERRAIN REFUSED (mixed basis)"};
    // 36 T4 (defect 1): a df-only recording has an EMPTY terr.basis (no `trace`
    // feeds the canvas), so fall back to the trajectory's basis — which DOES say
    // "rel". This finally reports the rel chip 25 T6 promised but that never fired
    // because the HUD keyed only on the canvas basis. Deleting the `: traj.basis`
    // fallback makes a df-only recording return an empty basis chip — which a
    // named test now catches.
    const std::string &basis = !terr.basis.empty() ? terr.basis : traj.basis;
    if (basis.empty())
        return {PlacementChip::Ok, ""}; // nothing to say
    return {PlacementChip::Ok, basis == "abs"
                                   ? "abs: true address-space path"
                                   : "rel: routine-relative (not a true path)"};
}

void draw_scene_hud(HudState &s, const space::TerrainModel &terr,
                    const space::TrajectorySet &traj) {
    ImGui::Begin("3D overview");

    // Report HUD keyboard focus (22 T2): the caller applies the camera keys when
    // the HUD OR the 3D viewport holds focus, so orbiting works from either.
    s.kbd_focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // --- provenance chips: coarse-vs-rich, exact-vs-statistical, truncation ----
    ImGui::TextUnformatted("provenance:");
    ImGui::SameLine();
    bool first = true;
    // The basis chip is now PURE (basis_chip), so its defect-1 traj.basis fallback
    // is testable — reverting it fails a named check.
    {
        const PlacementChip bc = basis_chip(terr, traj);
        if (!bc.text.empty())
            chip(sev_col(bc.sev), bc.text.c_str(), first);
    }
    if (traj.refused())
        chip(kBad, "trajectory refused", first);
    // 36 T4: the placement chrome — nothing about anchoring is silent.
    for (const PlacementChip &pc : placement_chips(terr, traj))
        chip(sev_col(pc.sev), pc.text.c_str(), first);
    // coarse-vs-rich
    chip(terr.mem_present ? kOk : kWarn,
         terr.mem_present ? "rich: per-access memory"
                          : "coarse: no memory stream",
         first);
    // exact-vs-statistical
    if (terr.has_stat)
        chip(kWarn, "statistical residency present (separate layer)", first);
    if (terr.torn)
        chip(kBad, "TRUNCATED / TORN", first);
    if (terr.churn_present)
        chip(kWarn, "JIT churn", first);
    if (!terr.mem_note.empty()) {
        ImGui::TextColored(kDim, "%s", terr.mem_note.c_str());
    }
    // 36 T4: the placement notes as dim asides beside mem_note (anchor_error is a
    // refusal, so it rides in the refuse colour). No HudState field is needed —
    // these derive from terr/traj each frame.
    if (!terr.height_note.empty())
        ImGui::TextColored(kDim, "%s", terr.height_note.c_str());
    if (!terr.anchor_error.empty())
        ImGui::TextColored(kBad, "%s", terr.anchor_error.c_str());
    if (!traj.placement_note.empty())
        ImGui::TextColored(kDim, "%s", traj.placement_note.c_str());
    if (!terr.basis_error.empty())
        ImGui::TextColored(kBad, "%s", terr.basis_error.c_str());
    if (traj.refused())
        ImGui::TextColored(kBad, "%s", traj.diagnostic.c_str());

    ImGui::Separator();

    // --- playhead: drives t -> the caller re-slices T2 / rebuilds T3 -----------
    int t = static_cast<int>(s.t);
    int tmax = static_cast<int>(s.nsteps);
    if (tmax < 0)
        tmax = 0;
    // The ONE time-position widget, CONTINUOUS variant (24 T4): a real total (the
    // step count) exists here, so a scrub is faithful — unlike the Invocations
    // pager, which has no continuous total and uses the discrete variant.
    if (dt_timepos_scrub("playhead (step)", &t, tmax)) {
        s.t = static_cast<uint64_t>(t);
        s.playhead_moved = true;
    }
    // --- play/pause: animate the playhead over the terrain-time axis (34 T3) ----
    // Reports the intent; the caller owns the transport and advances t. Labelled
    // by its axis so it never reads as the flat views' execution-step playhead.
    if (ImGui::Button(s.playing ? "Pause##scene" : "Play##scene"))
        s.req_play_toggle = true;
    ImGui::SameLine();
    ImGui::TextColored(kDim, "play — step (trace time)");

    // --- layer toggles ---------------------------------------------------------
    ImGui::TextUnformatted("layers:");
    ImGui::SameLine();
    ImGui::Checkbox("terrain", &s.layers.terrain);
    ImGui::SameLine();
    ImGui::Checkbox("exact", &s.layers.exact);
    ImGui::SameLine();
    ImGui::Checkbox("statistical", &s.layers.statistical);
    ImGui::SameLine();
    ImGui::Checkbox("access", &s.layers.access_marks);
    // T7 (44-faithful-city-phase-a): the four new city-reskin bools, extending
    // this same checkbox list — each independently toggleable, matching the
    // existing five.
    ImGui::SameLine();
    ImGui::Checkbox("zoning", &s.layers.zoning);
    ImGui::SameLine();
    ImGui::Checkbox("weather", &s.layers.weather);
    ImGui::SameLine();
    ImGui::Checkbox("ghost fog", &s.layers.ghost_fog);
    ImGui::SameLine();
    ImGui::Checkbox("vehicle", &s.layers.vehicle);
    ImGui::SameLine();
    ImGui::Checkbox("contours", &s.layers.contours);

    // --- camera presets --------------------------------------------------------
    if (ImGui::Button("reset view"))
        s.req_reset_view = true;
    ImGui::SameLine();
    if (ImGui::Button("top-down (2D-ish)"))
        s.req_top_down = true;
    ImGui::SameLine();
    ImGui::TextColored(kDim, "3D to find, 2D to read");

    ImGui::Separator();

    // --- encodings (T3/T4, 49-one-time-truth): the scene's own colour + axis
    // key, placed here (not the first-open primer) so it stays reachable ----
    ImGui::TextUnformatted("encodings:");
    ImGui::TextColored(kDim, "%s",
                       height_scale_note(terr.max_full_heat(s.t)).c_str());
    for (const EncodingSwatch &sw : terrain_encoding_swatches()) {
        ImVec4 col{sw.rgb[0], sw.rgb[1], sw.rgb[2], 1.0f};
        ImGui::ColorButton(("##enc" + sw.label).c_str(), col,
                           ImGuiColorEditFlags_NoTooltip |
                               ImGuiColorEditFlags_NoPicker,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::TextUnformatted(sw.label.c_str());
    }
    ImGui::TextColored(kDim, "%s", vertical_axes_note().c_str());

    ImGui::Separator();

    // --- region legend ---------------------------------------------------------
    ImGui::TextUnformatted("regions:");
    for (const space::Region &r : terr.proj.regions) {
        space::RegionStyle st = space::region_style(r.kind);
        ImVec4 sw{st.r, st.g, st.b, 1.0f};
        ImGui::ColorButton(("##sw" + r.label).c_str(), sw,
                           ImGuiColorEditFlags_NoTooltip |
                               ImGuiColorEditFlags_NoPicker,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::Text("%s  (%s)", r.label.empty() ? st.name : r.label.c_str(),
                    st.name);
    }

    ImGui::End();
}

} // namespace asmdesk::scene3d
