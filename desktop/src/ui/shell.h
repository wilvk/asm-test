// shell.h — the visible skeleton (03-desktop-shell.md T6): a home screen with
// the three doors, a recording-open dialog, and a tab strip over the Workspace.
// draw_shell is backend-free (pure ImGui immediate calls over ShellState), so
// the null backend drives it headlessly in tests. Real views land in docs 04-09.
#ifndef ASMDESK_UI_SHELL_H
#define ASMDESK_UI_SHELL_H

#include <optional>
#include <string>
#include <vector>

#include "imgui.h" // ImGuiID (the published dockspace id, 19)

#include "analysis/stepindex.h"
#include "doc/streams.h"
#include "doc/workspace.h"
#include "doc/workspace_state.h" // WorkspaceState / FilterPreset / recents (20 T3/T4)
#include "live/inspect.h" // FeedbackInputs, for the toast-transition tracker (16 T1)
#include "loom/loom_draw.h"
#include "nav.h"
#include "platform/window_picker.h" // PickedWindow (doc 45 T7/T8) — X11-only, D4
#include "scene3d/atmosphere.h" // T3 (44): Atmosphere, SceneView::atmo
#include "scene3d/camera.h"
#include "scene3d/hud.h"
#include "scene3d/pick.h" // T1/T2 (47): PickHint, SceneView::hover_hint
#include "space/canopy.h" // ModuleCanopy (56 T3): SceneView::canopies
#include "space/converge.h"
#include "space/datacell.h" // DataReliefLayer (58 T2): SceneView::relief
#include "space/dataribbon.h" // DataRibbon (58 T5): SceneView::ribbon
#include "space/locate.h" // Located, SceneView::highlight (50 T2)
#include "space/crossing.h" // CrossingLayer (57 T2): SceneView::crossings
#include "space/blameforest.h" // BlameForest (57 T4): SceneView::blame
#include "space/ridge.h"       // PathRidge (57 T5): SceneView::ridge
#include "space/taint.h"    // TaintFront (57 T3): SceneView::taint
#include "space/opcode_terrain.h" // CellOpcode (56 T4): SceneView::opcode_cells
#include "space/sediment.h" // SedimentColumns (58 T6): SceneView::sediment
#include "space/terrain.h"
#include "space/trajectory.h"
#include "ui/doors.h"
#include "ui/fidelity.h" // T3 (44): FidelityTier, scene_atmosphere_for_tier
#include "ui/find.h"     // global find model (22 T3)
#include "ui/mode.h"     // task-language modes (20 T2)
#include "ui/primer.h"   // first-open primer state (24 T5)
#include "ui/scene_host.h"
#include "ui/selection.h" // shared brushing-and-linking selection (22 T1)
#include "ui/settings.h"  // user text-scale / theme / window size (20 T5)
#include "ui/transport.h" // play/pause transport + playhead projection (34)
#include "ui/undo.h"      // app-level command/undo stack (22 T4)
#include "ui/view_presence.h" // ViewId — the want_view_id tab intent (34 T2)
#include "views/completeness.h"
#include "views/hotedges.h" // HotEdgeSceneView (56 T2/T5): SceneView::hotedges_scene
#include "views/observer_draw.h"
#include "views/timeline.h" // dt_timeline — shell_timeline_model's return type
#include "walkthrough.h"

#include <map>

namespace asmdesk {

// The 3D spacetime overview's per-recording state (10-spacetime-3d-overview.md —
// the integration surfacing pass). The pure, engine-free space/ models are built
// LAZILY on first view of the recording's 3D tab (a recording whose overview is
// never opened pays nothing), then cached; `cam` + `hud` are the tab's interactive
// state, persisted per recording so switching tabs holds each view's camera and
// playhead. The GL scene itself is drawn by ShellState::scene_host — absent under
// the null test backend, where this state and the HUD are the whole pane.
struct SceneView {
    bool built = false;      // the models below are woven for this recording
    bool has_regions = false; // codeimage/maps placed at least one region
    space::TerrainModel terr;
    space::TrajectorySet traj;
    space::ConvergenceSet conv;
    space::Terrain slice;          // the cached terrain slice for `slice_t`
    uint64_t slice_t = UINT64_MAX; // the t `slice` was cut at (invalid => none)
    // 56 T3: the per-module canopies at THIS SAME slice_t — recomputed
    // alongside `slice` (raw_heat is t-gated the same way), never on a
    // playhead move that only re-lands the coarse degrade plane.
    std::vector<space::ModuleCanopy> canopies;
    // 58 T2: the read/write twin relief at THIS SAME slice_t — recomputed
    // alongside `slice`/`canopies` (its two prefix sums are t-gated the same
    // way), never on a degrade frame that only re-lands the coarse plane.
    space::DataReliefLayer relief;
    // 58 T3: the working-set tide at THIS SAME slice_t, with the HUD's dwell
    // window. Rebuilt when EITHER the playhead or the window moves — the
    // window is part of the layer's definition, so a stale one would draw a
    // recency claim the reader did not ask for.
    space::WorkingSetTide tide;
    uint64_t tide_window = space::kTideWindowDefault;
    // 58 T4: the observed-lifetime pillars — a WHOLE-RECORDING aggregate (a
    // pillar spans first..last touch regardless of the playhead), so it is
    // woven ONCE per recording alongside terr/traj, exactly like opcode_cells.
    space::LifetimePillars lifetime;
    // 58 T5: the data-access order ribbon — also a whole-recording aggregate
    // (every recorded access, in step order), woven once with the models above.
    space::DataRibbon ribbon;
    // 58 T6: the residency sediment columns — whole-recording (the layer's
    // point is to be readable with the playhead STATIONARY), so woven once
    // alongside the models above, and built against the SAME scrub cell budget
    // rather than a throttle of its own.
    space::SedimentColumns sediment;
    // 56 T4: the per-cell opcode classification — woven ONCE per recording
    // alongside terr/traj above (a cell's instruction mix is a whole-
    // recording fact, never re-gated on the playhead, unlike canopies).
    std::vector<space::CellOpcode> opcode_cells;
    // The 3D scrub's degrade-to-coarse state (23-graded-truth-layer.md T4):
    // `scrub_pending` marks a re-slice deferred because the full slice would
    // exceed the frame budget — the coarse plane renders this frame and the full
    // slice lands next, instead of a silent UI-thread stall.
    bool scrub_pending = false;
    scene3d::Camera cam;
    scene3d::HudState hud;
    bool nav_dragging = false; // a left-drag is orbiting (suppresses the pick)

    // T1 (47-scene-inspect-and-pickable-overlays): the throttled hover-pick
    // state. `hover_px` is the viewport-local pixel the LAST hover pick ran for
    // (a sentinel far off-screen so the very first frame always runs one);
    // `hover_id` is the raw id that pick returned — persisted so re-picking
    // runs only when the cursor pixel actually moves and no drag is in
    // progress (an orbit must cost zero readbacks, the brief's own risk to
    // guard against). `hover_hint` (T2) is the resolved PickHint the tooltip
    // (T4) reads without re-reading the FBO every frame.
    ImVec2 hover_px = ImVec2(-1.0f, -1.0f);
    uint32_t hover_id = 0;
    scene3d::PickHint hover_hint;
    // Did the Tab-reachable 3D viewport hit-target hold focus last frame (22 T2)?
    // ORed with the HUD focus to decide whether the keyboard camera acts; persisted
    // because the target is drawn after the camera keys are applied.
    bool viewport_focus = false;
    // 34 T3: the terrain-time play/pause transport, per recording so switching
    // tabs holds each scene's playback state. Advances hud.t over terr.nsteps —
    // the trace-residency axis, distinct from the execution step the flat views
    // brush (the brief's fidelity note).
    Transport play;
    dt_primer_state primer; // the first-open primer (24 T5), per recording

    // T3 (44-faithful-city-phase-a): the DAMPED weather-sky config, persisted
    // per recording so switching tabs holds the current sky rather than
    // snapping. draw_scene_overview lerps this toward
    // scene_atmosphere_for_tier(...)'s target every frame and passes the
    // result straight through via SceneFrame.atmo; Scene itself does no
    // damping (scene3d/atmosphere.h's own doc comment).
    scene3d::Atmosphere atmo;
    bool atmo_inited = false; // first weave snaps atmo to its target, no lerp

    // T5/T6 (44-faithful-city-phase-a): the SECOND, independent playhead over
    // TrajPoint.t's own per-tid axis — the "followed citizen" (T6). THE
    // AXIS-MISMATCH DECISION (read before extending this code, per the brief's
    // own instruction): `Selection.step` is a dataflow-step index (the flat
    // views' execution-step axis, ui/transport.h's playhead_project) while
    // `TrajPoint.t` is a per-tid trace-vertex COUNTER (space/trajectory.cpp,
    // `next_t[tid]++`) — the two do NOT coincide in general for a multi-tid
    // recording. This brief adopts the SAFE default: follow_step walks
    // TrajPoint.t as an INDEPENDENT playhead, advanced by its own Transport
    // (follow_play) exactly like hud.t walks terrain-time — it is NEVER
    // seeded from the shared Selection here (unlike the Scrubber's
    // playhead_project seed, ui/transport.h:72-78 / shell.cpp's execution-step
    // pane), because a reliable step->TrajPoint.t mapping cannot be
    // established in general (it would require verifying a single-tid,
    // trace-ordinal recording — not attempted in Phase A). follow_step
    // free-runs on Play/Pause alone. A later phase may add a real
    // Selection.step -> TrajPoint.t resolver and cross-brush where it is
    // verified sound (43's roadmap); this brief does not.
    Transport follow_play;
    uint64_t follow_step = 0;

    // 48 T4: the landmark — the code-district centroid (scene3d::
    // scene_home_target), computed ONCE per weave alongside terr/traj above,
    // never re-derived per frame (stable across live growth by construction:
    // a founding region's placement does not move as new events arrive).
    // has_home is false when the recording places no code region at all; the
    // "reset view" button is then a documented no-op rather than a silent
    // jump to the plane centre.
    float home_u = 0.5f, home_v = 0.5f;
    bool has_home = false;

    // 50 T2 (two-way-brushing): the shared Selection, located onto this plane
    // by its ADDRESS (space/locate.h) — recomputed only when the selection or
    // the weave changes, never every frame regardless of picking activity.
    // `highlight_epoch`/`highlight_gen` are the cache key (Selection::epoch,
    // the recording's event_count()); UINT64_MAX on both means "never
    // resolved yet", so the very first frame always resolves.
    space::Located highlight;
    uint64_t highlight_epoch = UINT64_MAX;
    uint64_t highlight_gen = UINT64_MAX;

    // 52 T3 (flat-terrain-surface.md): the GL-free flat surface's own
    // "reading mode" toggle for the GL path — a plain per-recording bool
    // (not routed through scene3d::HudState, which is mid-edit elsewhere on
    // this tree) so checking it swaps the flat surface in for the viewport
    // image; the three degraded branches draw the surface unconditionally,
    // regardless of this flag.
    bool flat_view = false;

    // 56 T2/T5 (fidelity-and-module-layers): the survey's scene-facing extract
    // — woven once per recording alongside terr/traj/conv above (a survey
    // aggregate does not change with the playhead), shared by T2's
    // coverage-window mask and T5's misprediction layer so both read the
    // SAME `have_window`/`window_base`/`window_len` fidelity fields rather
    // than building two HotEdgeViews that could disagree.
    HotEdgeSceneView hotedges_scene;
    // 56 T5: the misprediction survey layer's plane-space geometry, built
    // from hotedges_scene alongside it (same weave-time cadence).
    space::MispredLayer mispred;

    // 51 T2 (scene-focus-and-scale): the focused REGION's per-cell mask, the
    // one part of the subject filter a fragment shader cannot derive
    // (scene3d::build_focus_mask). Cached on (focused region, weave), never
    // rebuilt per frame or on a playhead scrub — the same "recompute only
    // when its inputs changed" discipline `highlight` above keeps.
    // `focus_mask_region` is -2 until the first build (distinct from -1, a
    // live "no region focused"); `focus_mask_gen` is the recording's
    // event_count, because a grown weave means a new Projection.
    std::vector<uint8_t> focus_mask;
    int32_t focus_mask_region = -2;
    uint64_t focus_mask_gen = UINT64_MAX;
    // --- T1 (59-standalone-scenes): the non-plane substrates ---------------
    // Which one this pane is showing (the HUD's selector reports changes back
    // through hud.req_kind), plus the pure models for the four. They are woven
    // ALONGSIDE the plane models above and cached the same way — a pane that
    // never switches kind pays for them once, and a kind switch is then a
    // pointer change rather than a re-weave.
    //
    // ONE AT A TIME: `kind` selects; nothing composes. The plane's models stay
    // built regardless, so switching back is instant and byte-identical.
    scene3d::SceneKind kind = scene3d::SceneKind::Plane;
    scene3d::DivergenceScene divergence;  // T2
    // Which B-side recording `divergence` was built against ("" = none). The
    // user can attach/detach a B side at any time, so this one model is
    // rebuilt when the pairing changes rather than on the weave gate.
    std::string div_b;
    scene3d::InvocationScene invocation;  // T3
    // `module_ribbon`, not `ribbon`: 58 T5 owns `ribbon` above for the
    // data-access ribbon. See the same note in scene_host.h.
    scene3d::ModuleRibbonScene module_ribbon; // T4
    scene3d::LanePrismScene prism;        // T5
    // T5: which register the prism is of, and the wide registers this
    // recording actually wrote (the selector's options). A prism is of ONE
    // register — two on one X axis would be two meanings on one position.
    uint32_t prism_reg = 0;
    std::vector<uint32_t> prism_regs;
    // Per-kind camera, so switching substrates does not carry the plane's
    // orbit into a scene whose extents it was not chosen for (and switching
    // back restores what you had). Index by scene_kind_index().
    std::vector<scene3d::Camera> kind_cam;
    std::vector<char> kind_cam_inited;
    // 57-causal-layers: the four layers of CAUSE, all woven once per
    // recording alongside terr/traj/conv above (a def-use cone, a blame set
    // overlap and a block-transition histogram are whole-recording facts, not
    // playhead-gated ones).
    space::CrossingLayer crossings; // T2
    // T3: the taint front, and the origin it was built for. Unlike the other
    // three this one depends on a CHOSEN origin (blame where the recording
    // has it, else the flat views' Selection), so it is recomputed on the
    // same epoch/growth gate `highlight` uses rather than only at weave time.
    space::TaintFront taint;
    bool taint_has_origin = false;
    uint32_t taint_origin = 0;
    uint64_t taint_epoch = UINT64_MAX;
    uint64_t taint_gen = UINT64_MAX;
    // T4: the blame convergence forest — a whole-recording set overlap, so
    // woven once with terr/traj/conv above.
    space::BlameForest blame;
    // T5: the dominant-path ridge and, SEPARATELY, its survey fallback. Two
    // members, never one: the exact transition histogram and the sampled
    // edges are different evidence and must not share a container.
    space::PathRidge ridge;
    space::RidgeSurvey ridge_survey;
};

struct ShellState {
    Workspace ws;
    int active_tab = -1;        // -1 = home (the three doors)
    bool open_dialog = false;   // the recording-open dialog is showing
    bool layout_inited = false; // the default dock layout was built once (T2)
    // The DockSpaceOverViewport id, published each frame by the docked shell (19)
    // so the View menu — and the tests — can rebuild presets against it. 0 until
    // the first docked frame (the non-docked path never sets it).
    ImGuiID dockspace_id = 0;
    char open_path[1024] = {0}; // its InputText buffer
    std::string open_error;     // last open failure, rendered verbatim

    // --- 20 T2: the task-language entry rail --------------------------------
    // The active task mode. Defaults to Learn (the dependency-free landing), so
    // an empty workspace auto-lands there (T2 step 4). Selecting a mode sets
    // `pending_preset`, the seam the docked frame applies to the dockspace so the
    // label and the pane arrangement never disagree (test_shell asserts this
    // field rather than the DockBuilder tree — test_layout covers that).
    Mode mode = Mode::Learn;
    std::optional<LayoutPreset> pending_preset;

    // --- 20 T3/T4: the persisted workspace state ----------------------------
    // The MRU recents (most-recent first), surfaced on the rail and in File ▸
    // Open Recent; `perspectives` are named dock arrangements and `presets` named
    // filter/query strings, all round-tripped through the WorkspaceState store.
    // `ws_dirty` asks main.cpp to flush the store after an open/close.
    std::vector<std::string> recents;
    std::map<std::string, std::string> perspectives;
    std::vector<FilterPreset> presets;
    bool ws_dirty = false;
    char persp_name[64] = {0};  // "Save perspective as…" name buffer
    char preset_name[64] = {0}; // "Save filter preset as…" name buffer

    // --- 20 T5: user display settings ---------------------------------------
    // The Settings pane's model (text-scale, content/DPI scale, window size,
    // light theme). `settings_dirty` asks main.cpp to re-bake the atlas / persist.
    Settings settings;
    bool show_settings = false;
    bool settings_dirty = false;
    // Help ▸ About — a standalone "what is this app" window, toggled from the
    // menu bar (its own section) exactly as show_settings/show_help are.
    bool show_about = false;
    // Help ▸ This host — the capability panel now lives in a collapsed "Details"
    // section under Home (no longer an Inspector tab). This one-frame intent asks
    // draw_home_rail to expand that section (and the docked shell to focus the
    // Home pane), so the menu item reaches it without a duplicate surface.
    bool want_details = false;

    // --- the replay views (04-replay-views.md) ---------------------------
    // Decoded once per open recording, parallel to ws.recordings: the builders
    // are pure functions of these, so nothing below re-parses JSON per frame.
    std::vector<Streams> streams;
    // Plan D3: every view takes one OR two recordings. `b_index` is the
    // attached B side (the `d` binding), -1 for none.
    int b_index = -1;
    dt_view view = dt_view::canvas;
    // The ONE shared brushing-and-linking selection (22 T1, F7): a pick in any
    // pane writes it and every pane reads its projection, so the same entity
    // cross-highlights everywhere it appears (detail / disasm / Loom / 3D) and
    // ONLY there. DISTINCT from nav.current, which points a view (a plain view
    // switch sets nav; a pick sets both). The cone fields below are a DERIVED
    // highlight OF this selection, keyed on `step` — not a second selection.
    Selection selection;
    // The lit cones, when a slice is active; cleared by `c`.
    bool cone_active = false;
    // Which cone `b`/`f` lit (17-T1 keymap): false = backward (what produced
    // this), true = forward (what this feeds). Read by the timeline/slice draw.
    bool cone_fwd = false;
    // The `1`/`2`/`3`/`4` view-switch intent (17-T1 keymap). Consumed by the
    // view tab bar with ImGuiTabItemFlags_SetSelected, exactly like `want_loom`
    // below — a keypress cannot select an ImGui tab directly, only ask the tab
    // to select itself next frame. Cleared once honoured.
    std::optional<dt_view> want_view;
    // 34 T2: the `5` / "View in 3D" intent to select a hostable tab that has NO
    // dt_view spelling (the 3D overview; and any future non-router view). Mirrors
    // want_view but keyed on ViewId — honoured with SetSelected, routed to the
    // "unavailable views" affordance when absent, and consumed the same frame.
    std::optional<ViewId> want_view_id;
    // 34 T3: the execution-step play/pause transport. Advances the shared
    // selection.step once per frame in draw_shell (over the active recording's
    // dataflow step space), so the timeline / slice / Loom / Scrubber animate
    // together. The 3D overview's terrain-time transport is a SEPARATE clock on
    // SceneView::play — a different axis, deliberately not chained to this one.
    Transport play;
    // The `Ctrl+G` go-to-step/offset modal (17-T1 keymap): open flag + its
    // InputText buffer. The modal parses the text with dt_nav_parse and jumps
    // via dt_nav_go, so a typed target lands exactly like a clicked link.
    bool show_goto = false;
    char goto_buf[64] = {0};
    // The `Ctrl+Shift+P` / `Ctrl+P` command palette (21-spine-navigation.md T1):
    // open flag + its query buffer, beside show_goto/goto_buf. The table
    // (build_palette) is a pure function of ShellState; every entry dispatches
    // through this same nav table (or the exact want_view/show_* intent the
    // keymap uses), so a palette command and a keypress are one at the model.
    bool show_palette = false;
    char palette_buf[256] = {0};
    // The operand timeline's overview/minimap window (21-spine-navigation.md T3),
    // in step space; the ImZoomSlider pans/zooms it and the strip draws it as the
    // current viewport. hi <= lo means "whole trace" (the first-frame default) —
    // a view-local camera, not nav state (D4), exactly like the Loom's cam.
    double tl_lo = 0, tl_hi = 0;
    dt_nav_table nav;
    bool show_help = false;
    bool show_learn = false;
    bool show_author = false;
    bool show_inspect = false;
    // docs/internal/archive/gui/45-launch-and-window-target.md T6/T7: the opaque
    // GLFWwindow* main.cpp owns, injected the same way ShellState::scene_host
    // is (ui/scene_host.h's split) — void* so this header (and every TU that
    // includes it, including the null-backend test tree) needs no GLFW
    // include; null under the null backend, where the crosshair cursor swap
    // is simply skipped (shell_set_crosshair_cursor/shell_restore_cursor,
    // shell.cpp, both guard on it being non-null).
    void *glfw_window = nullptr;
    // T7: a left-drag on the Home rail's crosshair button is in flight — the
    // OS cursor is a crosshair and each frame tracks the GLOBAL pointer
    // (window_picker_global_pointer, not ImGui's window-relative
    // GetMousePos, which goes stale once the pointer leaves this app's own
    // window). Cleared on release (shell_finish_window_pick), whether or not
    // a window was actually found there.
    bool picking_window = false;
    // Dockable-pane visibility (the docked shell): the user's open/close state per
    // pane, keyed by the kPane* name; absent => the pane's shipped default (see
    // pane_default_open). The View ▸ Panels menu toggles these and each pane's
    // close (X) clears its own. Panes are ALSO gated on CONTEXT (a recording open /
    // a host connected) so an irrelevant pane never shows — "only open as
    // required". A pure model field, so test_shell can drive visibility headlessly.
    std::map<std::string, bool> pane_open;
    std::string status; // the status bar: nav refusals land here verbatim

    // --- the colored session/status Log (the Log pane) ----------------------
    // A scrollback of everything the capture (and other tabs) emit as "additional
    // information": every live-session TRANSITION (the same live_session_toasts()
    // the toast layer fires, so a toast IS a log line), plus a nav refusal each
    // time `status` changes. Colored by ToastKind. It lets the capture/other tabs
    // stay uncluttered — the noise moved here. Bounded (kLogMax) so an unbounded
    // session cannot grow it without limit. A pure model field; test_shell drives
    // the append rules headlessly.
    struct LogLine {
        std::string text;
        ToastKind kind = ToastKind::Info;
    };
    std::vector<LogLine> log;
    std::string last_logged_status; // the `status` value already in the log
    static constexpr size_t kLogMax = 500;
    CompletenessState completeness;
    // The Loom's per-tab state (05-loom-day-one.md). Woven once per recording,
    // not per frame.
    LoomState loom;
    // The Learn door's card list + player (06-doors-and-learning.md T4).
    LearnState learn;
    // The capability panel (06-doors-and-learning.md T6).
    CapState caps;
    // The Author door (06-doors-and-learning.md T5); full build only.
    AuthorState author;
    // The Inspect door (07-serve-live-host.md T4/T5). In BOTH binaries: it
    // links no engine and captures through the `asmspy --serve` subprocess.
    InspectState inspect;
    // Last frame's feedback state (live status + save outcome), so draw_shell
    // can raise a toast on each TRANSITION (a new refusal / session end / fatal
    // / skip / save) rather than every frame (16-live-feedback-and-filtering.md
    // T1). Toasts SUPPLEMENT the in-pane refusal banners — the Inspect door
    // still shows them first-class.
    FeedbackInputs prev_feedback;
    // The live Observer views (08-observer-views.md), one deck per open
    // recording — parallel to ws.recordings, exactly like `streams`. A live
    // session's recording and a replayed file feed the SAME deck, which is the
    // property the whole doc is built on.
    std::vector<ObserverState> observers;
    std::string repo_root = ".";

    // The register time-travel scrubber (09-teaching-producers.md T3), surfaced
    // as a per-recording tab. `stepidx` is the O(1) regstate seek index, built
    // once at open and parallel to ws.recordings exactly like `streams` /
    // `observers`; a recording with no ring yields an absent index and the tab
    // shows the producer placard. `scrubber_playhead` is that tab's cursor, one
    // per recording so switching tabs keeps each recording's place.
    std::vector<StepIndex> stepidx;
    std::vector<uint64_t> scrubber_playhead;

    // 40 T2: the dataflow stream segmented by df_invocation, one per recording,
    // parallel to `streams`. A continuous dataflow/auto capture is many invocation
    // passes in one recording (35 T1), each restarting df_step at 0; the dataflow
    // views (Slice / Timeline / Loom) show ONE pass at a time. `df_pass` is the
    // selected pass per recording: < 0 follows the LATEST pass (the live default,
    // as build_step_index resolves the register ring for the Scrubber), >= 0 pins
    // an earlier one. A one-shot recording (one pass) shows no selector and reads
    // exactly as pre-40. shell_apply_df_pass resolves + applies the choice.
    std::vector<SegmentedDataflow> seg_df;
    std::vector<int> df_pass;

    // The ABI x-ray (09-teaching-producers.md T4), surfaced as a tab that locks
    // the active recording (the SysV leg) against the attached B (the Win64 leg,
    // the `d` binding) — the same A/B mechanism the Diff tab uses. The rail
    // MUTATES `abixray_walk` (stop navigation), so it must persist across frames;
    // `abixray_key` is the pair it was built for (`A_id\x1fB_id`), rebuilt only
    // when the pair changes. `abixray_playhead` is the single locked playhead.
    std::string abixray_key;
    wt_model abixray_walk;
    uint64_t abixray_playhead = 0;

    // The 3D spacetime overview (10-spacetime-3d-overview.md), surfaced as a
    // per-recording tab. `scenes` is parallel to ws.recordings exactly like
    // `streams` / `observers` / `stepidx`; each slot is woven lazily on first
    // view. `scene_host` is the GL render-to-texture bridge threaded in from
    // main.cpp (ui/gl_scene_host.cpp); it is null under the null test backend and
    // in any run with no GL context, and the pane then draws its models + HUD +
    // a placard where the viewport would be. draw_shell links no GL — the scene
    // is reached ONLY through this abstract pointer.
    std::vector<SceneView> scenes;
    SceneHost *scene_host = nullptr;

    // --- 25-live-model-wiring.md: the live capture as a workspace tab ------
    // The growing `asmspy --serve` recording, promoted into ws.recordings (with
    // its parallel streams/observers/stepidx/scenes slots) so the SAME docked
    // panes + view_presence that a replayed file drives also render a live
    // session — Loom / Slice / Timeline / 3D go live, not just the Observer
    // deck. `live_tab` is that entry's index, or -1 when no session is up. The
    // slot is ephemeral (never persisted) and rebuilt only when the capture
    // grows: `live_built_events` / `live_built_recordings` gate that rebuild the
    // way draw_live_views gates its observer, so a static frame costs nothing.
    int live_tab = -1;
    uint64_t live_built_events = 0;
    size_t live_built_recordings = 0;
    // 25 T3: the dedup watermark. When an ENDED session's capture is opened as a
    // saved file (the Inspect door's "Open in Loom"), that permanent file tab
    // supersedes the ephemeral live tab — so we retire the live tab and remember
    // how many completed recordings the session had, so shell_sync_live_tab does
    // not resurrect it from the same still-present `recordings()` entry. Reset to
    // 0 when the host fully empties (a fresh connect starts clean).
    size_t live_dismissed_done = 0;
    // The CAPTURE ORDINAL the "only the panes this capture fills" pass last ran
    // for (R10 / doc 40) — NOT the live_tab index, which is unstable: an unrelated
    // tab close shifts live_tab (shell_close), and a completed capture stays
    // mirrored so live_tab is reused across captures in one session. The ordinal
    // (completed recordings + a growing one) is a stable per-capture identity, so
    // the one-shot pass fires exactly once per distinct capture and never re-opens
    // a pane the user closed on an unrelated tab move. -1 = never applied.
    long live_applied_ordinal = -1;

    // A pending cross-door jump: a capture the Inspect door just saved and asked
    // to open in the Loom (07-serve-live-host.md). `want_open_tab` is the
    // recording index whose outer tab to select; `want_loom` forces its Loom
    // inner tab. Both are set when draw_shell consumes InspectState::open_request
    // and cleared the same frame after the tab strip applies them.
    int want_open_tab = -1;
    bool want_loom = false;

    // --- 18-breach-stops.md T1 (convention-alignment keys) ----------------
    // `F` fit-selection intent (mirrors want_view): the active spatial view frames
    // the current selection. `wasd_context` is the labelled context switch that
    // resolves the W/S/A/D-vs-diff-`d` conflict: WASD means CAMERA only when a
    // spatial pane (timeline / 3D) holds focus; outside it, `d` keeps its diff
    // meaning. `wasd_zoom`/`wasd_pan` accumulate the camera nudges the spatial
    // panes read — an explicit, testable state field, not an implicit focus guess.
    bool want_fit = false;
    bool wasd_context = false;
    int wasd_zoom = 0;
    int wasd_pan = 0;

    // --- 18-breach-stops.md T2 (real, always-available Reset) -------------
    // `want_layout_reset` is the keymap/palette intent (Ctrl+Shift+R) consumed
    // near the dockspace build, so Reset fires with or without the menu bar and
    // in both binaries. `layout_settle` counts docked frames after init so the
    // zero-visible-pane auto-fallback runs once the panes have had a frame to
    // adopt their nodes — never fighting a user who is mid-drag.
    bool want_layout_reset = false;
    int layout_settle = 0;

    // --- 22-selection-and-search.md T2 (keyboard-camera focus) ------------
    // Set by the 3D overview pane (its HUD or viewport) when it holds focus, read
    // by handle_keymap NEXT frame so the arrow keys orbit the camera there rather
    // than stepping the selection — the same last-frame-focus seam wasd_context
    // uses. An explicit, testable field, not an implicit focus guess.
    bool cam_focus = false;

    // --- 22-selection-and-search.md T3 (global find, Ctrl+F) --------------
    // Search-as-measurement over the active recording's decoded streams + deck.
    // The model is pure (ui/find.h); this holds the query, the hit set and the
    // cycled index. Highlights ALL hits; never hides rows (D7).
    FindState find;

    // --- 22-selection-and-search.md T4 (app-level command / undo stack) ---
    // Ctrl+Z / Ctrl+Y over reversible view-model state (filter / cone / selection
    // / take set). DISTINCT from the Author editor's own text undo (doc 17 T2) —
    // both guard on io.WantTextInput and own disjoint state. `undo_filter_seen`
    // is the last filter value the stack recorded, so a settled edit becomes ONE
    // Command rather than one per frame.
    UndoStack undo;
    std::string undo_filter_seen;
    int undo_filter_tab = -1; // the recording undo_filter_seen belongs to

    // --- 18-breach-stops.md T3 (Author save-guard) ------------------------
    // `close_pending` is the workspace-recording index whose close is awaiting a
    // save/discard/cancel choice because it is dirty (authored + unsaved); -1 =
    // none pending. `author_close_guard` is the same guard for the Author door
    // tab. Both are raised by the shell_request_close* seams so a dirty tab can
    // never be closed with a single silent click (F24).
    int close_pending = -1;
    bool author_close_guard = false;
};

// Open a recording AND decode its streams, keeping ShellState::streams parallel
// to Workspace::recordings. Returns the new index, or -1 with `err` set.
int shell_open(ShellState &s, const std::string &path, std::string &err);
void shell_close(ShellState &s, size_t idx);

// 25-live-model-wiring.md T1/T2: keep one workspace tab mirroring the live
// `asmspy --serve` capture (growing, or the last completed one), so the docked
// panes + view_presence render a live session the same way a replayed file is
// rendered. Called once per frame from draw_shell; a no-op when no session is
// up. Idempotent and headless — test_shell drives it directly over a synthetic
// fed session. Rebuilds the tab's decoded streams / observer / step index only
// when the capture's event count moves.
void shell_sync_live_tab(ShellState &s);

// 34 T2: consume the Live-capture "View in 3D overview" intent
// (InspectState::want_scene). When a live capture tab is up, jump the active tab
// to it (want_open_tab) and request its 3D inner tab (want_view_id = Scene3D);
// otherwise route a truthful reason to the status bar. A pure model move (no
// ImGui), so test_shell drives it directly. Called once per frame from draw_shell.
void shell_consume_scene_handoff(ShellState &s);

// 25 T3: drop the ephemeral live tab (its parallel slots, the index shift, the
// active-tab clamp) without touching the dedup watermark. Used both by
// shell_sync_live_tab's own teardown and when a saved capture is opened as a
// permanent file tab that supersedes it. No-op when no live tab is up.
void shell_retire_live_tab(ShellState &s);

// The dirty-close guard (18-breach-stops.md T3, F24). `shell_request_close` is
// the seam the tab `✕` drives: it closes a clean recording immediately, but a
// DIRTY (authored + unsaved) one raises the save/discard/cancel guard
// (`close_pending`) instead of erasing, so authored output is never lost to a
// single click. `shell_discard_close` erases the pending entry; `shell_cancel_close`
// abandons the guard, keeping the recording. Pure model moves, so test_shell
// drives them headlessly.
void shell_request_close(ShellState &s, size_t idx);
void shell_discard_close(ShellState &s);
void shell_cancel_close(ShellState &s);
// The Author DOOR TAB's own guard: closing it while its run is unsaved raises
// `author_close_guard` rather than dropping the recording. Returns true when the
// tab actually closed (it was clean), false when the guard was raised.
bool shell_request_author_close(ShellState &s);

// Register every view with the router and point it at the open set. Idempotent:
// safe to call again after the workspace changes.
void shell_wire_nav(ShellState &s);

// --- 20 T2: select a task mode ------------------------------------------------
// Set `s.mode`, request its dock perspective (`pending_preset`, applied by the
// docked frame), and open the mode's surface (Learn/Open/Capture/Author). The
// seam the rail CTAs and the tests both drive — a pure ShellState move, so
// test_shell asserts the resulting mode + pending_preset without a display.
void shell_select_mode(ShellState &s, Mode m);

// --- docs/internal/archive/gui/45-launch-and-window-target.md T7/T8: the crosshair
// drag affordance's STATE MACHINE, split from its ImGui glue (drawn in
// draw_home_rail) exactly so it is unit-testable without a display or a real
// X11 drag: shell_start_window_pick / shell_finish_window_pick take
// synthetic state, the same "drive the gate directly" pattern
// inspect_request_start/inspect_request_launch's tests already use. ---------
// Arm the drag: sets picking_window and swaps the OS cursor to a crosshair
// (a no-op if s.glfw_window is null — the null-backend tests, and the
// render-only viewer before a window exists).
void shell_start_window_pick(ShellState &s);
// Resolve the drag: clears picking_window, restores the OS cursor, and
// (T8) either logs `picked.why_not` (never silent) or attaches through the
// SAME path a Processes-row click uses (inspect_attach_full_detail) and
// lands the shell in Mode::Capture. Takes an ALREADY-RESOLVED PickedWindow
// rather than screen coordinates — the live X11 resolve
// (resolve_window_at_screen_point) stays in draw_home_rail's real draw
// path, so this function itself needs no display either.
void shell_finish_window_pick(ShellState &s, const PickedWindow &picked);

// --- 20 T3: workspace persistence --------------------------------------------
// Capture the open set, the active position and each recording's per-pane
// selection as asmtrace-links, plus recents/perspectives/presets — everything
// build/desktop-workspace.json remembers. Restore replays it: shell_open each
// path, then dt_nav_go the active position so the workspace lands exactly where
// it was left (D4). A path that no longer loads is kept in recents WITH its load
// error routed to s.status (D7 — never silently dropped). Pure model moves, so
// test_shell/test_workspace_state drive the round trip headless.
WorkspaceState shell_capture_workspace(const ShellState &s);
void shell_restore_workspace(ShellState &s, const WorkspaceState &ws);

// The A / B streams for the active tab; B is null when nothing is attached.
const Streams *shell_a(const ShellState &s);
const Streams *shell_b(const ShellState &s);

// 40 T2: resolve + apply the dataflow pass recording `i`'s views show. df_pass[i]
// < 0 follows the LATEST pass (the live default); >= 0 pins one. Sets the cached
// streams[i].df to the chosen pass (a no-op when following latest — decode_streams
// already put it there) and returns the resolved pass index (0 when the recording
// has no dataflow or no segment cache). Pure of ImGui so test_shell drives it.
size_t shell_apply_df_pass(ShellState &s, size_t i);

// Build the operand-timeline model for `a` (with `b` for a diff), projecting the
// shared selection + the global-find hits — but the selection ONLY when it
// belongs to `a` (selection.rec == a->id), so a step brushed in another recording
// never marks a coincident index here (22 T1, D7). Pure and engine-free: `s` is
// read-only and nothing is drawn, so body_timeline draws what this returns and
// the null-backend test drives it directly (a true regression seam for the
// recording-scoping fix).
dt_timeline shell_timeline_model(const ShellState &s, const Streams *a,
                                 const Streams *b);

// Draw one frame of the shell. Backend-free: only ImGui immediate-mode calls, so
// a null ImGui context (no GLFW/GL) drives it in tests.
void draw_shell(ShellState &s);

// The persistent task-language entry rail (20 T2). Normally drawn inside the
// shell (kPaneHome / the windowed left child); exposed so the doc-17 interaction
// lane can drive its CTA clicks directly, as it does draw_obs_syscalls.
void draw_home_rail(ShellState &s);

// The 3D spacetime overview pane for the active recording (10-spacetime-3d-
// overview.md — the integration surfacing pass). Weaves the pure space/ models
// once, draws the HUD, re-slices the terrain on a playhead move, and — when
// s.scene_host is present — blits the GL scene and routes a pick OUT to a flat 2D
// view through 04's router (3D to find, 2D to read). Backend-free itself: the GL
// touch is entirely behind s.scene_host, so the null backend drives the model +
// HUD + placard path. Public so test_shell can drive it without forcing the tab
// selection. Call inside an ImGui window, with s.active_tab set to the recording.
void draw_scene_overview(ShellState &s, const Recording &r, const Streams &a);

// T3 (44-faithful-city-phase-a): the fidelity-tier -> Atmosphere mapping,
// pure and public so it is unit-testable with NO ImGui frame and no GL
// context (test_shell.cpp calls it directly). Reads ONLY the shared palette
// (ui/theme.h's dt_warn_col/dt_refuse_col/dt_dim_col) — never an
// independently-chosen RGB literal — so the sky is byte-identical in COLOR
// SOURCE to the 2D fidelity banner, per the brief's load-bearing invariant
// ("weather is byte-identical to the 2D fidelity verdict"). Lives here (the
// ImGui-linked shell TU) rather than in scene3d/ because theme.h must stay
// OUT of the engine-free scene TU (D4-adjacent).
scene3d::Atmosphere scene_atmosphere_for_tier(FidelityTier tier);

// T5 (44-faithful-city-phase-a): SceneFrame.sun as a PURE function of
// existing HudState fields (t / nsteps) — no new persisted UI state
// duplicates it, so this is directly unit-testable with no ImGui frame.
// `nsteps == 0` (nothing placed yet) returns a fixed "noon" constant per the
// brief's own note, rather than dividing by zero.
inline float scene_sun_from_hud(uint64_t t, uint64_t nsteps) {
    if (nsteps == 0)
        return 0.5f; // noon
    const float s = static_cast<float>(t) / static_cast<float>(nsteps);
    return s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
}

// Apply one undo Command onto the shell (22 T4). `redo` selects the after-value
// (Ctrl+Y) vs the before-value (Ctrl+Z); it restores the filter / cone /
// selection / take-set state the command captured, and never touches the Author
// editor's own text undo. Pure model move, so test_undo drives it headlessly.
void undo_apply(ShellState &s, const UndoCommand &c, bool redo);

// The truncation/drops/torn banner for a recording — PURE: nullptr when the
// recording is clean, else a human-readable line (e.g.
// "TRUNCATED recording — buffers filled"). The returned pointer is valid until
// the next call on the same thread. This is D7 as behaviour, asserted by tests.
const char *shell_banner(const Recording &r);

// Append one line to the session Log (the Log pane), bounded to kLogMax (the
// oldest lines drop). Pure model move, so test_shell drives the append + the ring
// bound headlessly. The colored render is draw_log_pane.
void shell_log_push(ShellState &s, const std::string &text, ToastKind kind);

// When `advice` (a refusal / skip reason) names a gate with a one-line terminal
// fix (remedy_command), echo "run this in a terminal: <cmd>" into the Log so the
// exact command survives in the scrollback after the transient toast dismisses.
// No-op when the advice has no single-command fix. Pure model move, test-driven.
void shell_log_command_hint(ShellState &s, const std::string &advice);

// Set pane_open for the panes RELEVANT to task mode `m` and clear the rest — the
// "only open the relevant tabs" move (docs R8/R9), applied on a mode transition
// (shell_select_mode) and once for the restored mode on first frame. Context still
// gates whether a relevant pane actually shows; View ▸ Panels still reopens any of
// them. A pure model move, so test_shell asserts the resulting pane_open map.
void shell_apply_mode_panes(ShellState &s, Mode m);

// When a live capture appears, open exactly the visualization panes that capture
// fills and close the rest — once per capture (R10), so a manual close afterward
// holds. Runs only in Capture/Inspect mode; resets its guard when the capture
// ends. Pure model move over ShellState; test_shell drives it headlessly.
void shell_apply_live_panes(ShellState &s);

} // namespace asmdesk
#endif // ASMDESK_UI_SHELL_H
