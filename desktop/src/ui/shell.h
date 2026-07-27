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
#include "scene3d/camera.h"
#include "scene3d/hud.h"
#include "space/converge.h"
#include "space/terrain.h"
#include "space/trajectory.h"
#include "ui/doors.h"
#include "ui/mode.h"     // task-language modes (20 T2)
#include "ui/primer.h"   // first-open primer state (24 T5)
#include "ui/scene_host.h"
#include "ui/settings.h" // user text-scale / theme / window size (20 T5)
#include "views/completeness.h"
#include "views/observer_draw.h"
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
    // The 3D scrub's degrade-to-coarse state (23-graded-truth-layer.md T4):
    // `scrub_pending` marks a re-slice deferred because the full slice would
    // exceed the frame budget — the coarse plane renders this frame and the full
    // slice lands next, instead of a silent UI-thread stall.
    bool scrub_pending = false;
    scene3d::Camera cam;
    scene3d::HudState hud;
    bool nav_dragging = false; // a left-drag is orbiting (suppresses the pick)
    dt_primer_state primer;    // the first-open primer (24 T5), per recording
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

    // --- the replay views (04-replay-views.md) ---------------------------
    // Decoded once per open recording, parallel to ws.recordings: the builders
    // are pure functions of these, so nothing below re-parses JSON per frame.
    std::vector<Streams> streams;
    // Plan D3: every view takes one OR two recordings. `b_index` is the
    // attached B side (the `d` binding), -1 for none.
    int b_index = -1;
    dt_view view = dt_view::canvas;
    std::optional<uint32_t> selected_step;
    std::optional<uint64_t> selected_off;
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
    // The `Ctrl+G` go-to-step/offset modal (17-T1 keymap): open flag + its
    // InputText buffer. The modal parses the text with dt_nav_parse and jumps
    // via dt_nav_go, so a typed target lands exactly like a clicked link.
    bool show_goto = false;
    char goto_buf[64] = {0};
    dt_nav_table nav;
    bool show_help = false;
    bool show_learn = false;
    bool show_author = false;
    bool show_inspect = false;
    std::string status; // the status bar: nav refusals land here verbatim
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

// The truncation/drops/torn banner for a recording — PURE: nullptr when the
// recording is clean, else a human-readable line (e.g.
// "TRUNCATED recording — buffers filled"). The returned pointer is valid until
// the next call on the same thread. This is D7 as behaviour, asserted by tests.
const char *shell_banner(const Recording &r);

} // namespace asmdesk
#endif // ASMDESK_UI_SHELL_H
