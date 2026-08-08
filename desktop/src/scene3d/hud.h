// hud.h — the ImGui HUD drawn over the 3D scene (docs/internal/gui/
// 10-spacetime-3d-overview.md T4 step 4). A SEPARATE TU from scene.cpp so the GL
// scene links no ImGui and the HUD links no GL: the playhead, the layer toggles,
// the provenance chips (coarse-vs-rich, exact-vs-statistical, truncation) and the
// region legend are pure ImGui over the pure space/ models. Engine-free (D4).
//
// The HUD never draws GL and never mutates the models; it reports the user's
// intent back through HudState (a moved playhead, a camera preset) and the caller
// re-slices the terrain / rebuilds the trajectory and drives the Camera.
#ifndef ASMDESK_SCENE3D_HUD_H
#define ASMDESK_SCENE3D_HUD_H

#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h" // 49 T4: ImDrawList/ImVec2 in draw_trajectory_ruler's signature
#include "scene3d/camera.h" // 49 T4: draw_trajectory_ruler's projection
#include "scene3d/focus.h"  // 51 T1/T2: SceneFocus, the thread roster
#include "scene3d/lod.h"    // 51 T4: LodTier, the entity-budget placard
#include "scene3d/scene.h"
#include "space/datacell.h" // T2 (58): ReliefShape, ReliefSwatch::shape
#include "space/mnemonic.h" // T4 (56): OpClass, OpClassSwatch::cls
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// 36 T4: the placement-provenance chips, as PURE DATA so the fidelity chrome is
// testable without an ImGui frame — deleting a branch fails a named check. Bad is
// a refusal (rendered ALL CAPS, in the refuse colour); Warn is a graded fact
// (lowercase `label: explanation`, in the warn colour); Ok is a good state (green,
// the basis chip). draw_scene_hud renders these into the chip row; the tests
// assert their contents per scenario.
struct PlacementChip {
    enum Sev { Bad, Warn, Ok } sev;
    std::string text;
};
std::vector<PlacementChip> placement_chips(const space::TerrainModel &terr,
                                           const space::TrajectorySet &traj);

// 36 T4 defect 1: the BASIS chip as pure data, so its fallback is testable —
// reverting it must fail a named check. Returns a Bad chip for a mixed-basis
// refusal, an Ok chip for a valid basis (falling back to the TRAJECTORY's basis
// when the terrain canvas has none — the df-only case 25 T6 promised but the HUD
// never drew, because it keyed only on the canvas basis), or an empty `text`
// (Ok) when there is no basis to report.
PlacementChip basis_chip(const space::TerrainModel &terr,
                         const space::TrajectorySet &traj);

// 49 T3: one swatch per terrain-shader colour branch, as PURE DATA so the
// legend is EXHAUSTIVE BY TEST — a shader branch (kTerrainFrag, embedded.h)
// with no entry here is a colour the user cannot decode, and this list is
// what a test walks to catch that. `rgb` mirrors the GLSL literal it
// documents, with a keep-in-sync comment at each use — the same convention
// TORN/STAT/CHURN already follow against TerrainFlag. Deliberately excludes
// TF_READ/TF_WRITE: kTerrainFrag does not branch on them (no distinct colour
// to key).
struct EncodingSwatch {
    space::TerrainFlag flag;
    std::string label;
    float rgb[3];
};
const std::vector<EncodingSwatch> &terrain_encoding_swatches();

// T5 (47-scene-inspect-and-pickable-overlays): one swatch per PICKABLE
// OVERLAY LINE class (as distinct from terrain_encoding_swatches' per-cell
// terrain colours above) — the convergence arcs and access spurs T3 made
// pickable. `rgb` mirrors scene.cpp's Line::color literals VERBATIM, the
// same keep-in-sync convention terrain_encoding_swatches follows against the
// GLSL shader (scene.cpp is the GL half; this TU cannot include it, D4/D9 —
// no GL, no engine — so the colours are a second, deliberately synced copy,
// not a shared constant). The convergence caption states the mark's own
// fidelity grade verbatim (converge.h's ConvergenceSet::label(): a co-
// locality hint, never a proven race or order) — the brief's own T5 step 3,
// so a pickable arc still never reads as a stronger claim than a drawn one.
// EXHAUSTIVE BY TEST, exactly like terrain_encoding_swatches: every drawn
// overlay-line class (scene.cpp's set_convergences/set_trajectories) must
// have an entry here.
struct OverlaySwatch {
    std::string label;
    float rgb[3];
};
const std::vector<OverlaySwatch> &overlay_encoding_swatches();

// T5: the one-line advertisement that the pane is interrogable — "hover to
// inspect, click to open the flat reader" — a single source of truth between
// the legend draw call and the test that the wording didn't silently drift
// or vanish (vertical_axes_note's own pattern). Lives in the LEGEND (this
// function, drawn every frame), never the first-open-only primer
// (shell.cpp's dt_primer), so it stays reachable after the primer is
// dismissed.
const char *inspect_hint_note();

// 49 T3: the raw scale a height contour band is worth, formatted for the
// legend — "no data at this slice" when max_full_heat is 0. Pure so the
// wording is testable without an ImGui frame.
std::string height_scale_note(uint64_t max_full_heat);

// T4 (55-scene-render-quality) step 5: which compositing mode the stacked
// translucent surfaces (the ghost-fog terrain, a statistical trajectory) use
// — always "dithered" today (kStatFrag/kTrajFrag's Interleaved Gradient
// Noise discard); weighted-blended OIT is deferred (see that doc's own status
// note), so there is no "exact" branch to report yet. Stated unconditionally
// rather than silently, so two users comparing screenshots are never
// comparing two undisclosed algorithms.
const char *translucency_mode_note();

// T2 (56-fidelity-and-module-layers): the confidence layer's own legend line
// — never merged into terrain_encoding_swatches() above, because the two
// coverage-window hatches it names (space::TF_INWINDOW_EMPTY/TF_OUTWINDOW)
// render ONLY when SceneLayers::confidence is on, unlike that legend's four
// always-visible branches. Drawn only while confidence is enabled, so it
// never advertises an idiom the plane is not currently showing.
const char *confidence_layer_note();

// T4 (56-fidelity-and-module-layers): one swatch per space::OpClass, mirroring
// embedded.h's opClassHue[8] VERBATIM (the same keep-in-sync convention
// terrain_encoding_swatches follows against kTerrainFrag) — EXHAUSTIVE BY
// TEST, one entry per OpClass value. Shown only while SceneLayers::opcode is
// on, exactly like confidence_layer_note above.
struct OpClassSwatch {
    space::OpClass cls;
    std::string label;
    float rgb[3];
};
const std::vector<OpClassSwatch> &opcode_class_swatches();

// T2 (58-memory-data-cell-family): the twin relief's legend — one line per
// ReliefShape, in space::relief_shape_label()'s own words (a single source of
// truth with the model, never a second copy of the phrasing), plus the layer
// note. EXHAUSTIVE BY TEST over ReliefShape, exactly like
// opcode_class_swatches over OpClass. Shown only while
// SceneLayers::data_relief is on, and it names the ABSENT-surface rule, which
// is the one thing a reader cannot deduce from the picture: a missing surface
// means the direction was never recorded, not that it measured zero.
struct ReliefSwatch {
    space::ReliefShape shape;
    std::string label;
    float rgb[3];
};
const std::vector<ReliefSwatch> &relief_shape_swatches();

// 49 T4: tick values for the trajectory-time ruler, `0..nsteps` inclusive at
// a step no finer than `nsteps / max_ticks` — pure so the tick set is
// golden-testable without a camera-projected draw. Empty when nsteps == 0
// (the ruler is skipped entirely).
std::vector<uint64_t> trajectory_axis_ticks(uint64_t nsteps,
                                            int max_ticks = 10);

// 49 T4: the one-line statement of the two vertical meanings — a single
// source of truth between the draw call and the test that wording didn't
// silently drift or vanish.
const char *vertical_axes_note();

// 48 T4: "you are here" — a pure function of (Projection, target), so the
// wording is golden-testable without an ImGui frame. Names the region under
// the camera target (via Projection::unproject) and its address, or states
// plainly that the target sits outside the compacted domain (never a blank
// line: an oriented-but-unmapped camera is still a fact worth stating, D7).
std::string camera_here_text(const space::Projection &proj, float u, float v);

// 59 T1: the SAME line, but for whichever substrate the pane is showing. Only
// the address plane has an address under the camera; the other four have axes
// that are not addresses at all (a byte index inside a register, a call
// ordinal, a recording side), so unprojecting their camera target through the
// plane's Projection states an address nobody recorded — the exact D7
// fabrication this family exists to remove. For those, name the kind's own
// vertical axis instead of inventing a location. Pure, like its sibling.
std::string camera_here_line(SceneKind k, const space::Projection &proj,
                             float u, float v);

// 59 T1: do the PLANE-COORDINATE affordances belong on this substrate? The
// "go to <address>" row and the "frame the selection" button both move the
// camera by plane coordinates, which is meaningless on a substrate whose
// extents were never chosen for them.
inline bool hud_plane_nav_applies(SceneKind k) {
    return k == SceneKind::Plane;
}

// 48 T5: the FULL advertised control list, generated from the CamKey enum
// (never a hand-written string — the review's own finding is precisely that
// hand-written key lists drift from the bindings) plus the mouse gestures
// CamKey has no key for. One entry per CamKey value; adding a value without
// adding its line here fails test_camera's exhaustiveness check.
const std::vector<std::string> &scene_control_lines();

// 49 T4: draw the trace-time ruler into the 3D VIEWPORT (not the HUD window) —
// ticks 0..nsteps, projected through `cam` at world (0, tick * traj_scale, 0)
// (the plane's own origin corner) into `origin`/`size`'s screen rect. Never
// the terrain playhead — the two clocks stay visually separate, per the HUD
// note above. No-op when nsteps == 0. The caller (draw_scene_overview) only
// reaches this after a real GL frame rendered, so a null backend never calls
// it — the graceful degradation the pane already practises for the viewport.
//
// 61 T5: it no longer measures the TRAJECTORY, which has left this axis and
// now lies flat. What it measures is the axis the three OPT-IN layers still
// ride — the observed-lifetime pillars, the sediment strata and the access
// arcs — so shell.cpp gates the call on one of those being drawn. Calling it
// unconditionally would label an axis the default scene does not use.
void draw_trajectory_ruler(ImDrawList *draw_list, const Camera &cam,
                           ImVec2 origin, ImVec2 size, uint64_t nsteps,
                           float traj_scale);

// 61 T8: draw the atlas's region labels into the 3D VIEWPORT (not the HUD
// window), each at its rectangle's centre ON the ground plane, projected
// through `cam` into `origin`/`size`'s screen rect — the SAME world->clip
// transform draw_trajectory_ruler above uses. A no-op for a Hilbert projection,
// because space::atlas_labels refuses one (see projection.h), which is why the
// caller needs no layout gate.
//
// PARTIAL BY DESIGN: rects below the legibility threshold carry no label, and
// the HUD's region legend remains the complete list. Do not present what this
// draws as every region.
void draw_atlas_labels(ImDrawList *draw_list, const Camera &cam, ImVec2 origin,
                       ImVec2 size, const space::Projection &proj);

struct HudState {
    uint64_t t = 0;      // playhead: the inclusive terrain slice [0, t]
    uint64_t nsteps = 0; // the recording's step extent (the slider's max)
    SceneLayers layers;  // terrain / exact / statistical / access toggles

    // Set true by draw_scene_hud when the user moved the playhead this frame, so
    // the caller re-slices T2 and rebuilds T3 for the new t. The caller clears it.
    bool playhead_moved = false;
    // Camera preset requests. 48 T4 split "reset view" into two honest, distinct
    // meanings: req_reset_view now frames the LANDMARK (home_u/home_v, the code-
    // district centroid the caller computes once per weave), while
    // req_default_view restores the literal Camera{} — the documented preset
    // 25/34 already promised, kept rather than silently repurposed. The caller
    // applies both to its Camera and clears both afterward (the existing
    // req_top_down pattern), so a stale press cannot re-fire next frame.
    bool req_reset_view = false;
    bool req_default_view = false;
    bool req_top_down = false;
    // 48 T4: the landmark the "reset view" button frames — computed ONCE per
    // weave by the caller (SceneView::home_u/v) and synced here every frame, not
    // re-derived per frame (a home that drifts as events arrive on a live/growing
    // capture is worse than a fixed one, per the brief's own fidelity note).
    float home_u = 0.5f, home_v = 0.5f;
    bool has_home =
        false; // false => no code region placed; the button is a no-op
    // 48 T4: the camera target, synced by the caller every frame so the "you are
    // here" readout is a pure function of (terr.proj, target) — never a second
    // camera reference threaded through draw_scene_hud's signature.
    float cam_target_u = 0.5f, cam_target_v = 0.5f;
    // 48 T3: the "go to" row's intent — resolved INSIDE draw_scene_hud (which
    // already receives terr.proj) and reported back exactly like the camera
    // presets above, so the caller's Camera stays the one place that mutates.
    bool req_goto = false;
    float goto_u = 0.5f, goto_v = 0.5f, goto_radius = 2.2f;
    // 48 T2/T3/T4: the one-line refusal note ("nothing to recentre on here",
    // "address not mapped by any region", ...) for a recentre/goto that could
    // not be honoured — named verbatim, in the warn colour, never silent (D7).
    // Persists until the next attempt overwrites or clears it; the caller (a
    // double-click recentre, or "show in 3D" from find) may also set this.
    std::string nav_note;
    // 48 T3: the "go to address" text input's own buffer — owned by the HUD
    // (like FindState::query owns its own text) so the caller need not thread a
    // char[] through its own state for a control this local.
    char goto_addr_buf[64] = {0};
    // 48 T3: the region-goto combo's selection, an index into terr.proj.regions
    // (-1 = none chosen yet).
    int goto_region_sel = -1;

    // 34 T3: the terrain-time play/pause transport, same shape as the camera
    // presets. `playing` is set by the caller before the draw so the button reads
    // "Play"/"Pause"; `req_play_toggle` is the HUD's intent, which the caller
    // applies to SceneView::play and clears. The 3D playhead walks trace-residency
    // time, a DIFFERENT axis from the flat views' execution step (34 fidelity note).
    bool playing = false;
    bool req_play_toggle = false;

    // 50 T2/T4 (two-way-brushing): the flat views' selection, located onto
    // this plane by its ADDRESS — synced by the caller every frame from
    // space::Located (SceneView::highlight), the HUD never resolves it
    // itself. `has_highlight` false + a non-empty `highlight_reason` means
    // the selection is active but could not be placed ("unplaceable, because
    // X" — D7, never silently absent); both false/empty means nothing is
    // selected in this recording at all.
    bool has_highlight = false;
    bool highlight_ambiguous = false;
    std::string highlight_reason;
    // T4: "frame the selection" — enabled only when has_highlight; the
    // caller applies it to Camera::frame on the located cell and clears it,
    // the same one-shot-intent pattern req_reset_view/req_goto already use.
    bool req_frame_highlight = false;

    // Set true by draw_scene_hud when the HUD window holds keyboard focus this
    // frame (22-selection-and-search.md T2, F18). The caller ORs it with the 3D
    // viewport's focus to decide whether the arrow/dolly/reset camera keys act, so
    // a keyboard-only analyst can orbit the scene from either the HUD or the
    // viewport — the accessibility substitute for ImGui's absent screen-reader tree.
    bool kbd_focus = false;

    // T5 (56-fidelity-and-module-layers): synced by the caller every frame
    // from space::MispredLayer::off_plane — an edge endpoint the projection
    // could not place is COUNTED, never silently dropped (T5 step 3's own
    // bar). Shown only while SceneLayers::mispred is on.
    uint32_t mispred_off_plane = 0;

    // 51 T1/T2 (scene-focus-and-scale): the SUBJECT filter, sitting beside
    // `layers` above and deliberately NOT merged into it — layers grade
    // EVIDENCE, this chooses SUBJECT (focus_axis_note()). Owned here (like
    // `layers`) so it persists per recording through SceneView, and read
    // straight out by the caller into SceneFrame each frame.
    SceneFocus focus;
    // 51 T4: the distance/density tier this frame landed in, and the placard
    // that names what it is therefore NOT drawing. Both are computed by the
    // caller (which owns the Camera and the model counts) and synced here, the
    // same one-way flow `playing`/`cam_target_u` already use — the HUD states
    // the tier, it never decides it. An empty `lod_note` means NEAR: nothing
    // was dropped, so there is nothing to disclose.
    LodTier lod = LodTier::Near;
    std::string lod_note;
    // T2 (58-memory-data-cell-family): synced by the caller every frame from
    // space::DataReliefLayer — traffic whose direction was never recorded is
    // drawn on NEITHER surface, so it would otherwise vanish between the
    // terrain (which counts it in cum_size) and this layer. Stated instead.
    // Shown only while SceneLayers::data_relief is on.
    uint32_t relief_undirected_cells = 0;
    uint64_t relief_undirected_bytes = 0;

    // T3 (58-memory-data-cell-family): the working-set DWELL WINDOW, in
    // TERRAIN-TIME steps — the playhead's own axis (`t` above), NEVER the
    // execution step the flat views brush (34-playhead-and-scene-reach's
    // two-axes rule). The HUD owns the control; the caller reads it back and
    // rebuilds the tide, exactly as it does for `t`. `tide_window_changed` is
    // the one-shot intent, cleared by the caller like playhead_moved.
    uint64_t tide_window = space::kTideWindowDefault;
    bool tide_window_changed = false;
    // Synced by the caller from space::WorkingSetTide so the HUD can state the
    // live/cold split and the tint provenance without rebuilding the layer.
    uint32_t tide_live_cells = 0, tide_cold_cells = 0;
    std::string tide_legend; // space::tide_note(tide), built by the caller

    // T4 (58-memory-data-cell-family): how many lifetime pillars are OPEN-
    // TOPPED (a torn capture cut the observation at the tail, so the interval's
    // top is a lower bound). Synced by the caller; shown only while
    // SceneLayers::lifetime is on, in the refuse colour.
    uint32_t lifetime_open_topped = 0;

    // T5 (58-memory-data-cell-family): the access-order ribbon's own legend
    // line, built by the caller from space::data_ribbon_note(ribbon) — it
    // names the SOURCE population (`mem` vs dataflow operands, which are NOT
    // the same set of accesses), the gap rule and the leap rule. Shown only
    // while SceneLayers::data_ribbon is on.
    std::string ribbon_legend;

    // T6 (58-memory-data-cell-family): the sediment layer's own legend line,
    // built by the caller from space::sediment_note(cols) — it STATES THE BAND
    // COUNT (T6 step 6's own requirement) and whether the scene budget
    // coarsened it. Shown only while SceneLayers::sediment is on.
    std::string sediment_legend;
    // T1 (59-standalone-scenes): WHICH substrate the pane is showing. The HUD
    // owns the selector (it already owns every other "what am I looking at"
    // control) and reports the change back exactly like the camera presets
    // above — `kind` is synced by the caller each frame, `req_kind` is the
    // one-shot intent the caller applies and clears.
    //
    // ONE AT A TIME: this is a selector, never a set of checkboxes. Scenes do
    // not compose — SceneLayers above is the additive mechanism of the ADDRESS
    // PLANE, and two substrates with different axes drawn together would put
    // two meanings on one screen position.
    SceneKind kind = SceneKind::Plane;
    bool req_kind_change = false;
    SceneKind req_kind = SceneKind::Plane;
    // The one-line reason a kind is unavailable for this recording (no second
    // recording to diverge against, no region capture, no call tree, no wide
    // register writes). Synced by the caller; a kind with a reason is offered
    // DISABLED with the reason beside it, never silently missing.
    std::vector<std::string> kind_unavailable =
        std::vector<std::string>(all_scene_kinds().size());
    // --- 57-causal-layers: what each causal layer could not draw -------------
    // Synced by the caller every frame from the layer models. A causal layer
    // that silently drew less than the data contains would be worse than one
    // that refused, so every one of these is stated rather than inferred from
    // an absence (57's own fidelity note).
    //
    // T2: the kernel-crossing spurs. `crossings_disabled_reason` non-empty
    // means the layer self-gated (no worldline, or no stream order) and the
    // HUD says which; `crossings_before_first_insn` counts the syscalls that
    // precede every recorded instruction and were therefore NOT anchored (to
    // instruction 0 or anywhere else).
    std::string crossings_disabled_reason;
    uint32_t crossings_before_first_insn = 0;
    uint32_t crossings_off_plane = 0;

    // T3: the taint front. Every one of these is a thing the front could NOT
    // draw, and each is a DIFFERENT reason — a register write with no
    // address, a region-relative write with no stated data base, a step the
    // recording never described, an address the plane does not map. Collapsing
    // them into one number would lose exactly the distinction the layer
    // exists to preserve.
    std::string taint_disabled_reason;
    bool taint_bounded = false;
    bool taint_truncated = false;
    uint32_t taint_reg_only_writes = 0;
    uint32_t taint_off_relative_writes = 0;
    uint32_t taint_unknown_steps = 0;
    uint32_t taint_off_plane = 0;

    // T4: the blame convergence forest. `blame_single_cone` is what makes
    // "degrade honestly" visible: a recording that blames one sink has
    // nothing to converge, and the HUD says so rather than letting a faint
    // baseline read as a weak finding.
    std::string blame_disabled_reason;
    bool blame_single_cone = false;
    bool blame_truncated = false;
    uint32_t blame_cones = 0;
    uint32_t blame_born_untraced = 0;
    uint32_t blame_max_weight = 0;
    std::string blame_off_plane_note;

    // T5: the dominant-path ridge. `ridge_caps` is the count of blocks whose
    // successor was never recorded — an UNKNOWN continuation, stated, because
    // a capped ridge and a ridge that simply ends look identical otherwise.
    std::string ridge_disabled_reason;
    bool ridge_truncated = false;
    uint32_t ridge_caps = 0;
    uint32_t ridge_forks = 0;
    uint32_t ridge_off_plane = 0;
    uint32_t ridge_unattributed_insns = 0;
    uint32_t ridge_blocks_unvisited = 0;
    // The SURVEY fallback's own count and sampler — reported separately from
    // everything above, because it is a different kind of evidence and this
    // HUD must never let the two totals read as one number.
    uint32_t ridge_survey_edges = 0;
    std::string ridge_survey_sampler;
};

// T1 (59-standalone-scenes) step 4: draw the axis block for a kind — one line
// per axis plus the vertical axis's DENIAL, straight from
// scene3d::scene_axes(). Split out of draw_scene_hud so the pane can draw it
// on the null-backend/no-GL path too (where there is no viewport but the axes
// are still what the scene claims), and so a test can call it without
// standing up the whole HUD. Renders nothing else.
void draw_scene_axes(SceneKind k);

// The title of the top-level window draw_scene_hud opens. The HUD is its own
// window (not a child of the 3D tab that hosts the viewport), so the dock layout
// has to name it to place it — and a name written twice is a name that drifts.
// It lives HERE, beside the ImGui::Begin that uses it, and ui/layout.cpp imports
// it: scene3d/ must not depend on ui/, so the arrow can only point this way.
// test_layout pins that kPaneScene3D and this are the same string.
extern const char *const kHudWindow;

// Draw the HUD for the current ImGui frame. `terr`/`traj` supply the provenance
// and the legend; nothing here is mutated.
void draw_scene_hud(HudState &s, const space::TerrainModel &terr,
                    const space::TrajectorySet &traj);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_HUD_H
