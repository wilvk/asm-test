// hud.cpp — the ImGui HUD of hud.h. ImGui + the pure space/ models only: no GL,
// no engine (D4). Renders into the caller's current ImGui frame.
#include "scene3d/hud.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "imgui.h"

#include "scene3d/goto.h"
#include "scene3d/layers.h" // 56 T1: the layer registry
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

// 56 T1 step 4: the group header text — one line per LayerDesc::Group, printed
// once when the toggle loop crosses into a new group (the table is already
// grouped in scene_layers_all()'s own declaration order).
const char *group_label(LayerDesc::Group g) {
    switch (g) {
    case LayerDesc::Group::Fidelity:
        return "fidelity:";
    case LayerDesc::Group::Structure:
        return "structure:";
    case LayerDesc::Group::Activity:
        return "activity:";
    case LayerDesc::Group::Survey:
        return "survey (statistical):";
    }
    return "";
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

const std::vector<EncodingSwatch> &terrain_encoding_swatches() {
    // Mirrors kTerrainFrag's branches (scene3d/shaders/embedded.h) VERBATIM —
    // the same keep-in-sync convention TORN/STAT/CHURN's C++ bit values
    // already follow against the GLSL constants. TF_READ/TF_WRITE are
    // deliberately absent: the terrain shader does not branch on them.
    // A function-local static: this is compile-time-constant data, so every
    // caller (the legend is drawn every HUD frame) shares one built-once
    // instance rather than reallocating the vector + its string labels per
    // frame.
    static const std::vector<EncodingSwatch> kSwatches = {
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
    return kSwatches;
}

const std::vector<OverlaySwatch> &overlay_encoding_swatches() {
    // Mirrors scene.cpp's Line::color literals VERBATIM (set_convergences /
    // set_trajectories' access_spurs_ setup) — see this function's own doc
    // comment in hud.h for why the colours are a synced copy, not a shared
    // constant. Function-local static for the same built-once reason as
    // terrain_encoding_swatches above.
    static const std::vector<OverlaySwatch> kSwatches = {
        {"convergence: co-locality hint (same cell, stated gap) — never a "
         "proven race or order",
         {1.0f, 0.25f, 0.85f}}, // scene.cpp set_convergences' bright magenta
        {"access spur: a PC vertex's data access, at the same height",
         {0.85f, 0.85f, 0.90f}}, // scene.cpp set_trajectories' lavender
    };
    return kSwatches;
}

const char *inspect_hint_note() {
    return "hover to inspect, click to open the flat reader";
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

const char *confidence_layer_note() {
    return "confidence: darker unknown = never described; amber hatch = "
           "in-window, below-rate; black hatch = outside the stated window";
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

const char *vertical_axes_note() {
    return "two vertical meanings share this screen axis: terrain height = "
           "access density (log), path height = trace time (steps)";
}

std::string camera_here_text(const space::Projection &proj, float u, float v) {
    uint64_t addr = 0;
    const space::Region *r = nullptr;
    if (!proj.unproject(u, v, &addr, &r) || r == nullptr)
        return "you are here: outside the compacted domain";
    char buf[192];
    space::RegionStyle st = space::region_style(r->kind);
    std::snprintf(buf, sizeof buf, "you are here: 0x%llx (%s)",
                  static_cast<unsigned long long>(addr),
                  r->label.empty() ? st.name : r->label.c_str());
    return buf;
}

const std::vector<std::string> &scene_control_lines() {
    // 48 T5: one line per CamKey value (the exhaustiveness a test pins), plus
    // the mouse gestures CamKey has no key for (drag/wheel/click/double-click
    // have no keyboard equivalent to enumerate from the enum). CamKey's
    // enumeration is invariant across the run, so this is built once
    // (function-local static) rather than reconstructed every frame the
    // "controls" header is expanded.
    static const std::vector<std::string> kLines = [] {
        std::vector<std::string> lines = {
            "left-drag: orbit",
            "middle-drag or shift+left-drag: pan",
            "mouse wheel: dolly (zoom)",
            "double-click: recentre on what's under the cursor",
            "click (no drag): open in 2D (\"3D to find, 2D to read\")",
        };
        using scene3d::CamKey;
        // One line per enum value, in declaration order — adding a CamKey
        // without adding it here is the drift this generation exists to
        // prevent.
        for (CamKey k :
            {CamKey::OrbitLeft, CamKey::OrbitRight, CamKey::OrbitUp,
             CamKey::OrbitDown, CamKey::DollyIn, CamKey::DollyOut,
             CamKey::Reset, CamKey::TopDown, CamKey::PanLeft,
             CamKey::PanRight, CamKey::PanForward, CamKey::PanBack}) {
            switch (k) {
            case CamKey::OrbitLeft:
                lines.push_back("Left arrow: orbit left");
                break;
            case CamKey::OrbitRight:
                lines.push_back("Right arrow: orbit right");
                break;
            case CamKey::OrbitUp:
                lines.push_back("Up arrow: orbit up");
                break;
            case CamKey::OrbitDown:
                lines.push_back("Down arrow: orbit down");
                break;
            case CamKey::DollyIn:
                lines.push_back("+ / =: dolly in");
                break;
            case CamKey::DollyOut:
                lines.push_back("-: dolly out");
                break;
            case CamKey::Reset:
                lines.push_back("R: reset view (the landmark)");
                break;
            case CamKey::TopDown:
                lines.push_back("T: top-down (2D-ish)");
                break;
            case CamKey::PanLeft:
                lines.push_back("(pan left: mouse only — no key bound)");
                break;
            case CamKey::PanRight:
                lines.push_back("(pan right: mouse only — no key bound)");
                break;
            case CamKey::PanForward:
                lines.push_back("(pan forward: mouse only — no key bound)");
                break;
            case CamKey::PanBack:
                lines.push_back("(pan back: mouse only — no key bound)");
                break;
            }
        }
        return lines;
    }();
    return kLines;
}

void draw_trajectory_ruler(ImDrawList *draw_list, const Camera &cam,
                           ImVec2 origin, ImVec2 size, uint64_t nsteps,
                           float traj_scale) {
    if (draw_list == nullptr || nsteps == 0 || size.x <= 0.0f || size.y <= 0.0f)
        return;
    float m[16];
    cam.mvp(m, size.x / size.y); // the SAME aspect the viewport rendered at
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(dt_dim_col());
    ImVec2 prev{};
    bool have_prev = false;
    for (uint64_t t : trajectory_axis_ticks(nsteps)) {
        // The same world->clip transform goto.cpp's scene_on_screen already
        // does — one linmath call, not a second hand-unrolled multiply.
        vec4 world = {0.0f, static_cast<float>(t) * traj_scale, 0.0f, 1.0f};
        vec4 clip;
        mat4x4_mul_vec4(clip, reinterpret_cast<vec4 *>(m), world);
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
    // 54 T1: the observed-data-span derivation, surfaced the same way.
    if (!terr.proj.data_span_note.empty())
        ImGui::TextColored(kDim, "%s", terr.proj.data_span_note.c_str());
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

    // --- layer toggles (56 T1): generated from the registry, never hand-listed
    // ---------------------------------------------------------------------------
    // Grouped by the question asked (fidelity/structure/activity/survey);
    // s.layers.*row.flag is a direct pointer-to-member access, so the toggle a
    // row drives is keyed by the MEMBER, never by the row's position — a
    // reordered table cannot shuffle which bool a checkbox flips. A
    // Statistical-graded row carries the shared STATISTICAL wording in the
    // warn colour automatically (T1 step 3), so no layer re-invents the
    // phrasing.
    ImGui::TextUnformatted("layers:");
    {
        std::optional<LayerDesc::Group> last_group;
        for (const LayerDesc &row : scene_layers_all()) {
            if (!last_group.has_value() || *last_group != row.group) {
                // No preceding SameLine(): every ImGui item starts a fresh
                // line by default, so a new group's label always begins one.
                ImGui::TextColored(kDim, "%s", group_label(row.group));
                last_group = row.group;
            }
            ImGui::SameLine();
            ImGui::Checkbox(row.label, &(s.layers.*row.flag));
            // The registry's own "question" (T1's stated goal: no layer
            // ships without one) — a hover tooltip, so the toggle row stays
            // compact while the question is still one hover away.
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", row.question);
            if (row.grade == LayerGrade::Statistical) {
                ImGui::SameLine();
                ImGui::TextColored(kWarn, "STATISTICAL — survey");
            }
        }
    }
    // T1 (55) step 3: "HUD-exposed" as a stated fact, not yet a live slider —
    // Scene owns edl_strength/edl_radius_px directly (like y_scale/traj_scale_
    // above it, neither of which is HUD-adjustable either) and the abstract
    // SceneHost/SceneFrame seam has no channel for a per-frame float today;
    // wiring a slider through it is a follow-on, not silently dropped.
    ImGui::TextColored(kDim, "EDL: strength %.1f, radius %.0fpx",
                       static_cast<double>(kEdlStrengthDefault),
                       static_cast<double>(kEdlRadiusPxDefault));

    // --- camera presets ----------------------------------------------------
    // 48 T4: two buttons, two honest meanings — "reset view" frames the
    // LANDMARK (the code-district centroid, stable across live growth),
    // "default view" restores the literal Camera{} pose 25/34 already
    // documented. Neither is silently repurposed into the other.
    if (s.has_home) {
        if (ImGui::Button("reset view"))
            s.req_reset_view = true;
    } else {
        ImGui::TextDisabled("reset view");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("no code region placed — nothing to land on");
    }
    ImGui::SameLine();
    if (ImGui::Button("default view"))
        s.req_default_view = true;
    ImGui::SameLine();
    if (ImGui::Button("top-down (2D-ish)"))
        s.req_top_down = true;
    ImGui::SameLine();
    ImGui::TextColored(kDim, "3D to find, 2D to read");

    // 48 T4: "you are here" — a pure function of (terr.proj, camera target),
    // so the wording is testable with no ImGui frame; drawn every frame here.
    ImGui::TextColored(
        kDim, "%s",
        camera_here_text(terr.proj, s.cam_target_u, s.cam_target_v).c_str());

    // 50 T2/T4: the flat views' selection, located onto this plane by its
    // address. Graded, never hidden (D7): active-but-unplaceable states the
    // reason verbatim; nothing selected says so plainly; a placed selection
    // offers "frame the selection" (T4) and, when ambiguous (the same
    // address revisited at several steps — a loop), says so rather than
    // implying it is the one true occurrence.
    if (s.has_highlight) {
        if (ImGui::Button("frame the selection"))
            s.req_frame_highlight = true;
        if (s.highlight_ambiguous) {
            ImGui::SameLine();
            ImGui::TextColored(
                kDim, "(this address recurs — cell is where, not when)");
        }
    } else if (!s.highlight_reason.empty()) {
        ImGui::TextColored(kWarn, "selection: %s", s.highlight_reason.c_str());
    } else {
        ImGui::TextColored(kDim, "selection: nothing brushed in this recording");
    }

    ImGui::Separator();

    // --- go to (48 T3): name a destination instead of only hunting for it ---
    ImGui::TextUnformatted("go to:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    const bool addr_enter = ImGui::InputTextWithHint(
        "##goto_addr", "0x... address", s.goto_addr_buf, sizeof s.goto_addr_buf,
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("go##addr") || addr_enter) {
        char *end = nullptr;
        uint64_t addr =
            static_cast<uint64_t>(std::strtoull(s.goto_addr_buf, &end, 16));
        float gu = 0.0f, gv = 0.0f;
        if (end != s.goto_addr_buf &&
            scene_goto_addr(terr.proj, addr, &gu, &gv)) {
            s.req_goto = true;
            s.goto_u = gu;
            s.goto_v = gv;
            // Keep the current dolly — a goto finds a place, it does not also
            // decide how close to stand.
            s.nav_note.clear();
        } else {
            s.nav_note =
                end == s.goto_addr_buf
                    ? "go to: enter a hex address (e.g. 0x400000)"
                    : "go to: address not mapped by any region in this "
                      "recording";
        }
    }
    if (terr.proj.regions.empty()) {
        // T5's own rule (48 T3 too): a control the current state cannot serve
        // says why rather than vanishing.
        ImGui::SameLine();
        ImGui::TextDisabled("(region goto: no regions in this recording)");
    } else {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        std::string preview =
            s.goto_region_sel >= 0 && static_cast<size_t>(s.goto_region_sel) <
                                          terr.proj.regions.size()
                ? terr.proj.regions[static_cast<size_t>(s.goto_region_sel)]
                      .label
                : std::string("region...");
        if (ImGui::BeginCombo("##goto_region", preview.c_str())) {
            for (size_t i = 0; i < terr.proj.regions.size(); i++) {
                const space::Region &r = terr.proj.regions[i];
                // Verbatim label, never an invented name — code@0x<base> for
                // an unnamed span (D7, and this brief's own fidelity note).
                // Every region already carries a label today
                // (regions_from_codeimage / observed_data_spans both set
                // one); the fallback is defensive, not a real path.
                char fallback[32];
                std::snprintf(fallback, sizeof fallback, "code@0x%llx",
                              static_cast<unsigned long long>(r.base));
                std::string name = r.label.empty() ? fallback : r.label;
                bool sel = s.goto_region_sel == static_cast<int>(i);
                if (ImGui::Selectable(name.c_str(), sel))
                    s.goto_region_sel = static_cast<int>(i);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("go##region") && s.goto_region_sel >= 0) {
            float gu = 0.0f, gv = 0.0f, grad = 0.0f;
            if (scene_goto_region(terr.proj,
                                  static_cast<size_t>(s.goto_region_sel), &gu,
                                  &gv, &grad)) {
                s.req_goto = true;
                s.goto_u = gu;
                s.goto_v = gv;
                s.goto_radius = grad;
                s.nav_note.clear();
            } else {
                s.nav_note = "go to: that region has no addressable extent";
            }
        }
    }
    if (!s.nav_note.empty())
        ImGui::TextColored(kWarn, "%s", s.nav_note.c_str());

    ImGui::Separator();

    // --- controls (48 T5): generated from CamKey, never hand-written --------
    if (ImGui::CollapsingHeader("controls")) {
        for (const std::string &line : scene_control_lines())
            ImGui::TextUnformatted(line.c_str());
    }

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
    // T2 (56): the confidence layer's own idiom, shown only while it is on —
    // it never advertises a hatch the plane is not currently drawing.
    if (s.layers.confidence)
        ImGui::TextColored(kDim, "%s", confidence_layer_note());
    // T5 (47): the pickable-overlay-line swatches (convergence arcs, access
    // spurs) — same row shape as the terrain swatches just above, a distinct
    // list because they encode LINES, not per-cell terrain colour.
    for (const OverlaySwatch &sw : overlay_encoding_swatches()) {
        ImVec4 col{sw.rgb[0], sw.rgb[1], sw.rgb[2], 1.0f};
        ImGui::ColorButton(("##ovl" + sw.label).c_str(), col,
                           ImGuiColorEditFlags_NoTooltip |
                               ImGuiColorEditFlags_NoPicker,
                           ImVec2(12, 12));
        ImGui::SameLine();
        ImGui::TextUnformatted(sw.label.c_str());
    }
    ImGui::TextColored(kDim, "%s", vertical_axes_note());
    // T5: advertise that the pane is interrogable — kept in the legend
    // (drawn every frame), never the first-open-only primer.
    ImGui::TextColored(kDim, "%s", inspect_hint_note());

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
