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

    // T1 (58-memory-data-cell-family): the DATA rung's own contract. Four
    // facts, always stated, so an empty memory layer is never mistaken for an
    // empty program:
    //   1. which rung is feeding the data half at all (`mem` present or not);
    //   2. how many observed-data spans the projection carries, and at what
    //      gap threshold, since those spans are what makes a heap/stack
    //      address placeable at all (54 T1);
    //   3. how many `mem` accesses failed to place, which used to be a silent
    //      `continue` in terrain.cpp;
    //   4. nothing invented for the absent case — the coarse wording is
    //      TerrainModel::mem_note's own, verbatim (D7 / 24-one-visual-language).
    if (terr.mem_present) {
        out.push_back({PlacementChip::Ok, "rich: per-access memory"});
    } else if (!terr.mem_note.empty()) {
        out.push_back({PlacementChip::Warn, terr.mem_note});
    }
    {
        size_t spans = 0;
        for (const space::Region &r : terr.proj.regions)
            if (r.label == space::kObservedDataLabel)
                spans++;
        char buf[144];
        if (spans > 0) {
            std::snprintf(buf, sizeof buf,
                          "data spans: %zu observed (%llu-byte gap threshold)",
                          spans,
                          (unsigned long long)space::kObservedSpanGap);
            out.push_back({PlacementChip::Warn, buf});
        } else if (terr.mem_present) {
            // `mem` fed the rung but no span was derived: every access landed
            // inside an already-known region (fine), or none placed (T1's own
            // drop chip below then says so). Either way, say which.
            out.push_back(
                {PlacementChip::Warn,
                 "data spans: none derived — every observed address fell "
                 "inside an existing region, or none placed"});
        }
    }
    if (terr.mem_accesses > 0) {
        char buf[160];
        if (terr.mem_dropped == 0) {
            std::snprintf(buf, sizeof buf,
                          "%llu of %llu memory accesses placed",
                          (unsigned long long)terr.mem_accesses,
                          (unsigned long long)terr.mem_accesses);
            out.push_back({PlacementChip::Ok, buf});
        } else {
            std::snprintf(
                buf, sizeof buf,
                "%llu of %llu memory accesses OFF-PLANE (no region maps "
                "them)",
                (unsigned long long)terr.mem_dropped,
                (unsigned long long)terr.mem_accesses);
            out.push_back({terr.mem_dropped == terr.mem_accesses
                               ? PlacementChip::Bad
                               : PlacementChip::Warn,
                           buf});
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

const char *translucency_mode_note() {
    return "layered translucency: dithered (approximate) — a surviving "
           "pixel is fully opaque, translucency is spent as coverage, not "
           "blend";
}

const char *confidence_layer_note() {
    return "confidence: darker unknown = never described; amber hatch = "
           "in-window, below-rate; black hatch = outside the stated window";
}

const std::vector<OpClassSwatch> &opcode_class_swatches() {
    // Mirrors embedded.h's opClassHue[8] VERBATIM, in space::OpClass's own
    // declaration order — a colour added or reordered there and not here
    // fails test_layers' exhaustiveness check.
    static const std::vector<OpClassSwatch> kSwatches = {
        {space::OpClass::Unknown, "unknown: not classifiable (abstain)",
         {0.55f, 0.55f, 0.55f}},
        {space::OpClass::Move, "move", {0.35f, 0.75f, 0.95f}},
        {space::OpClass::IntArith, "integer arithmetic", {0.90f, 0.55f, 0.15f}},
        {space::OpClass::Logic, "logic / bit manipulation",
         {0.80f, 0.80f, 0.40f}},
        {space::OpClass::CompareBranch, "compare / branch",
         {1.00f, 0.35f, 0.35f}},
        {space::OpClass::ScalarFloat, "scalar float", {0.70f, 0.50f, 0.85f}},
        {space::OpClass::VectorSIMD, "vector / SIMD", {0.85f, 0.30f, 0.75f}},
        {space::OpClass::System, "system / privileged",
         {0.45f, 0.85f, 0.45f}},
    };
    return kSwatches;
}

const std::vector<ReliefSwatch> &relief_shape_swatches() {
    // The colours mirror data_layers_gl.cpp's relief_read_/relief_write_
    // literals VERBATIM — the same keep-in-sync convention
    // terrain_encoding_swatches follows against kTerrainFrag (that TU is the
    // GL half, which this one cannot include: D4/D9, no GL here). The LABELS
    // are not copied at all: they come from space::relief_shape_label(), so
    // the legend and the model can never state two different rules for the
    // same shape. A ReadWrite cell gets the read hue (its peak is what the
    // reader sees first); the None row is deliberately grey, the same abstain
    // colour OpClass::Unknown uses.
    static const std::vector<ReliefSwatch> kSwatches = {
        {space::ReliefShape::ReadOnly,
         space::relief_shape_label(space::ReliefShape::ReadOnly),
         {0.30f, 0.72f, 0.95f}},
        {space::ReliefShape::WriteOnly,
         space::relief_shape_label(space::ReliefShape::WriteOnly),
         {1.00f, 0.60f, 0.20f}},
        {space::ReliefShape::ReadWrite,
         space::relief_shape_label(space::ReliefShape::ReadWrite),
         {0.30f, 0.72f, 0.95f}},
        {space::ReliefShape::None,
         space::relief_shape_label(space::ReliefShape::None),
         {0.55f, 0.55f, 0.55f}},
    };
    return kSwatches;
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
    // 61 T5: the path no longer carries trace time — it lies flat and is read
    // through the playhead, which is what returns this axis to access density.
    // The three OPT-IN layers still put trace time here, so the axis can still
    // carry two meanings; it just does not do so in the default scene, and the
    // note says which case the reader is in rather than claiming both always.
    return "vertical = access density (log). trace time is the PLAYHEAD, not "
           "an axis: the path lies flat. the opt-in lifetime / ribbon / "
           "sediment layers do stand trace time on this axis (steps)";
}

std::string camera_here_line(SceneKind k, const space::Projection &proj,
                             float u, float v) {
    if (k == SceneKind::Plane)
        return camera_here_text(proj, u, v);
    // No address axis here at all, so there is no "here" to unproject. Name the
    // substrate's own vertical axis — the load-bearing one — instead of
    // reporting a plane address the camera target does not mean.
    return std::string("you are here: \"") + scene_kind_name(k) +
           "\" has no address axis — its vertical axis is " +
           std::string(scene_axes(k).y);
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
                // CamKey::Reset applies Camera::reset() — the literal default
                // pose, which the button row below calls "default view". 48 T4
                // is explicit that "reset view" (the landmark) and "default
                // view" are two buttons with two meanings, "neither silently
                // repurposed into the other"; naming the landmark here was
                // exactly that repurposing, for the keyboard-only analyst this
                // generated list exists to serve.
                lines.push_back("R: default view (the plane's default pose)");
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

// 61 T8: Component 1's actual deliverable — the floor names its own
// rectangles. The transform is draw_trajectory_ruler's, verbatim: one cam.mvp +
// mat4x4_mul_vec4, the behind-the-camera clip[3] <= 0 skip, the same NDC->screen
// mapping. The world point is the ground plane at the rect's centre,
// {u, 0, v}, rather than the ruler's {0, t*scale, 0}.
//
// The label takes its region's own swatch colour, so a floor label and its
// legend row agree by construction rather than by two literals staying in step.
void draw_atlas_labels(ImDrawList *draw_list, const Camera &cam, ImVec2 origin,
                       ImVec2 size, const space::Projection &proj) {
    if (draw_list == nullptr || size.x <= 0.0f || size.y <= 0.0f)
        return;
    const std::vector<space::AtlasLabel> labels = space::atlas_labels(proj);
    if (labels.empty())
        return; // Hilbert, or nothing big enough to read — both are no-ops
    float m[16];
    cam.mvp(m, size.x / size.y); // the SAME aspect the viewport rendered at
    for (const space::AtlasLabel &l : labels) {
        vec4 world = {l.u, 0.0f, l.v, 1.0f};
        vec4 clip;
        mat4x4_mul_vec4(clip, reinterpret_cast<vec4 *>(m), world);
        if (clip[3] <= 0.0f)
            continue; // behind the camera: cannot place this label
        const float ndc_x = clip[0] / clip[3], ndc_y = clip[1] / clip[3];
        const ImVec2 p(origin.x + (ndc_x * 0.5f + 0.5f) * size.x,
                       origin.y + (1.0f - (ndc_y * 0.5f + 0.5f)) * size.y);
        const space::Region &reg = proj.regions[l.region];
        const space::RegionStyle st = space::region_style(reg.kind);
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(st.r, st.g, st.b, 0.95f));
        const ImVec2 sz = ImGui::CalcTextSize(l.text.c_str());
        draw_list->AddText(ImVec2(p.x - sz.x * 0.5f, p.y - sz.y * 0.5f), col,
                           l.text.c_str());
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

void draw_scene_axes(SceneKind k) {
    const SceneAxes ax = scene_axes(k);
    ImGui::TextColored(kDim, "axes — X: %s", ax.x);
    ImGui::TextColored(kDim, "axes — Y: %s", ax.y);
    ImGui::TextColored(kDim, "axes — Z: %s", ax.z);
    // The DENIAL, in the warn colour rather than the dim one: what a vertical
    // axis is NOT is the claim a new substrate is most likely to be misread as
    // making, so it is not an aside (D7).
    ImGui::TextColored(kWarn, "%s", ax.y_not);
}

// T1 (59) step 5: the substrate selector. A COMBO, never a checkbox row —
// scenes do not compose, and a checkbox would advertise that they might.
static void draw_scene_kind_selector(HudState &s) {
    const std::vector<SceneKind> &kinds = all_scene_kinds();
    ImGui::TextUnformatted("scene:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("##scene-kind", scene_kind_name(s.kind))) {
        for (SceneKind k : kinds) {
            const size_t i = scene_kind_index(k);
            const std::string why =
                i < s.kind_unavailable.size() ? s.kind_unavailable[i] : "";
            const bool ok = why.empty();
            // An unavailable kind is offered DISABLED with its reason, never
            // omitted: "this recording cannot show that" is a fact worth
            // stating, and a silently missing entry teaches nothing.
            ImGui::BeginDisabled(!ok);
            if (ImGui::Selectable(scene_kind_name(k), k == s.kind) && ok) {
                s.req_kind_change = true;
                s.req_kind = k;
            }
            ImGui::EndDisabled();
            if (!ok) {
                ImGui::SameLine();
                ImGui::TextColored(kDim, "— %s", why.c_str());
            }
        }
        ImGui::EndCombo();
    }
    const size_t cur = scene_kind_index(s.kind);
    if (cur < s.kind_unavailable.size() && !s.kind_unavailable[cur].empty())
        ImGui::TextColored(kWarn, "%s", s.kind_unavailable[cur].c_str());
    draw_scene_axes(s.kind);
    if (s.kind != SceneKind::Plane)
        ImGui::TextColored(kDim,
                           "one scene at a time — layers compose on the "
                           "address plane only, never across substrates");
}

void draw_scene_hud(HudState &s, const space::TerrainModel &terr,
                    const space::TrajectorySet &traj) {
    ImGui::Begin("3D overview");

    // Report HUD keyboard focus (22 T2): the caller applies the camera keys when
    // the HUD OR the 3D viewport holds focus, so orbiting works from either.
    s.kbd_focus = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // T1 (59-standalone-scenes): which substrate, and what its axes mean —
    // FIRST, above the provenance chips, because everything below is read in
    // terms of it.
    draw_scene_kind_selector(s);
    ImGui::Separator();

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
    // T1 (58): the coarse-vs-rich chip USED to be drawn inline here, a second
    // spelling of the same fact ("coarse: no memory stream") that no test could
    // reach. placement_chips now owns both branches (and grades them with the
    // span/drop census beside them), so this loop draws it once — one source of
    // truth for the data rung, testable without an ImGui frame.
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
    // 61 T9: the reflow notice, surfaced the same way — computed by the shell
    // and stashed on the model, never derived in the HUD.
    if (!terr.proj.layout_note.empty())
        ImGui::TextColored(kDim, "%s", terr.proj.layout_note.c_str());
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
    // T4 (55) step 5: the compositing mode two users' screenshots might
    // otherwise silently disagree about. Stated UNCONDITIONALLY (not gated on
    // a layer toggle like confidence/opcode's notes below): it describes how
    // every translucent surface on this plane composites, whichever of them
    // happen to be switched on right now.
    ImGui::TextColored(kDim, "%s", translucency_mode_note());

    // --- 51 T4: the camera-distance entity budget's placard -----------------
    // Empty at NEAR. Otherwise it names the tier and every class the budget
    // removed, in the same colour and the same spirit as scrub_degrade_note()
    // — a silent drop reads as "there was nothing there".
    if (!s.lod_note.empty())
        ImGui::TextColored(kWarn, "%s", s.lod_note.c_str());

    // --- 51 T1/T2: SUBJECT — which thread / region / kind is being read -----
    // Kept visually separate from "layers:" above, and captioned with the one
    // line that keeps the two axes apart (T3 step 3).
    ImGui::Separator();
    ImGui::TextUnformatted("subject:");
    ImGui::TextColored(kDim, "%s", focus_axis_note());
    // T2 step 5: an active filter ALWAYS says so. A filtered plane that looks
    // like an unfiltered one is a false reading of the recording.
    {
        const std::string note = subject_filter_note(s.focus, terr.proj);
        if (!note.empty())
            ImGui::TextColored(kWarn, "%s", note.c_str());
    }

    // T1 step 4: the thread roster. One row per trajectory the weave carries —
    // including one the plane could not draw, which says so rather than being
    // omitted. Selecting a row sets focus.tid; the statistical residency row
    // is never selectable (a survey is an aggregate, not a thread).
    {
        const std::vector<ThreadRosterRow> roster =
            thread_roster(traj, terr.proj);
        if (roster.empty()) {
            ImGui::TextColored(kDim, "threads: none woven in this recording");
        } else {
            for (size_t i = 0; i < roster.size(); i++) {
                const ThreadRosterRow &row = roster[i];
                ImVec4 col{row.rgb[0], row.rgb[1], row.rgb[2], 1.0f};
                ImGui::ColorButton(("##tid" + std::to_string(i)).c_str(), col,
                                   ImGuiColorEditFlags_NoTooltip |
                                       ImGuiColorEditFlags_NoPicker,
                                   ImVec2(12, 12));
                ImGui::SameLine();
                char label[96];
                if (row.statistical)
                    std::snprintf(label, sizeof label,
                                  "survey residency (aggregate)##r%zu", i);
                else if (row.tid < 0)
                    std::snprintf(label, sizeof label,
                                  "single path (no tid)##r%zu", i);
                else
                    std::snprintf(label, sizeof label, "tid %d##r%zu",
                                  static_cast<int>(row.tid), i);
                if (row.focusable) {
                    const bool sel = s.focus.tid >= 0 && s.focus.tid == row.tid;
                    if (ImGui::Selectable(label, sel))
                        // Clicking the focused row clears the focus — the
                        // roster is the ONE control for both directions.
                        s.focus.tid = sel ? -1 : row.tid;
                } else {
                    // Never focusable, and it says why rather than looking
                    // like a broken control (T5's own rule, 47/48).
                    ImGui::TextDisabled("%s", label);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(
                            "a survey is a whole-recording aggregate, not a "
                            "thread — it is never focusable, and keeps its "
                            "normal weight while a thread is focused");
                }
                ImGui::SameLine();
                ImGui::TextColored(kDim, "%llu of %llu vertices placed",
                                   (unsigned long long)row.placed,
                                   (unsigned long long)row.vertices);
                if (!row.note.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(kWarn, "%s", row.note.c_str());
                }
            }
        }
    }

    // T2 step 4: kind checkboxes, reusing region_style()'s labels and colours
    // (via kind_label) so the filter UI and the plane share ONE palette.
    ImGui::TextUnformatted("kinds:");
    {
        static const space::Region::Kind kKinds[6] = {
            space::Region::Code, space::Region::Stack, space::Region::Heap,
            space::Region::Data, space::Region::Mmap,  space::Region::Unknown};
        for (space::Region::Kind k : kKinds) {
            const uint32_t bit = 1u << static_cast<uint32_t>(k);
            bool on = (s.focus.kind_mask & bit) != 0;
            ImGui::SameLine();
            space::RegionStyle st = space::region_style(k);
            ImGui::ColorButton(
                (std::string("##kc") + st.name).c_str(),
                ImVec4(st.r, st.g, st.b, 1.0f),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                ImVec2(10, 10));
            ImGui::SameLine();
            if (ImGui::Checkbox(kind_label(k), &on)) {
                if (on)
                    s.focus.kind_mask |= bit;
                else
                    s.focus.kind_mask &= ~bit;
            }
        }
    }

    // T2 step 4: the region combo, listing proj.regions by VERBATIM label —
    // the same rule the 48 T3 goto combo keeps, and a separate control from
    // it because "point the camera there" and "read only that" are different
    // intents.
    if (terr.proj.regions.empty()) {
        ImGui::TextDisabled("region filter: no regions in this recording");
    } else {
        ImGui::TextUnformatted("region:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(220.0f);
        std::string preview =
            s.focus.region >= 0 && static_cast<size_t>(s.focus.region) <
                                       terr.proj.regions.size()
                ? terr.proj.regions[static_cast<size_t>(s.focus.region)].label
                : std::string("(all regions)");
        if (ImGui::BeginCombo("##focus_region", preview.c_str())) {
            if (ImGui::Selectable("(all regions)", s.focus.region < 0))
                s.focus.region = -1;
            for (size_t i = 0; i < terr.proj.regions.size(); i++) {
                const space::Region &r = terr.proj.regions[i];
                char fallback[32];
                std::snprintf(fallback, sizeof fallback, "region@0x%llx",
                              static_cast<unsigned long long>(r.base));
                std::string name = r.label.empty() ? fallback : r.label;
                const bool sel = s.focus.region == static_cast<int>(i);
                if (ImGui::Selectable((name + "##fr").c_str(), sel))
                    s.focus.region = static_cast<int32_t>(i);
            }
            ImGui::EndCombo();
        }
    }

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
        camera_here_line(s.kind, terr.proj, s.cam_target_u, s.cam_target_v)
            .c_str());

    // 50 T2/T4: the flat views' selection, located onto this plane by its
    // address. Graded, never hidden (D7): active-but-unplaceable states the
    // reason verbatim; nothing selected says so plainly; a placed selection
    // offers "frame the selection" (T4) and, when ambiguous (the same
    // address revisited at several steps — a loop), says so rather than
    // implying it is the one true occurrence.
    // 59 T1: also PLANE ONLY. The highlight is a cell located by ADDRESS on
    // this plane (shell.cpp computes it from terr.proj), so "frame the
    // selection" on another substrate would fly its camera to a coordinate
    // pair that means nothing there.
    if (!hud_plane_nav_applies(s.kind)) {
        ImGui::TextColored(kDim,
                           "selection: located by address on the address "
                           "plane — switch to it to frame the selection");
    } else if (s.has_highlight) {
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
    // 59 T1: PLANE ONLY. Both this row and the region jump below move the
    // camera by plane coordinates (frame(goto_u, goto_v, ...)), which is
    // meaningless on a substrate whose extents were never chosen for them —
    // typing an address would teleport the lane prism's camera by a u/v pair
    // that means nothing there. Say what the substrate reads by instead.
    if (!hud_plane_nav_applies(s.kind)) {
        ImGui::TextColored(kDim,
                           "go to: addresses locate on the address plane; "
                           "\"%s\" is read along %s",
                           scene_kind_name(s.kind), scene_axes(s.kind).x);
    } else {
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
    } // end of the plane-only "go to" block (59 T1)

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
    // T4 (56): the opcode-class swatches, likewise shown only while the
    // layer is on — a code-only legend, so it stays out of the way for a
    // recording nobody is reading by instruction kind right now.
    if (s.layers.opcode) {
        for (const OpClassSwatch &sw : opcode_class_swatches()) {
            ImVec4 col{sw.rgb[0], sw.rgb[1], sw.rgb[2], 1.0f};
            ImGui::ColorButton(("##op" + sw.label).c_str(), col,
                               ImGuiColorEditFlags_NoTooltip |
                                   ImGuiColorEditFlags_NoPicker,
                               ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::TextUnformatted(sw.label.c_str());
        }
    }
    // T5 (56): the misprediction layer's own chrome — the STATISTICAL label
    // rides on its shared toggle wording (T1 step 3); this states the two
    // idioms plus the off-plane count, which must never be silently dropped.
    if (s.layers.mispred) {
        ImGui::TextColored(
            kDim, "%s",
            "branch (statistical): bias arcs + site columns, cool->amber = "
            "misprediction rate");
        if (s.mispred_off_plane > 0)
            ImGui::TextColored(kWarn, "%u branch endpoint(s) off-plane",
                               s.mispred_off_plane);
    }
    // T2 (58): the twin relief's own legend, shown only while it is on. The
    // note first (it states the ABSENT-surface rule, the one thing the picture
    // cannot say for itself), then one row per shape.
    if (s.layers.data_relief) {
        ImGui::TextColored(kDim, "%s", space::data_relief_note());
        for (const ReliefSwatch &sw : relief_shape_swatches()) {
            ImVec4 col{sw.rgb[0], sw.rgb[1], sw.rgb[2], 1.0f};
            ImGui::ColorButton(("##rlf" + sw.label).c_str(), col,
                               ImGuiColorEditFlags_NoTooltip |
                                   ImGuiColorEditFlags_NoPicker,
                               ImVec2(12, 12));
            ImGui::SameLine();
            ImGui::TextUnformatted(sw.label.c_str());
        }
        if (s.relief_undirected_cells > 0)
            ImGui::TextColored(kWarn,
                               "%u data cell(s) carry %llu byte(s) of traffic "
                               "with NO recorded direction — counted, drawn on "
                               "neither surface",
                               s.relief_undirected_cells,
                               (unsigned long long)s.relief_undirected_bytes);
    }
    // T3 (58): the working-set tide's own chrome, shown only while it is on:
    // the dwell knob (with its UNIT named on the control — a bare number was
    // the 2026-07-29 review's top finding), the live/cold split, and the
    // legend line the model itself produced.
    if (s.layers.working_set) {
        int win = static_cast<int>(s.tide_window > 0 ? s.tide_window : 1);
        ImGui::SetNextItemWidth(160);
        if (ImGui::SliderInt("dwell window (trace-time steps)", &win, 1,
                             static_cast<int>(s.nsteps > 1 ? s.nsteps : 1024))) {
            s.tide_window = static_cast<uint64_t>(win < 1 ? 1 : win);
            s.tide_window_changed = true;
        }
        if (!s.tide_legend.empty())
            ImGui::TextColored(kDim, "%s", s.tide_legend.c_str());
        ImGui::TextColored(kDim, "%u live cell(s), %u cold (faded watermark)",
                           s.tide_live_cells, s.tide_cold_cells);
    }
    // T4 (58): the lifetime pillars' own chrome — the label FIRST, because it
    // is the load-bearing sentence of that whole brief (these are observed
    // touches, not allocations) and a reader who sees the geometry before the
    // caveat has already drawn the wrong conclusion.
    if (s.layers.lifetime) {
        ImGui::TextColored(kDim, "%s", space::lifetime_pillar_label());
        if (s.lifetime_open_topped > 0)
            ImGui::TextColored(kBad,
                               "%u pillar(s) OPEN-TOPPED — a torn capture cut "
                               "the observation, not the object: the top is a "
                               "lower bound",
                               s.lifetime_open_topped);
    }
    // T5 (58): the access-order ribbon. Its note comes from the model
    // (space::data_ribbon_note) so the source population, the gap rule and the
    // leap rule are stated once and cannot drift; the extra line here is the
    // disambiguation against the access SPURS, which are a different layer
    // drawn in the same pane and would otherwise read as one thing.
    if (s.layers.data_ribbon) {
        if (!s.ribbon_legend.empty())
            ImGui::TextColored(kDim, "%s", s.ribbon_legend.c_str());
        if (s.layers.access_marks)
            ImGui::TextColored(kWarn,
                               "\"access order\" and \"access\" are two "
                               "DIFFERENT layers, both on: the ribbon is the "
                               "ORDER data was touched in, the spurs are which "
                               "instruction touched which datum");
    }
    // T6 (58): the sediment layer's note, which STATES THE BAND COUNT — the
    // brief's own requirement, because a coarsened column looks exactly like a
    // sparsely-hit one unless the reader is told which they are seeing.
    if (s.layers.sediment && !s.sediment_legend.empty())
        ImGui::TextColored(kDim, "%s", s.sediment_legend.c_str());
    // T2 (57): the kernel-crossing layer's own chrome. The dwell disclaimer
    // is UNCONDITIONAL while the layer is on — "the geometry claims no
    // duration" is exactly the kind of thing a reader will otherwise assume
    // from a shape that leaves the plane and comes back.
    if (s.layers.crossings) {
        if (!s.crossings_disabled_reason.empty()) {
            ImGui::TextColored(kWarn, "crossings: %s",
                               s.crossings_disabled_reason.c_str());
        } else {
            ImGui::TextColored(kDim, "crossings: %s",
                               space::CrossingLayer::dwell_note());
            ImGui::TextColored(kDim, "crossing anchor: %s",
                               space::CrossingLayer::anchor_label());
            if (s.crossings_before_first_insn > 0)
                ImGui::TextColored(
                    kWarn,
                    "%u syscall(s) precede every recorded instruction — not "
                    "anchored (anchoring them to instruction 0 would be a "
                    "fabrication)",
                    s.crossings_before_first_insn);
            if (s.crossings_off_plane > 0)
                ImGui::TextColored(kWarn, "%u crossing anchor(s) off-plane",
                                   s.crossings_off_plane);
        }
    }
    // T3 (57): the taint front. The AXIS disclaimer is unconditional while
    // the layer is on — a front that advances across a plane is exactly the
    // shape a reader will otherwise read as time.
    if (s.layers.taint) {
        if (!s.taint_disabled_reason.empty()) {
            ImGui::TextColored(kWarn, "taint: %s",
                               s.taint_disabled_reason.c_str());
        } else {
            ImGui::TextColored(kDim, "taint: %s",
                               space::TaintFront::axis_note());
            if (s.taint_bounded)
                ImGui::TextColored(kWarn, "taint: %s",
                                   space::TaintFront::bounded_note());
            if (s.taint_truncated)
                ImGui::TextColored(
                    kWarn,
                    "taint: the recording is truncated — the front's extent "
                    "is a stated lower bound");
            if (s.taint_unknown_steps > 0)
                ImGui::TextColored(
                    kWarn,
                    "%u reached step(s) the recording never described — "
                    "unknown, NOT \"the value did not go there\"",
                    s.taint_unknown_steps);
            if (s.taint_reg_only_writes > 0)
                ImGui::TextColored(
                    kDim,
                    "%u register-only write(s) tint nothing (no address to "
                    "place)",
                    s.taint_reg_only_writes);
            if (s.taint_off_relative_writes > 0)
                ImGui::TextColored(
                    kWarn,
                    "%u region-relative memory write(s) not placed (the wire "
                    "states no base for data)",
                    s.taint_off_relative_writes);
            if (s.taint_off_plane > 0)
                ImGui::TextColored(kWarn, "%u taint write(s) off-plane",
                                   s.taint_off_plane);
        }
    }
    // T4 (57): the blame convergence forest. The SET-OVERLAP disclaimer is
    // unconditional while the layer is on: a bright spike shared by several
    // cones is exactly the shape a reader will otherwise read as a link.
    if (s.layers.blame) {
        if (!s.blame_disabled_reason.empty()) {
            ImGui::TextColored(kWarn, "blame: %s",
                               s.blame_disabled_reason.c_str());
        } else {
            ImGui::TextColored(kDim, "blame: %s",
                               space::BlameForest::label());
            if (s.blame_single_cone)
                ImGui::TextColored(
                    kWarn,
                    "blame: this recording attributes ONE sink — there is "
                    "nothing to converge, so the layer is a single faint "
                    "bundle, not a finding");
            else
                ImGui::TextColored(kDim,
                                   "blame: %u cone(s), peak overlap %u",
                                   s.blame_cones, s.blame_max_weight);
            if (s.blame_born_untraced > 0)
                ImGui::TextColored(kWarn, "%u cone(s) %s",
                                   s.blame_born_untraced,
                                   space::BlameForest::born_untraced_label());
            if (s.blame_truncated)
                ImGui::TextColored(
                    kWarn,
                    "blame: the recording is truncated — every cone is a "
                    "lower bound, and a missing convergence is NOT SEEN");
            if (!s.blame_off_plane_note.empty())
                ImGui::TextColored(kWarn, "blame: %s",
                                   s.blame_off_plane_note.c_str());
        }
    }
    // T5 (57): the dominant-path ridge. The AGGREGATE disclaimer is
    // unconditional while the layer is on: a bright tube threaded through a
    // chain of blocks is exactly the shape a reader takes for "one run went
    // this way", and it is not that claim.
    if (s.layers.ridge) {
        if (!s.ridge_disabled_reason.empty()) {
            ImGui::TextColored(kWarn, "ridge: %s",
                               s.ridge_disabled_reason.c_str());
        } else {
            ImGui::TextColored(kDim, "%s",
                               space::PathRidge::aggregate_note());
            if (s.ridge_caps > 0)
                ImGui::TextColored(kWarn, "%u block(s): %s", s.ridge_caps,
                                   space::PathRidge::cap_note());
            if (s.ridge_truncated)
                ImGui::TextColored(
                    kWarn,
                    "ridge: the trace is truncated — every transition count "
                    "is a stated lower bound");
            if (s.ridge_unattributed_insns > 0)
                ImGui::TextColored(
                    kDim,
                    "%u instruction(s) seen before any recorded block — "
                    "counted, never attributed by containment",
                    s.ridge_unattributed_insns);
            if (s.ridge_blocks_unvisited > 0)
                ImGui::TextColored(
                    kWarn,
                    "%u recorded block(s) the instruction stream never "
                    "reached",
                    s.ridge_blocks_unvisited);
            if (s.ridge_off_plane > 0)
                ImGui::TextColored(kWarn, "%u ridge endpoint(s) off-plane",
                                   s.ridge_off_plane);
        }
        if (s.ridge_survey_edges > 0) {
            // Its OWN line, and the STATISTICAL wording verbatim: the survey
            // fallback is separate ink and separate evidence, never folded
            // into the exact counts above.
            ImGui::TextColored(kWarn, "%s", space::RidgeSurvey::label());
            ImGui::TextColored(kDim, "  %u survey edge(s), sampler %s",
                               s.ridge_survey_edges,
                               s.ridge_survey_sampler.empty()
                                   ? "(unstated)"
                                   : s.ridge_survey_sampler.c_str());
        }
    }
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
