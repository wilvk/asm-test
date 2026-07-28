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
    if (!terr.basis_error.empty()) {
        chip(kBad, "EXACT TERRAIN REFUSED (mixed basis)", first);
    } else {
        // 36 T4 (defect 1): a df-only recording has an EMPTY terr.basis (no
        // `trace` feeds the canvas), so fall back to the trajectory's basis —
        // which DOES say "rel". This finally draws the rel chip 25 T6 promised
        // but that never fired because the HUD keyed only on the canvas basis.
        const std::string &basis =
            !terr.basis.empty() ? terr.basis : traj.basis;
        if (!basis.empty())
            chip(kOk,
                 basis == "abs" ? "abs: true address-space path"
                                : "rel: routine-relative (not a true path)",
                 first);
    }
    if (traj.refused())
        chip(kBad, "trajectory refused", first);
    // 36 T4: the placement chrome — nothing about anchoring is silent.
    for (const PlacementChip &pc : placement_chips(terr, traj))
        chip(pc.sev == PlacementChip::Bad ? kBad : kWarn, pc.text.c_str(),
             first);
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
    // step count) exists here, so a scrub is honest — unlike the Invocations
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

    // --- camera presets --------------------------------------------------------
    if (ImGui::Button("reset view"))
        s.req_reset_view = true;
    ImGui::SameLine();
    if (ImGui::Button("top-down (2D-ish)"))
        s.req_top_down = true;
    ImGui::SameLine();
    ImGui::TextColored(kDim, "3D to find, 2D to read");

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
