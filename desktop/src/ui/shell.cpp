// shell.cpp — see shell.h. All draws are backend-free ImGui immediate calls; no
// GLFW, no GL, no engines are touched here, so the null backend renders every
// path in tests (03-desktop-shell.md T6).
#include "ui/shell.h"
#include "ui/layout.h"

#include <functional>
#include <string>
#include <utility>

#include "imgui.h"
// Non-modal toasts for live-session events (16 T1). This header is a docking/
// multi-viewport client at its default NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW=true;
// we render inside the main viewport, so pin it false BEFORE the include (the
// compile-probe pins the same value). It also pulls IconsFontAwesome6.h, whose
// glyphs load_fonts merges into the atlas — so the icons in a toast resolve.
#define NOTIFY_RENDER_OUTSIDE_MAIN_WINDOW false
// The addon include ORDER below is load-bearing: ImGuiNotify.hpp includes
// imgui_internal.h while IMGUI_DEFINE_MATH_OPERATORS is still undefined (its guard
// passes), and MUST precede ImGuiFileDialog.h, which #defines that macro and
// re-pulls imgui_internal.h. Alphabetical sorting (FileDialog first) makes
// imgui_internal.h #error "define IMGUI_DEFINE_MATH_OPERATORS before imgui.h", so
// the block is fenced from clang-format's include sorting.
// clang-format off
#include "ImGuiNotify.hpp"
#include "ImGuiFileDialog.h" // pure-ImGui open dialog (14 T7)
// clang-format on

#include <algorithm> // std::sort/std::swap for the keymap (17 T1)
#include <cstdio>    // std::snprintf — undo_apply's filter restore (22 T4)
#include <cstdlib>   // std::strtoul — the go-to modal's step parse (17 T1)
#include <cstring>   // std::strcmp — the pane-def name lookup

#include "analysis/diff.h" // dt_diff_build — n/p divergence walk (17 T1)
#include "analysis/slice.h"
#include "live/inspect.h" // live_session_toasts (16 T1)
#include "scene3d/goto.h"
#include "scene3d/hud.h"
#include "scene3d/pick.h"
#include "space/projection.h"
#include "ui/legend.h"       // shared semantic legend (24 T1/T2)
#include "ui/palette.h"      // command palette over the spine (21 T1)
#include "ui/perspectives.h" // named dock perspectives + filter presets (20 T4)
#include "ui/terms.h"        // domain-term-first headings + Terms pane (24 T3)
#include "ui/theme.h" // dt_set_light_theme (20 T5); dt_maybe_col coarse-scrub degrade (23 T4)
#include "ui/timepos.h" // the one time-position widget: the df pass pager (40 T2)
#include "ui/view_presence.h" // data-driven view set + faithful absence (20 T1)
#include "ui/wayfinding.h"    // persistent breadcrumb + disambiguation (21 T2)
#include "views/abixray.h"
#include "views/crossing.h" // 57 T2: build_crossing_layer
#include "views/views_draw.h"

namespace asmdesk {

// Defined below draw_shell, used by both shells' status areas (18-T6 breadcrumb).
static void draw_breadcrumb(ShellState &s);

// 50 T4: the cell-centre rounding resolve_pick/classify_cell (pick.cpp)
// already use, shared here so "frame the selection" and the off-screen
// disclosure label can never round differently from each other or from a
// click.
static void cell_center_uv(uint32_t cell, uint32_t n, float *u, float *v) {
    *u = (cell % n + 0.5f) / static_cast<float>(n);
    *v = (cell / n + 0.5f) / static_cast<float>(n);
}

// The verbatim greyed-out-shows-why reason (plan D2): an engine door disabled in
// the render-only viewer states, in place, exactly why. [[maybe_unused]] because
// only the render-only build (-DASMTEST_DESKTOP_RENDER_ONLY) references it.
[[maybe_unused]] static const char *kEngineDoorReason =
    "requires the full app (GPL-2.0; links the engines) — this is the "
    "render-only viewer";

const char *shell_banner(const Recording &r) {
    static thread_local std::string buf;
    if (!r.truncated() && !r.dropped())
        return nullptr;
    buf.clear();
    if (r.torn)
        buf = "TORN recording — no end event; shown as incomplete";
    else if (r.end_truncated)
        buf = "TRUNCATED recording — buffers filled";
    if (r.dropped()) {
        if (!buf.empty())
            buf += "; ";
        buf += "dropped " + std::to_string(r.drops_lost);
        if (r.drops_throttled)
            buf += " (throttled)";
    }
    return buf.c_str();
}

static std::string base_name(const std::string &path); // defined below

int shell_open(ShellState &s, const std::string &path, std::string &err) {
    int idx = s.ws.open(path, err);
    if (idx < 0)
        return -1;
    // Decode once, at open. Every builder below is a pure function of Streams,
    // so no view re-parses JSON per frame.
    s.streams.resize(s.ws.recordings.size());
    s.streams[static_cast<size_t>(idx)] =
        decode_streams(s.ws.recordings[static_cast<size_t>(idx)]);
    // The Observer deck (08) is built from the Recording rather than the
    // decoded Streams: its kinds (syscall, watch, topo, call, codeimage) are
    // not 04's, and a second decode of the same fields would be a second place
    // for them to be wrong.
    s.observers.resize(s.ws.recordings.size());
    observer_build(s.observers[static_cast<size_t>(idx)],
                   s.ws.recordings[static_cast<size_t>(idx)]);
    // The register scrubber's O(1) seek index (09-T3), one per recording. Built
    // once here — it reads only `regstate` events, so a recording without the
    // ring yields an absent index (the normal case) and the tab says so. The
    // playhead starts at 0; a fresh slot for a reopened index must reset, so it
    // is assigned rather than left to resize's fill.
    s.stepidx.resize(s.ws.recordings.size());
    s.stepidx[static_cast<size_t>(idx)] =
        build_step_index(s.ws.recordings[static_cast<size_t>(idx)]);
    s.scrubber_playhead.resize(s.ws.recordings.size());
    s.scrubber_playhead[static_cast<size_t>(idx)] = 0;
    // The dataflow stream segmented by df_invocation (40 T2), parallel to
    // `streams`. The pass selector defaults to following the latest pass (-1);
    // decode_streams already put that pass in streams[idx].df.
    s.seg_df.resize(s.ws.recordings.size());
    s.seg_df[static_cast<size_t>(idx)] =
        build_segmented_dataflow(s.ws.recordings[static_cast<size_t>(idx)]);
    s.df_pass.resize(s.ws.recordings.size());
    s.df_pass[static_cast<size_t>(idx)] = -1;
    // The 3D overview's per-recording state (doc 10), parallel to the workspace.
    // The models are woven lazily on first view, so a fresh (unbuilt) slot is all
    // that is reserved here; assigned rather than left to resize's fill so a reused
    // index starts clean.
    s.scenes.resize(s.ws.recordings.size());
    s.scenes[static_cast<size_t>(idx)] = SceneView{};
    // Remember it (T3): the MRU recents on the rail + File ▸ Open Recent, and a
    // change to persist. `path` is stored, not the basename, so a recent reopens
    // the exact file. main.cpp flushes the store when `ws_dirty` is set.
    recents_push(s.recents, path);
    s.ws_dirty = true;
    shell_wire_nav(s);
    shell_log_push(s, "opened " + base_name(path), ToastKind::Success);
    return idx;
}

void shell_close(ShellState &s, size_t idx) {
    if (idx >= s.ws.recordings.size())
        return;
    s.ws.close(idx);
    if (idx < s.streams.size())
        s.streams.erase(s.streams.begin() + static_cast<long>(idx));
    if (idx < s.observers.size())
        s.observers.erase(s.observers.begin() + static_cast<long>(idx));
    if (idx < s.stepidx.size())
        s.stepidx.erase(s.stepidx.begin() + static_cast<long>(idx));
    if (idx < s.scrubber_playhead.size())
        s.scrubber_playhead.erase(s.scrubber_playhead.begin() +
                                  static_cast<long>(idx));
    if (idx < s.seg_df.size())
        s.seg_df.erase(s.seg_df.begin() + static_cast<long>(idx));
    if (idx < s.df_pass.size())
        s.df_pass.erase(s.df_pass.begin() + static_cast<long>(idx));
    if (idx < s.scenes.size())
        s.scenes.erase(s.scenes.begin() + static_cast<long>(idx));
    if (s.b_index == static_cast<int>(idx))
        s.b_index = -1;
    else if (s.b_index > static_cast<int>(idx))
        s.b_index--;
    if (s.close_pending == static_cast<int>(idx))
        s.close_pending = -1;
    else if (s.close_pending > static_cast<int>(idx))
        s.close_pending--;
    // The live tab (25) rides the same index shift: cleared if it was the one
    // erased, decremented if an earlier tab was.
    if (s.live_tab == static_cast<int>(idx))
        s.live_tab = -1;
    else if (s.live_tab > static_cast<int>(idx))
        s.live_tab--;
    s.ws_dirty = true; // the open set changed — persist it (T3)
    shell_wire_nav(s);
}

// 25 T3. Drop the ephemeral live tab and heal the indices around it. Leaves the
// dedup watermark alone — the caller owns that.
void shell_retire_live_tab(ShellState &s) {
    if (s.live_tab < 0)
        return;
    size_t idx = static_cast<size_t>(s.live_tab);
    bool was_active = (s.active_tab == s.live_tab);
    if (s.active_tab > s.live_tab)
        s.active_tab--;  // erasing idx shifts later tabs down
    shell_close(s, idx); // erases every parallel slot + clears live_tab
    s.live_built_events = 0;
    s.live_built_recordings = 0;
    if (was_active)
        s.active_tab =
            s.ws.recordings.empty()
                ? -1
                : static_cast<int>(std::min(idx, s.ws.recordings.size() - 1));
}

// 25-live-model-wiring.md T1/T2. Keep one workspace tab mirroring the live
// capture so the docked panes + view_presence render it exactly as a replayed
// file — the growing recording is not a second code path, it is the same model.
void shell_sync_live_tab(ShellState &s) {
    LiveSession &sess = s.inspect.session;
    const Recording *live = sess.growing();
    // A completed capture stays mirrored as a frozen tab — UNLESS it has already
    // been adopted as a saved file tab (25 T3): past the dedup watermark, the
    // last completed recording is the one "Open in Loom" already promoted, so it
    // must not be resurrected here as a second, ephemeral copy of the same run.
    if (live == nullptr && sess.recordings().size() > s.live_dismissed_done)
        live = &sess.recordings().back();

    // No capture to show (idle / reset / the only completed one was adopted):
    // drop any promoted tab and leave.
    if (live == nullptr) {
        shell_retire_live_tab(s);
        // A host with no recordings at all is a clean slate — forget the
        // watermark so the next connect starts fresh.
        if (sess.recordings().empty())
            s.live_dismissed_done = 0;
        return;
    }

    // First sight of a live recording: append the slot + its parallel indices.
    if (s.live_tab < 0) {
        s.ws.recordings.push_back(*live);
        s.live_tab = static_cast<int>(s.ws.recordings.size()) - 1;
        size_t i = static_cast<size_t>(s.live_tab);
        s.streams.resize(s.ws.recordings.size());
        s.observers.resize(s.ws.recordings.size());
        s.stepidx.resize(s.ws.recordings.size());
        s.scrubber_playhead.resize(s.ws.recordings.size());
        s.scenes.resize(s.ws.recordings.size());
        s.seg_df.resize(s.ws.recordings.size());
        s.df_pass.resize(s.ws.recordings.size());
        s.scrubber_playhead[i] = 0;
        s.scenes[i] = SceneView{};
        s.df_pass[i] = -1; // a live continuous capture follows its latest pass
        s.live_built_events =
            ~static_cast<uint64_t>(0); // force the first build
        s.live_built_recordings = ~static_cast<size_t>(0);
        // Land the panes on the capture only from Home — never steal a
        // deliberate file selection.
        if (s.active_tab < 0)
            s.active_tab = s.live_tab;
    }

    // Rebuild only when the capture grew, or a session boundary moved which
    // recording `live` points at — the same gate draw_live_views uses, so a
    // static frame re-decodes nothing.
    uint64_t n = live->event_count();
    size_t ndone = sess.recordings().size();
    if (n == s.live_built_events && ndone == s.live_built_recordings)
        return;
    size_t i = static_cast<size_t>(s.live_tab);
    s.ws.recordings[i] = *live; // the r/a-based views (Summary, 3D, observer)
    s.streams[i] = decode_streams(s.ws.recordings[i]);
    // A live session keeps its lifecycle OUTSIDE the recording (07-T3); feed it
    // in so the observer deck reads the started-params + skip the way a file's
    // inline lifecycle is read.
    std::vector<nlohmann::json> bodies;
    for (const LiveNote &note : sess.notes())
        bodies.push_back(note.body);
    ObsLifecycle lc = observer_lifecycle_from(bodies);
    observer_build(s.observers[i], s.ws.recordings[i], &lc);
    s.stepidx[i] = build_step_index(s.ws.recordings[i]);
    // Re-segment the grown capture so a new invocation pass appears in the pager;
    // a df_pass following the latest (-1) tracks the new tail automatically.
    s.seg_df[i] = build_segmented_dataflow(s.ws.recordings[i]);
    // 25 T6: force a lazy 3D re-weave over the grown recording, but KEEP the
    // interactive camera / HUD / primer — else a growing capture would snap the
    // user's 3D view back to the default orbit on every event batch.
    {
        scene3d::Camera cam = s.scenes[i].cam;
        scene3d::HudState hud = s.scenes[i].hud;
        dt_primer_state primer = s.scenes[i].primer;
        s.scenes[i] = SceneView{};
        s.scenes[i].cam = cam;
        s.scenes[i].hud = hud;
        s.scenes[i].primer = primer;
    }
    s.live_built_events = n;
    s.live_built_recordings = ndone;
    shell_wire_nav(
        s); // the decoded stream id is now live — point the router at it
}

// 34 T2: the Live-capture "View in 3D overview" handoff. A pure model move — no
// ImGui — so test_shell drives it directly. want_open_tab selects the live
// recording's outer tab (honoured by both shells); want_view_id selects its 3D
// inner tab (mirroring the 1/2/3/4 want_view path). With no live tab yet, an
// faithful status line instead of a silent no-op (D7).
void shell_consume_scene_handoff(ShellState &s) {
    if (!s.inspect.want_scene)
        return;
    s.inspect.want_scene = false;
    if (s.live_tab >= 0) {
        s.want_open_tab = s.live_tab;
        s.want_view_id = ViewId::Scene3D;
    } else {
        s.status = "no live capture tab yet — attach a target in Processes "
                   "first, then View in 3D";
    }
}

// The dirty-close guard (18-breach-stops.md T3, F24): a clean recording closes
// on the spot; a DIRTY (authored + unsaved) one raises the save/discard/cancel
// choice instead of erasing, so authored output is never lost to one click.
void shell_request_close(ShellState &s, size_t idx) {
    if (idx >= s.ws.recordings.size())
        return;
    if (s.ws.recordings[idx].dirty)
        s.close_pending =
            static_cast<int>(idx); // raise the guard, do not erase
    else
        shell_close(s, idx);
}

void shell_discard_close(ShellState &s) {
    if (s.close_pending < 0 ||
        s.close_pending >= static_cast<int>(s.ws.recordings.size()))
        return;
    size_t idx = static_cast<size_t>(s.close_pending);
    s.close_pending = -1;
    shell_close(s,
                idx); // shell_close resets close_pending if it were still set
}

void shell_cancel_close(ShellState &s) { s.close_pending = -1; }

bool shell_request_author_close(ShellState &s) {
    if (s.author.dirty) {
        s.author_close_guard = true; // raise the guard; leave the tab open
        return false;
    }
    s.show_author = false;
    return true;
}

const Streams *shell_a(const ShellState &s) {
    if (s.active_tab < 0 || s.active_tab >= static_cast<int>(s.streams.size()))
        return nullptr;
    return &s.streams[static_cast<size_t>(s.active_tab)];
}

const Streams *shell_b(const ShellState &s) {
    if (s.b_index < 0 || s.b_index >= static_cast<int>(s.streams.size()))
        return nullptr;
    if (s.b_index == s.active_tab)
        return nullptr; // a recording is never diffed against itself
    return &s.streams[static_cast<size_t>(s.b_index)];
}

// Find an open recording by its deep-link id (its basename).
static int index_of_id(const ShellState &s, const std::string &id) {
    for (size_t i = 0; i < s.streams.size(); i++)
        if (s.streams[i].id == id)
            return static_cast<int>(i);
    return -1;
}

void shell_wire_nav(ShellState &s) {
    s.nav.have_recording = [&s](const std::string &id) {
        return index_of_id(s, id) >= 0;
    };
    // One handler per view. They only move the SELECTION — plan D4: no view
    // keeps its own navigation state, so a link and a keypress land identically.
    auto go = [&s](dt_view v) {
        return [&s, v](const dt_link &l) {
            // `blame` (09-T5) is not a view of its own: it is the failure-
            // attribution entry point, resolved HERE onto the slice explorer at
            // the failure step. The rest of this handler already does exactly
            // what blame needs — select the recording, land on `step`, and (via
            // the `if (l.step)` below) light the backward cone that answers
            // "what produced this wrong value". Reusing the slice explorer is the
            // whole point: no new heavy view ships for the Wave-2 producer.
            s.view = (v == dt_view::blame) ? dt_view::slice : v;
            int a = index_of_id(s, l.rec);
            if (a >= 0)
                s.active_tab = a;
            s.b_index = l.rec_b.empty() ? -1 : index_of_id(s, l.rec_b);
            // The ONE selection writer (22 T1): a deep link brushes the entity in
            // every pane at once and bumps `epoch`, so a link and a keypress land
            // identically (D4). Selection is distinct from nav — the router sets
            // both here (this handler moves the view AND brushes), while a plain
            // 1/2/3/4 view switch sets only nav.
            s.selection.set(l.rec, l.step, l.off);
            if (l.step)
                s.cone_active = true;
        };
    };
    // Every view, replay (04) and live (08) alike: a link naming an Observer
    // view must land, or the topology drill-in would be a dead end.
    for (dt_view v : dt_all_views())
        dt_nav_register(s.nav, v, go(v));
}

static std::string base_name(const std::string &path) {
    size_t slash = path.find_last_of('/');
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    return b.empty() ? "(unnamed)" : b;
}

void shell_log_push(ShellState &s, const std::string &text, ToastKind kind) {
    if (text.empty())
        return;
    s.log.push_back({text, kind});
    // Bounded ring: an unbounded live session must not grow the log without limit.
    // Drop from the front (oldest) once over the cap — a scrollback keeps the tail.
    if (s.log.size() > ShellState::kLogMax)
        s.log.erase(s.log.begin(),
                    s.log.begin() +
                        static_cast<long>(s.log.size() - ShellState::kLogMax));
}

void shell_log_command_hint(ShellState &s, const std::string &advice) {
    std::string cmd = remedy_command(advice);
    if (cmd.empty())
        return;
    shell_log_push(s, "run this in a terminal: " + cmd, ToastKind::Info);
}

// A descriptive label for an open recording in the Home list (R6): the
// disambiguated name, then what it IS — backend, exact/statistical, and its size
// (events, and steps when it carries a dataflow) — so the list reads as more than
// a column of "capture.asmtrace"s. The live capture is marked "● live … —
// capturing" so it is unmistakable. No `###id` suffix or dirty `*` here; the
// caller appends those so the Selectable id stays stable across a rename.
static std::string descriptive_recording_label(const ShellState &s, size_t i) {
    const Recording &r = s.ws.recordings[i];
    const bool is_live = static_cast<int>(i) == s.live_tab;
    std::string meta;
    if (!r.provenance.backend.empty())
        meta = r.provenance.backend;
    if (!meta.empty())
        meta += " · ";
    meta += r.statistical() ? "statistical" : "exact";
    uint64_t ev = r.event_count();
    meta += " · " + std::to_string(ev) + (ev == 1 ? " event" : " events");
    if (i < s.streams.size() && s.streams[i].df.present() &&
        s.streams[i].df.nsteps > 0)
        meta += ", " + std::to_string(s.streams[i].df.nsteps) + " steps";

    std::string name = disambiguated_label(s.ws, i);
    if (is_live) {
        const std::string &mode = s.inspect.session.status().mode;
        std::string tag = mode.empty() ? "live" : ("live " + mode);
        return "\xE2\x97\x8F " + name + "  \xE2\x80\x94 " + tag +
               ", capturing (" + meta + ")";
    }
    return name + "  \xE2\x80\x94 " + meta;
}

// --- 20 T2: select a task mode (the seam the rail CTAs + the tests drive) -----
void shell_select_mode(ShellState &s, Mode m) {
    s.mode = m;
    // An application-log line for the task switch, so the Log carries the app's own
    // lifecycle (not only the live-session feed).
    shell_log_push(s, std::string("task: ") + mode_cta(m), ToastKind::Info);
    s.pending_preset =
        mode_preset(m); // the docked frame applies it (T2 step 5)
    // Open exactly the panes this task leads with and close the rest (R9): picking
    // Capture must not leave the replay reading panes cluttering the workspace, and
    // picking Open must not leave the capture panes up. Context still gates whether
    // a listed pane shows; View ▸ Panels still reopens any of them.
    shell_apply_mode_panes(s, m);
    switch (m) {
    case Mode::Learn:
        s.show_learn = true;
        break;
    case Mode::Open:
        s.open_dialog = true;
        s.open_error.clear();
        break;
    case Mode::Capture:
    case Mode::Inspect:
        // Inspect is in BOTH binaries — the one door where D9 pays off visibly.
        // It links no engine: it reads /proc itself and captures by spawning
        // `asmspy --serve` as a subprocess, so the render-only viewer hosts live
        // sessions with its `ldd` still free of every tracer.
        s.show_inspect = true; // the windowed shell's Inspect tab
        // Land on the Processes tab (opened by shell_apply_mode_panes) and connect
        // the serve host from the saved Settings (want_autoconnect, consumed in
        // draw_shell) rather than forcing the Connect pane open. Connect stays a
        // fallback: draw_shell reveals it if the auto-connect fails.
        s.inspect.want_autoconnect = true;
        // Bring the Processes pane forward so the target picker takes focus, not
        // whichever peer (Connect / Live capture) last held its dock tab.
        s.inspect.want_focus_processes = true;
        break;
    case Mode::Author:
        s.show_author = true;
        break;
    case Mode::Launch:
        // docs/internal/gui/45 T4: the SAME windowed-shell flag Capture sets
        // (this is the same live-workflow posture, LayoutPreset::LiveObserver
        // above), but a DIFFERENT one-shot pane-focus request — bring the
        // Launch pane forward, not Processes — and no want_autoconnect:
        // connecting before the operator has typed a command to launch is
        // premature. inspect_launch_full_detail (T3) connects when "Launch &
        // trace" is actually pressed. Explicitly cleared (not just left
        // unset) because a PRIOR Capture entry may have left it true — that
        // flag is one-shot-consumed by draw_shell, not mode-scoped, so a
        // Capture -> Launch switch before draw_shell's next frame consumes it
        // would otherwise carry it over.
        s.show_inspect = true;
        s.inspect.want_autoconnect = false;
        s.inspect.want_focus_launch = true;
        break;
    }
}

// docs/internal/archive/gui/45-launch-and-window-target.md T7: the crosshair cursor
// swap needs GLFW, which ui/shell.o must NOT require to compile under the
// null-backend test tree — desktop-test explicitly promises "no display, no
// GL" (mk/desktop.mk's DESKTOP_MISSING/libglfw3-dev gate covers only the
// app/viewer builds, never desktop-test). So this TU includes glfw3.h ONLY
// when ASMTEST_DESKTOP_HAVE_GLFW is set — a per-object compile define
// mk/desktop.mk applies to shell.cpp for the app + render trees exclusively
// (mirroring author_door.cpp's ASMTEST_DESKTOP_CAN_AUTHOR split, ui/scene_
// host.h's "including it costs a backend-free TU nothing" backend-free
// split) — and the test tree gets a truthful no-op, same posture every
// other full-app-only feature degrades to there. void* (never a GLFWwindow*
// in the signature) is what lets shell.h itself, included everywhere,
// forward-declare nothing and stay GLFW-free unconditionally.
#ifdef ASMTEST_DESKTOP_HAVE_GLFW
#include <GLFW/glfw3.h>
// GLFW's own standard cursor shape — no need to rasterize the FontAwesome
// glyph into a cursor bitmap. Cached in a function-local static (created on
// first use, not a global initializer: GLFW cursor creation needs a
// current context, which is not yet true at static-init time).
static void shell_set_crosshair_cursor(void *window) {
    static GLFWcursor *crosshair =
        glfwCreateStandardCursor(GLFW_CROSSHAIR_CURSOR);
    if (window && crosshair)
        glfwSetCursor(static_cast<GLFWwindow *>(window), crosshair);
}
static void shell_restore_cursor(void *window) {
    if (window)
        glfwSetCursor(static_cast<GLFWwindow *>(window), nullptr);
}
#else
static void shell_set_crosshair_cursor(void *) {}
static void shell_restore_cursor(void *) {}
#endif

// --- docs/internal/gui/45 T7/T8: the crosshair drag's state machine --------
void shell_start_window_pick(ShellState &s) {
    s.picking_window = true;
    shell_set_crosshair_cursor(s.glfw_window);
}

void shell_finish_window_pick(ShellState &s, const PickedWindow &picked) {
    s.picking_window = false;
    shell_restore_cursor(s.glfw_window);
    if (!picked.ok) {
        // Never a silent no-op (T8 step 1) — the same shell_log_push
        // mechanism shell_select_mode already uses for every task switch.
        shell_log_push(s, picked.why_not, ToastKind::Warning);
        return;
    }
    // The SAME "full detail" attach a Processes-row double-click fires
    // (T8 step 2) — unchanged: it connects the host from saved Settings if
    // needed, sets LiveMode::Auto, and requests a start. shell_select_mode
    // is called AFTER, not instead of, folding in Capture's other side
    // effects (pending_preset, pane set) without double-logging the attach
    // itself (inspect_attach_full_detail logs nothing on its own).
    inspect_attach_full_detail(s.inspect, picked.pid);
    shell_select_mode(s, Mode::Capture);
}

// --- 20 T3: capture / restore the workspace as asmtrace-links -----------------
WorkspaceState shell_capture_workspace(const ShellState &s) {
    WorkspaceState ws;
    // The live tab (25) is ephemeral: it has no file to reopen (an empty path
    // would fail to restore), so it is never written to the store — a restored
    // workspace is exactly the file recordings.
    for (size_t i = 0; i < s.ws.recordings.size(); i++) {
        if (static_cast<int>(i) == s.live_tab)
            continue;
        ws.open.push_back(s.ws.recordings[i].path);
    }
    if (s.nav.current)
        ws.active = dt_nav_format(*s.nav.current);
    for (size_t i = 0; i < s.streams.size(); i++) {
        if (static_cast<int>(i) == s.live_tab)
            continue; // no pane link for the ephemeral live tab
        dt_link l;
        l.rec = s.streams[i].id;
        // Only the active recording carries the live selection; the model keeps
        // one selection, so a per-pane link for the others names just the
        // recording (its position restores to the top).
        if (static_cast<int>(i) == s.active_tab) {
            l.view = s.view;
            l.step = s.selection.step;
            l.off = s.selection.off;
        }
        ws.pane_links.push_back(dt_nav_format(l));
    }
    ws.recents = s.recents;
    ws.perspectives = s.perspectives;
    ws.presets = s.presets;
    return ws;
}

void shell_restore_workspace(ShellState &s, const WorkspaceState &ws) {
    s.recents = ws.recents;
    s.perspectives = ws.perspectives;
    s.presets = ws.presets;
    for (const std::string &path : ws.open) {
        std::string err;
        int idx = shell_open(s, path, err);
        if (idx < 0) {
            // A vanished recording is KEPT in recents with its error (D7 — never
            // silently dropped), so the user sees what went missing.
            recents_push(s.recents, path);
            s.status = err.empty() ? ("could not reopen " + path) : err;
        }
    }
    // Replay each recording's per-pane selection, then the active position LAST
    // so it wins — all through dt_nav_go, so a restore lands exactly like a
    // pasted link (D4). Best-effort: a link naming a recording that failed to
    // reopen refuses loudly into last_error and is skipped.
    for (const std::string &link : ws.pane_links) {
        dt_link l;
        std::string perr;
        if (dt_nav_parse(link, l, perr))
            dt_nav_go(s.nav, l);
    }
    if (!ws.active.empty()) {
        dt_link l;
        std::string perr;
        if (dt_nav_parse(ws.active, l, perr) && !dt_nav_go(s.nav, l))
            s.status = s.nav.last_error;
    }
}

// Open a recent/dropped path from the rail: open it, select it, remember it.
static void rail_open_path(ShellState &s, const std::string &path) {
    std::string err;
    int idx = shell_open(s, path, err);
    if (idx < 0) {
        s.status = err.empty() ? ("could not open " + path) : err;
        recents_push(s.recents, path); // kept-with-error (D7)
    } else {
        s.active_tab = idx;
        s.want_open_tab = idx;
    }
}

// --- 20 T2/T3: the persistent task-language entry rail ------------------------
// Replaces the old "choose a door" chooser (F13). Task SENTENCES, not nouns;
// learner-first (Learn / Open above Capture / Author); an empty workspace
// auto-lands in Learn (the default `mode`) and the rail highlights it; and the
// MRU recents land here so a start never forces recall-and-retype of a path. It
// is drawn every frame in a fixed home surface (kPaneHome when docked, a left
// child otherwise) — never a closeable `BeginTabItem` in the `main` strip.
// Public so the doc-17 interaction lane can drive the CTA clicks directly
// (test_ui), exactly as it drives draw_obs_syscalls.
void draw_home_rail(ShellState &s) {
    ImGui::TextUnformatted("asmtest — pick a task");
    ImGui::Spacing();

    const bool empty = s.ws.recordings.empty() && !s.inspect.host_started;

    auto cta = [&](Mode m, const char *caption) {
        const bool is_active = s.mode == m;
        if (is_active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(
                                                       ImGuiCol_ButtonActive));
        if (ImGui::Button(mode_cta(m)))
            shell_select_mode(s, m);
        if (is_active)
            ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", caption);
    };

    // Learner-first: the two primary CTAs, Author/Capture framed below them.
    cta(Mode::Learn, "play a bundled walkthrough — no deps, no root");
    cta(Mode::Open, "replay a .asmtrace you already have");
    ImGui::Spacing();
    ImGui::TextDisabled(empty ? "new here? start above — or, when you have "
                                "something to look at:"
                              : "or:");
    cta(Mode::Capture, "attach to a running process — see why not when you "
                       "cannot");
    // docs/internal/archive/gui/45-launch-and-window-target.md T7/T8: a THIRD way to
    // a target — drag this crosshair onto any window (even outside this
    // app's own) to attach to its owning process, Spy++-style. Greyed with a
    // STATED reason (never a hidden refusal) when unsupported: no live X11
    // session, or window-pick targets a window on THIS machine and Capture
    // is set to an ssh host (a local pick would name the wrong host's pid —
    // "Constraints & gates": the gate belongs here, where ssh_host is
    // visible, not buried in the resolver).
    {
        const bool supported = window_picker_supported();
        const bool remote = s.inspect.ssh_host[0] != '\0';
        const bool enabled = supported && !remote;
        ImGui::BeginDisabled(!enabled);
        ImGui::Button(ICON_FA_CROSSHAIRS "##window-pick");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            if (!supported)
                ImGui::SetTooltip(
                    "window-pick needs a live X11 session (including "
                    "XWayland) — none reachable here");
            else if (remote)
                ImGui::SetTooltip(
                    "window-pick targets a window on THIS machine — Connect "
                    "is set to ssh host \"%s\", so a local pick would name "
                    "the wrong host's pid",
                    s.inspect.ssh_host);
            else
                ImGui::SetTooltip(
                    "drag onto any window — even outside this app — to "
                    "attach to its process");
        }
        // Drag start: same idiom as the two existing IsMouseDragging sites
        // (shell.cpp's 3D orbit, slice_view_draw.cpp's canvas pan), just not
        // gated to inside a viewport. The !s.picking_window guard fires this
        // (and the cursor swap it triggers) exactly once per drag, not every
        // frame IsMouseDragging stays true.
        if (enabled && !s.picking_window && ImGui::IsItemActive() &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            shell_start_window_pick(s);
        // Release: checked UNCONDITIONALLY (not scoped to hovering the
        // button) — by release time the pointer can be anywhere on screen,
        // possibly over a completely different window, which is the whole
        // point of the affordance.
        if (s.picking_window &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            PickedWindow picked;
            int gx = 0, gy = 0;
            if (window_picker_global_pointer(&gx, &gy))
                picked = resolve_window_at_screen_point(gx, gy);
            else
                picked.why_not = "lost the X11 display mid-drag";
            shell_finish_window_pick(s, picked);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("or drag onto any window to target it by pointing");
    }
    // doc 45 T4: an alternate way into the SAME live workflow — start a fresh
    // process and trace it from birth, rather than pick one already running.
    // Placed right after Capture's CTA (same "or:" grouping above), not as a
    // fifth unrelated task.
    cta(Mode::Launch,
       "start a fresh process and trace it from the first instruction");
    cta(Mode::Author, "type assembly, run it, see faults as data");

    ImGui::Separator();
    ImGui::TextUnformatted("recent:");
    if (s.recents.empty())
        ImGui::TextDisabled("(nothing yet — open a trace and it lands here)");
    for (size_t i = 0; i < s.recents.size(); ++i) {
        std::string label =
            base_name(s.recents[i]) + "###recent" + std::to_string(i);
        if (ImGui::Selectable(label.c_str()))
            rail_open_path(s, s.recents[i]);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", s.recents[i].c_str());
    }

    // Details ▸ "This host" (the capability panel). It used to be an Inspector
    // tab; it now lives here, collapsed by default, so Home is the one place that
    // answers "what can this host do?" without a recording open. Help ▸ This host
    // sets want_details, which forces the header open for one frame.
    ImGui::Separator();
    if (s.want_details) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        s.want_details = false; // consumed — draw_home_rail runs once per frame
    }
    if (ImGui::CollapsingHeader("Details")) {
        const Recording *r =
            (s.active_tab >= 0 &&
             s.active_tab < static_cast<int>(s.ws.recordings.size()))
                ? &s.ws.recordings[static_cast<size_t>(s.active_tab)]
                : nullptr;
        draw_capability_panel(s.caps, r);
    }
}

// A recording's summary pane: provenance chrome + the fidelity banner + per-kind
// event counts. All of it is derived from the model, so tests assert it without
// pixels via shell_banner (the banner) and the model fields (the chrome).
static void draw_summary(const Recording &r) {
    ImGui::Text("producer: %s %s",
                r.producer.name.empty() ? "(unknown)" : r.producer.name.c_str(),
                r.producer.version.c_str());
    ImGui::Text("backend: %s (%s)",
                r.provenance.backend.empty() ? "(unknown)"
                                             : r.provenance.backend.c_str(),
                r.provenance.exact ? "exact" : "statistical");
    if (!r.provenance.trust.empty())
        ImGui::Text("trust: %s", r.provenance.trust.c_str());
    if (!r.arch.empty())
        ImGui::Text("arch: %s", r.arch.c_str());
    if (r.provenance.redacted)
        ImGui::TextUnformatted(
            "payload REDACTED at record time (absent, not hidden)");

    if (const char *banner = shell_banner(r)) {
        ImGui::Separator();
        ImGui::TextUnformatted(banner);
    }

    ImGui::Separator();
    if (r.by_kind.empty()) {
        ImGui::TextDisabled("(no events)");
    } else {
        ImGui::TextUnformatted("events by kind:");
        for (const auto &kv : r.by_kind)
            ImGui::BulletText("%s: %zu", kv.first.c_str(), kv.second.size());
    }
    if (r.unknown_kinds > 0)
        ImGui::Text("(%llu event(s) of unknown kind, kept)",
                    (unsigned long long)r.unknown_kinds);
}

// The Tab-reachable 3D viewport hit-target (22-selection-and-search.md T2, F18):
// a focusable InvisibleButton the size of the viewport region. It exists in EVERY
// branch below — including the null-backend placard paths (no GL, no Image) — so a
// keyboard-only analyst can Tab into the 3D pane and, when it holds focus, drive
// the pure Camera with no GL. That is exactly what makes the keyboard camera
// headlessly testable (CLAUDE.md: a lane that could only self-skip is not a test).
// Returns whether the target holds focus this frame.
static bool scene_viewport_target(ImVec2 size) {
    if (size.x < 16.0f)
        size.x = 16.0f;
    if (size.y < 16.0f)
        size.y = 16.0f;
    ImGui::InvisibleButton("3d-viewport", size);
    return ImGui::IsItemFocused();
}

// Apply the keyboard camera (22 T2): arrows orbit, +/=/- dolly, R resets, T is the
// plain top-down 2D-ish fallback. Routed through the SAME Camera methods the
// mouse drag uses (scene3d::camera_key), so keyboard and mouse are one code path.
// Guarded on WantTextInput exactly as handle_keymap is.
static void scene_apply_camera_keys(scene3d::Camera &cam) {
    using scene3d::CamKey;
    if (ImGui::GetIO().WantTextInput)
        return;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
        scene3d::camera_key(cam, CamKey::OrbitLeft);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
        scene3d::camera_key(cam, CamKey::OrbitRight);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        scene3d::camera_key(cam, CamKey::OrbitUp);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        scene3d::camera_key(cam, CamKey::OrbitDown);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))
        scene3d::camera_key(cam, CamKey::DollyIn);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract))
        scene3d::camera_key(cam, CamKey::DollyOut);
    if (ImGui::IsKeyPressed(ImGuiKey_R) && !ImGui::GetIO().KeyCtrl)
        scene3d::camera_key(cam, CamKey::Reset);
    if (ImGui::IsKeyPressed(ImGuiKey_T))
        scene3d::camera_key(cam, CamKey::TopDown);
}

// T3 (44-faithful-city-phase-a): the fidelity-tier -> Atmosphere mapping.
// Reads ONLY the shared palette (dt_warn_col/dt_refuse_col/dt_dim_col,
// ui/theme.h) — never an independently-chosen RGB literal — so the sky is
// byte-identical in COLOR SOURCE to the 2D fidelity banner (the load-bearing
// invariant test_shell asserts). dt_panel_bg_col is the shared dark baseline
// every tier ambient sits on; only the `front` colour and fog change by tier.
scene3d::Atmosphere scene_atmosphere_for_tier(FidelityTier tier) {
    scene3d::Atmosphere a;
    const ImVec4 amb = dt_panel_bg_col();
    ImVec4 front;
    switch (tier) {
    case FidelityTier::Neutral:
        front = dt_dim_col(); // clear: quiet, not a warning
        a.fog_density = 0.0f;
        break;
    case FidelityTier::Caution:
        front = dt_warn_col(); // amber overcast — same source as the 2D banner
        a.fog_density = 0.35f;
        break;
    case FidelityTier::Integrity:
        front = dt_refuse_col(); // red dusk — same source as the 2D banner
        a.fog_density = 0.6f;
        break;
    }
    a.ambient[0] = amb.x;
    a.ambient[1] = amb.y;
    a.ambient[2] = amb.z;
    a.front[0] = front.x;
    a.front[1] = front.y;
    a.front[2] = front.z;
    return a;
}

void draw_scene_overview(ShellState &s, const Recording &r, const Streams &a) {
    size_t i = static_cast<size_t>(s.active_tab);
    if (s.active_tab < 0 || i >= s.scenes.size())
        return;
    SceneView &sv = s.scenes[i];

    // Domain-term-first heading + "?" caveat (re-opens the primer) (24 T3/T5).
    dt_view_header("scene3d", [&sv] { dt_primer_reopen(sv.primer); });
    // First-open primer (24 T5): what the terrain/trajectory are + exact vs
    // statistical, with the semantic legend. The "3D to find, 2D to read" line
    // (formerly buried in the HUD) is promoted here.
    if (dt_primer_active(sv.primer)) {
        dt_primer(
            "scene-primer", "Address-space spacetime (3D overview)",
            "The terrain is the ADDRESS SPACE laid flat; a trajectory is one "
            "execution PATH threading across it over time. An exact path is a "
            "solid tube; statistical residency is stippled, never a solid tube "
            "(the second channel keeps sampled evidence faithfully distinct). "
            "Use "
            "3D to FIND a place, then the flat 2D views to READ it. The "
            "playhead "
            "here walks TRACE-RESIDENCY time — a different axis from the "
            "execution step the flat views brush, so the two clocks are "
            "separate "
            "(press Play to watch the path form). Reach this view with 5. "
            "Left-drag orbits; middle-drag or shift+left-drag PANS; the "
            "wheel dollies; DOUBLE-CLICK recentres on whatever is under the "
            "cursor. Know where you want to go? Use \"go to\" in the HUD to "
            "name an address or a region directly, instead of hunting for "
            "it.",
            [] { dt_semantic_legend(); }, sv.primer);
    }

    // Weave the pure, engine-free models once per recording (lazy — a recording
    // whose 3D tab is never opened pays nothing). The coarse rung's plane comes
    // from the recording's `codeimage` code regions (08-T7); a replay file with
    // none yields an empty plane, which the pane SAYS rather than drawing a void.
    if (!sv.built) {
        std::vector<space::Region> regs = space::regions_from_codeimage(r);
        sv.has_regions = !regs.empty();
        // 54 T1: observed-data-span extension — an access no codeimage region
        // maps still places, as an Unknown "observed data" span rather than a
        // dropped point. `existing` is `regs` as built so far (code only), so
        // an observed address inside a code region never creates a shadow span.
        std::string span_note;
        std::vector<space::Region> obs =
            space::observed_data_spans(r, regs, &span_note);
        regs.insert(regs.end(), obs.begin(), obs.end());
        space::Projection proj = space::build_projection(std::move(regs));
        proj.data_span_note = std::move(span_note);
        sv.terr = space::build_terrain(std::move(proj), r);
        // 36 T2: the terrain's Projection anchors a rel PC path onto the plane
        // (build_terrain moved it into sv.terr.proj above; the terrain is built
        // first precisely so it is available here).
        sv.traj = space::build_trajectories(r, sv.terr.proj);
        sv.conv = space::detect_convergences(sv.traj, sv.terr.proj);
        // 56 T2/T5: the survey aggregate does not change with the playhead,
        // so it is woven once here, like terr/traj/conv above.
        sv.hotedges_scene = obs_hotedges_for_scene(obs_hotedges_build(r));
        // 56 T5: the misprediction layer's plane-space geometry, from the
        // SAME survey aggregate just woven.
        sv.mispred = build_mispred_layer(sv.hotedges_scene, sv.terr.proj);
        // 57 T2: the kernel-crossing spurs. Woven here, on the same
        // whole-recording cadence: a syscall's stream position and the
        // instruction stream it is ordered against are both fixed for the
        // recording, so nothing about this layer moves with the playhead.
        sv.crossings = build_crossing_layer(obs_syscalls_build(r), r,
                                            sv.terr.proj);
        // 57 T4: the blame convergence forest. A set overlap over every
        // recorded cone — a whole-recording fact, so it is woven here too.
        sv.blame = space::build_blame_forest(a.blame, a.df, sv.terr.proj,
                                             r.truncated());
        // 56 T4: the opcode classification is a whole-recording fact (which
        // offsets exist and what they are), never gated on the playhead.
        sv.opcode_cells = space::build_opcode_terrain(
            sv.terr, r, space::opcode_guest_from_arch(a.arch));
        sv.hud.nsteps = sv.terr.nsteps;
        sv.hud.t = sv.terr.nsteps; // show the whole trace by default
        // 48 T4: the landmark, computed ONCE per weave alongside the models
        // above — never re-derived per frame (see SceneView::home_u's doc).
        sv.has_home = scene3d::scene_home_target(sv.terr, &sv.home_u, &sv.home_v);
        sv.built = true;
    }

    // 50 T2 (two-way-brushing): recompute the located highlight only when the
    // selection or the weave changed — never every frame regardless of
    // picking activity. Guarded on s.selection.rec == a.id (the existing
    // cross-recording brushing rule, 22 T1): a selection brushed in a
    // DIFFERENT open recording must not light this one.
    {
        const uint64_t gen = r.event_count();
        if (s.selection.rec == a.id && s.selection.active()) {
            if (sv.highlight_epoch != s.selection.epoch ||
                sv.highlight_gen != gen) {
                sv.highlight_epoch = s.selection.epoch;
                sv.highlight_gen = gen;
                sv.highlight = s.selection.off.has_value()
                                  ? space::scene_locate_off(
                                        sv.terr.proj, r, *s.selection.off)
                                  : space::scene_locate_step(
                                        sv.terr.proj, a.df,
                                        *s.selection.step);
            }
        } else {
            sv.highlight = space::Located{};
            sv.highlight_epoch = UINT64_MAX;
            sv.highlight_gen = UINT64_MAX;
        }
    }

    // 57 T3 (causal-layers): the taint front's ORIGIN — `Streams::blame` where
    // the recording carries it (a producer-stated attribution beats a UI
    // selection), otherwise the flat views' Selection step. Recomputed on the
    // same epoch/growth gate the highlight above uses, because unlike the
    // other three causal layers this one depends on a chosen origin. No
    // origin means the layer draws nothing AND says why — never a silent
    // blank.
    {
        const uint64_t gen = r.event_count();
        bool have_origin = false;
        uint32_t origin = 0;
        if (!a.blame.empty()) {
            origin = a.blame.front().step;
            have_origin = true;
        } else if (s.selection.rec == a.id && s.selection.step.has_value()) {
            origin = static_cast<uint32_t>(*s.selection.step);
            have_origin = true;
        }
        if (sv.taint_epoch != s.selection.epoch || sv.taint_gen != gen ||
            sv.taint_has_origin != have_origin ||
            sv.taint_origin != origin) {
            sv.taint_epoch = s.selection.epoch;
            sv.taint_gen = gen;
            sv.taint_has_origin = have_origin;
            sv.taint_origin = origin;
            if (have_origin) {
                // The cap is the pass's own step count: a visited-once BFS
                // never needs more hops than that, so this is the UNBOUNDED
                // walk (analysis/slice.h's own note) and `bounded` stays a
                // real signal rather than an artefact of an arbitrary cap.
                sv.taint = space::build_taint_front(
                    a.df, sv.terr.proj, origin,
                    static_cast<int32_t>(a.df.nsteps), r.truncated());
            } else {
                sv.taint = space::TaintFront{};
                sv.taint.disabled_reason =
                    "no origin to spread from — this recording carries no "
                    "`blame` attribution, and nothing is selected in it. "
                    "Select a step in a flat view to seed the front.";
            }
        }
    }

    // The HUD (its own window): provenance chips, playhead, layer toggles, camera
    // presets, region legend. Pure ImGui — drawn even under the null backend. It
    // reports the user's intent back through sv.hud; we apply it here (04's rule:
    // the view keeps no state the model does not).
    sv.hud.nsteps = sv.terr.nsteps;
    sv.hud.playing = sv.play.playing; // 34 T3: the HUD labels its Play button
    // 48 T4: sync the landmark + current camera target every frame — read by
    // draw_scene_hud for the "reset view" gate and the "you are here" text.
    sv.hud.home_u = sv.home_u;
    sv.hud.home_v = sv.home_v;
    sv.hud.has_home = sv.has_home;
    sv.hud.cam_target_u = sv.cam.target[0];
    sv.hud.cam_target_v = sv.cam.target[2];
    // 50 T2: sync the located highlight every frame — read by draw_scene_hud
    // for the "frame the selection" gate and the graded readout below it.
    sv.hud.has_highlight = sv.highlight.ok;
    sv.hud.highlight_ambiguous = sv.highlight.ambiguous;
    sv.hud.highlight_reason = sv.highlight.reason;
    // 56 T5: the misprediction layer's off-plane endpoint count — never
    // silently dropped.
    sv.hud.mispred_off_plane = sv.mispred.off_plane;
    // 57 T2: what the crossing layer could not draw, and why it may be off.
    sv.hud.crossings_disabled_reason =
        sv.crossings.enabled ? std::string() : sv.crossings.disabled_reason;
    sv.hud.crossings_before_first_insn = sv.crossings.before_first_insn;
    sv.hud.crossings_off_plane = sv.crossings.off_plane;
    // 57 T3: what the taint front could not draw, each reason kept distinct.
    sv.hud.taint_disabled_reason =
        sv.taint.enabled ? std::string() : sv.taint.disabled_reason;
    sv.hud.taint_bounded = sv.taint.bounded;
    sv.hud.taint_truncated = sv.taint.truncated;
    sv.hud.taint_reg_only_writes = sv.taint.reg_only_writes;
    sv.hud.taint_off_relative_writes = sv.taint.off_relative_writes;
    sv.hud.taint_unknown_steps = sv.taint.unknown_steps;
    sv.hud.taint_off_plane = sv.taint.off_plane;
    // 57 T4: the forest's own honesty channel.
    sv.hud.blame_disabled_reason =
        sv.blame.enabled ? std::string() : sv.blame.disabled_reason;
    sv.hud.blame_single_cone = sv.blame.single_cone;
    sv.hud.blame_truncated = sv.blame.truncated;
    sv.hud.blame_cones = sv.blame.cones;
    sv.hud.blame_born_untraced = sv.blame.born_untraced;
    sv.hud.blame_max_weight = sv.blame.max_weight;
    sv.hud.blame_off_plane_note = sv.blame.off_plane_note;
    scene3d::draw_scene_hud(sv.hud, sv.terr, sv.traj);
    // 48 T4: "reset view" frames the landmark; "default view" is the literal
    // Camera{} preset 25/34 documented — two buttons, two meanings, neither
    // silently repurposed into the other.
    if (sv.hud.req_reset_view && sv.has_home)
        sv.cam.frame(sv.home_u, sv.home_v, scene3d::Camera{}.radius);
    if (sv.hud.req_default_view)
        sv.cam.reset();
    if (sv.hud.req_top_down)
        sv.cam.top_down();
    // 48 T3: the "go to" row's resolved destination (address or region).
    if (sv.hud.req_goto)
        sv.cam.frame(sv.hud.goto_u, sv.hud.goto_v, sv.hud.goto_radius);
    // 50 T4: "frame the selection" — the located cell's centre, the same
    // cell-centre rounding resolve_pick/classify_cell already use.
    if (sv.hud.req_frame_highlight && sv.highlight.ok && sv.terr.w > 0) {
        float hu = 0.0f, hv = 0.0f;
        cell_center_uv(sv.highlight.cell, sv.terr.w, &hu, &hv);
        sv.cam.frame(hu, hv, sv.cam.radius);
    }
    sv.hud.req_reset_view = sv.hud.req_default_view = sv.hud.req_top_down =
        sv.hud.req_goto = sv.hud.req_frame_highlight = false;
    // 34 T3: play/pause the TERRAIN-TIME playhead — its own axis (trace-residency
    // steps), distinct from the execution step the flat views brush. Restart from
    // 0 when pressed at the end, then advance hud.t over terr.nsteps; the existing
    // re-slice below (sv.slice_t != sv.hud.t) redraws the terrain for the new t,
    // so the trajectory visibly forms across the surface over time.
    if (sv.hud.req_play_toggle) {
        sv.hud.req_play_toggle = false;
        sv.play.playing = !sv.play.playing;
        if (sv.play.playing && sv.hud.t >= sv.terr.nsteps) {
            sv.hud.t = 0;
            sv.play.accum = 0.0f;
        }
        // T5 (44): the SAME Play button also starts/stops the SECOND,
        // independent follow_step playhead (SceneView::follow_play) — one
        // shared user INTENT ("Play"), two separately-ticking Transports
        // (distinct accum/value), never one value read from the other. This
        // is the UI-convenience half of "both playheads advance independently
        // under Play" (T5's Done-when); see SceneView::follow_play's doc
        // comment for the axis this playhead actually walks.
        sv.follow_play.playing = sv.play.playing;
        if (sv.follow_play.playing && sv.follow_step >= sv.terr.nsteps) {
            sv.follow_step = 0;
            sv.follow_play.accum = 0.0f;
        }
    }
    if (sv.play.playing)
        sv.hud.t = transport_tick(sv.play, sv.hud.t, sv.terr.nsteps,
                                  ImGui::GetIO().DeltaTime);
    // T5: follow_step walks TrajPoint.t's own axis — approximated here by
    // terr.nsteps as the upper bound (sound for the common single-tid case
    // T5's doc comment names; a later phase's real resolver can tighten this
    // to the followed trajectory's own max t).
    if (sv.follow_play.playing)
        sv.follow_step = transport_tick(sv.follow_play, sv.follow_step,
                                        sv.terr.nsteps,
                                        ImGui::GetIO().DeltaTime);

    // 22 T2 (F18): the keyboard camera acts when the HUD (reported above) or the 3D
    // viewport holds focus. The viewport target is drawn in the branches below —
    // after these keys apply — so its focus is read from LAST frame (persisted in
    // sv.viewport_focus). s.cam_focus lets handle_keymap defer the arrow keys to
    // the camera next frame, exactly as wasd_context defers WASD.
    s.cam_focus = sv.hud.kbd_focus || sv.viewport_focus;
    if (s.cam_focus)
        scene_apply_camera_keys(sv.cam);

    // Re-slice the terrain when the playhead moved (or on first build). O(touched
    // cells), far under frame budget for a golden recording (T2 step 3) — but a
    // pathologically large terrain can exceed it, and a synchronous full slice
    // there would STALL the UI thread with no busy signal (F21). So the scrub
    // DEGRADES to the labelled coarse plane for the in-flight frame (23 T4),
    // showing progress, then lands the full slice next frame. The coarse plane is
    // the same labelled rung the terrain shows normally, so this hides nothing
    // (D7). The budget is generous — a golden-sized terrain never degrades.
    static const uint64_t kScrubCellBudget = 200000;
    if (sv.slice_t != sv.hud.t) {
        const uint64_t cells = sv.terr.code.size() + sv.terr.data.size();
        if (!sv.scrub_pending && should_degrade(cells, kScrubCellBudget)) {
            sv.slice = sv.terr.coarse_slice(); // cheap, labelled coarse
            sv.scrub_pending = true; // finish the full slice next frame
        } else {
            sv.slice = sv.terr.slice(sv.hud.t); // the full slice lands
            // 56 T2: the confidence layer's coverage-window mask — a no-op
            // when the survey stated no window (the common case).
            apply_coverage_window(sv.slice, sv.terr, sv.hotedges_scene);
            // 56 T3: the per-module skyline, t-gated the same way.
            sv.canopies = space::build_module_canopies(sv.terr, sv.hud.t);
            sv.slice_t = sv.hud.t;
            sv.scrub_pending = false;
        }
    }
    if (sv.scrub_pending)
        ImGui::TextColored(dt_maybe_col(), "%s", scrub_degrade_note());
    sv.hud.playhead_moved = false;

    // No plane to draw without regions — say so, never a blank void.
    if (!sv.has_regions) {
        ImGui::TextUnformatted(
            "no address-space regions in this recording — the 3D overview "
            "needs "
            "codeimage events (or a live maps snapshot) to place the plane. "
            "The "
            "provenance, trajectory and legend above still read.");
        // The keyboard viewport target still exists (22 T2), so a keyboard-only
        // analyst can Tab in and orbit the camera even with no plane to draw.
        sv.viewport_focus =
            scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }

    // 36 T4: regions exist (the tab opened) but NOTHING was placed on the plane —
    // a rel path whose codeimage span is absent or ambiguous (two candidate
    // spans). The HUD is a SEPARATE window, so name the reason in the pane
    // itself, the same rule as the no-regions placard: never an empty plane,
    // unlabelled. The flat plane is still drawn below; this says why it is flat.
    if (sv.terr.code.empty() && sv.traj.pc_placed == 0 &&
        (!sv.terr.anchor_error.empty() || !sv.traj.placement_note.empty())) {
        const std::string &why = !sv.terr.anchor_error.empty()
                                     ? sv.terr.anchor_error
                                     : sv.traj.placement_note;
        ImGui::TextUnformatted(
            ("nothing placed on the plane — " + why +
             ". The regions, provenance and legend above still read; 37 states "
             "the region on the wire so this resolves instead of refusing.")
                .c_str());
    }

    // T3 (44): damp the weather-sky tier transition (~0.5s) so a one-tier
    // flicker never strobes the sky — a simple lerp-toward-target each frame,
    // gated on DeltaTime (mirrors transport_tick's dt handling, 34 T3). The
    // tier itself is fidelity_severity(fidelity_facts_of(r)) — the SAME
    // computation the 2D fidelity banner uses, never a second GL-side
    // judgment. Runs every frame regardless of scene_host so the damped state
    // stays live even while the viewport placard is showing.
    {
        const scene3d::Atmosphere target =
            scene_atmosphere_for_tier(fidelity_severity(fidelity_facts_of(r)));
        if (!sv.atmo_inited) {
            sv.atmo = target; // first weave: snap, never lerp from zero-init
            sv.atmo_inited = true;
        } else {
            const float dt = ImGui::GetIO().DeltaTime;
            const float k = dt > 0.0f ? std::min(1.0f, dt / 0.5f) : 0.0f;
            for (int i = 0; i < 3; i++) {
                sv.atmo.ambient[i] +=
                    (target.ambient[i] - sv.atmo.ambient[i]) * k;
                sv.atmo.front[i] += (target.front[i] - sv.atmo.front[i]) * k;
                sv.atmo.sun_dir[i] +=
                    (target.sun_dir[i] - sv.atmo.sun_dir[i]) * k;
            }
            sv.atmo.fog_density +=
                (target.fog_density - sv.atmo.fog_density) * k;
        }
    }

    // 52 T2/T3: the GL-free flat surface. Offered in EACH of the three
    // degraded branches below, IN ADDITION to their existing placard, never
    // instead of it (the placard states WHY there is no 3D; that reason is
    // still true — D7/F5's restructure-never-remove rule) — and swappable in
    // on the real GL path via the "flat" toggle just below (T3 step 2), the
    // pane's own "3D to find, 2D to read" rule finally having a surface to
    // happen on. Built lazily, only where it is actually drawn.
    auto draw_flat_surface = [&] {
        Scene2dPlan plan =
            build_scene2d_plan(sv.terr, sv.slice, sv.traj, sv.conv, sv.hud.t);
        draw_scene2d(plan, sv.terr, sv.traj, sv.conv, a.id, sv.hud.t,
                    sv.follow_step, &a.df, [&s](const dt_link &l) {
                        if (!dt_nav_go(s.nav, l))
                            s.status = s.nav.last_error;
                    });
    };

    // The GL scene is drawn by the host threaded in from main.cpp. It is absent
    // under the null test backend (and any run with no GL context): the models +
    // HUD above are the whole pane there, with this placard in place of the
    // viewport. draw_shell links no GL — every GL touch is behind this pointer.
    if (s.scene_host == nullptr) {
        ImGui::TextDisabled(
            "3D viewport unavailable — no GL context in this build/run "
            "(headless test, or a viewer with no display). The scene's models, "
            "provenance and legend above are fully woven.");
        draw_flat_surface();
        // The Tab-reachable focus target + keyboard camera exist here too (22 T2):
        // this is precisely the null-backend path, where moving the pure Camera
        // with no GL is what makes the keyboard camera headlessly testable.
        sv.viewport_focus =
            scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }
    if (!s.scene_host->ready()) {
        ImGui::TextDisabled("3D scene did not initialise: %s",
                            s.scene_host->error());
        draw_flat_surface();
        sv.viewport_focus =
            scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int fbw = static_cast<int>(avail.x);
    int fbh = static_cast<int>(avail.y);
    if (fbw < 16)
        fbw = 16;
    if (fbh < 16)
        fbh = 16;

    SceneFrame f;
    f.terr = &sv.terr;
    f.traj = &sv.traj;
    f.conv = &sv.conv;
    f.slice = &sv.slice;
    f.canopies = &sv.canopies;         // 56 T3
    f.opcode_cells = &sv.opcode_cells; // 56 T4
    f.mispred = &sv.mispred;           // 56 T5
    f.crossings = &sv.crossings;       // 57 T2
    f.taint = &sv.taint;               // 57 T3
    f.blame = &sv.blame;               // 57 T4
    f.key = std::hash<std::string>{}(a.id);
    // Fold the recording's growth into the frame so the GL host re-uploads the
    // worldlines/arcs as a LIVE capture grows — the identity (`key`) is invariant
    // across the per-batch re-weave, but event_count advances each batch.
    f.gen = r.event_count();
    f.slice_t = sv.slice_t;
    f.cam = sv.cam;
    f.layers = sv.hud.layers;
    f.fbw = fbw;
    f.fbh = fbh;
    f.atmo = sv.atmo;                                    // T3
    f.sun = scene_sun_from_hud(sv.hud.t, sv.hud.nsteps);  // T5
    f.follow_step = sv.follow_step;                       // T5/T6
    f.has_highlight = sv.highlight.ok;                     // 50 T2
    f.highlight_cell = sv.highlight.cell;                  // 50 T2

    ImTextureID tex = s.scene_host->render(f);
    if (!tex) {
        ImGui::TextDisabled("3D scene produced no frame on this driver");
        draw_flat_surface();
        sv.viewport_focus =
            scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }

    // T3 step 2: the flat toggle swaps the reading surface in for the
    // viewport itself — "3D to find, 2D to read" (the primer's own words)
    // finally has a surface to happen on, on a driver where the GL path
    // works fine. Drawn here (not the separate HUD window) so it sits right
    // beside what it toggles. Own picking, own hover when checked; none of
    // the GL orbit/dolly/pick code below applies while it is showing.
    ImGui::Checkbox("flat surface (2D reading mode)", &sv.flat_view);
    if (sv.flat_view) {
        draw_flat_surface();
        sv.viewport_focus =
            scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }
    // The viewport is ONE focusable hit-target (22 T2): the InvisibleButton is
    // both the Tab-reach focus target (a keyboard-only analyst can reach it) and
    // the mouse hit-target for orbit/dolly/pick; the GL texture is drawn over it.
    // GL renders bottom-left origin; flip V so the image reads upright in ImGui.
    ImVec2 vp_origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("3d-viewport", ImVec2(static_cast<float>(fbw),
                                                 static_cast<float>(fbh)));
    sv.viewport_focus = ImGui::IsItemFocused();
    const bool vp_hover = ImGui::IsItemHovered();
    ImGui::GetWindowDrawList()->AddImage(
        tex, vp_origin,
        ImVec2(vp_origin.x + static_cast<float>(fbw),
               vp_origin.y + static_cast<float>(fbh)),
        ImVec2(0, 1), ImVec2(1, 0));

    // T4 (49): the trajectory-time ruler, over the viewport image — reached
    // ONLY on this real-GL path (a null backend returns above, before `tex`
    // exists, so the ruler's absence there is the SAME graceful degradation
    // the rest of this pane already practises; the HUD's axis note above
    // still reads either way).
    scene3d::draw_trajectory_ruler(
        ImGui::GetWindowDrawList(), sv.cam, vp_origin,
        ImVec2(static_cast<float>(fbw), static_cast<float>(fbh)),
        sv.terr.nsteps, s.scene_host->traj_scale());

    // T4 (50-two-way-brushing): a located selection off-screen is DISCLOSED —
    // a small edge-anchored label naming the direction — rather than moved to
    // (the follow toggle this brief marks optional-off-by-default would do
    // that) or silently absent. Never fires for "behind" (degenerate/no
    // camera set up yet); the ruler's own GL-path gate applies here too.
    if (sv.highlight.ok && sv.terr.w > 0) {
        float hu = 0.0f, hv = 0.0f;
        cell_center_uv(sv.highlight.cell, sv.terr.w, &hu, &hv);
        const float aspect =
            fbh > 0 ? static_cast<float>(fbw) / static_cast<float>(fbh) : 1.0f;
        std::string edge;
        if (!scene3d::scene_on_screen(sv.cam, hu, hv, aspect, &edge) &&
            edge != "behind") {
            const float pad = 6.0f;
            ImVec2 pos = vp_origin;
            if (edge == "left")
                pos = ImVec2(vp_origin.x + pad,
                             vp_origin.y + static_cast<float>(fbh) * 0.5f - 8.0f);
            else if (edge == "right")
                pos = ImVec2(vp_origin.x + static_cast<float>(fbw) - 100.0f,
                             vp_origin.y + static_cast<float>(fbh) * 0.5f - 8.0f);
            else if (edge == "top")
                pos = ImVec2(vp_origin.x + static_cast<float>(fbw) * 0.5f - 45.0f,
                             vp_origin.y + pad);
            else // "bottom"
                pos = ImVec2(vp_origin.x + static_cast<float>(fbw) * 0.5f - 45.0f,
                             vp_origin.y + static_cast<float>(fbh) - 20.0f);
            ImGui::GetWindowDrawList()->AddText(
                pos, IM_COL32(255, 235, 80, 255), "selection is off-screen");
        }
    }

    // Camera + pick, only while the pointer is over the viewport. A left-drag
    // orbits; the wheel dollies; a click that did NOT drag is a pick — read the
    // id under the cursor and drill OUT to the flat 2D view through 04's router
    // (3D to find, 2D to read).
    if (vp_hover) {
        ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            sv.nav_dragging = false;
        // 48 T2: a double-click recentres on whatever is under the cursor,
        // read through the SAME pick the single-click-release path uses below.
        // Set BEFORE the drag/orbit checks so the paired release (T2 step 4)
        // sees nav_dragging already true and does not also fire a pick —
        // this is the second click of the pair, not a fresh single click.
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ImVec2 origin = ImGui::GetItemRectMin();
            int px = static_cast<int>(io.MousePos.x - origin.x);
            int py = static_cast<int>(io.MousePos.y - origin.y);
            if (px >= 0 && py >= 0 && px < fbw && py < fbh) {
                uint32_t id = s.scene_host->pick(sv.cam, fbw, fbh, px, py);
                scene3d::Pick pk = scene3d::decode_pick(id, sv.terr.w);
                float u = 0.0f, v = 0.0f;
                if (scene3d::scene_recentre_target(sv.terr, sv.traj, pk, &u,
                                                   &v)) {
                    sv.cam.frame(u, v, sv.cam.radius);
                    sv.hud.nav_note.clear();
                } else {
                    sv.hud.nav_note = "double-click: nothing to recentre on "
                                      "here (background/padding)";
                }
            }
            sv.nav_dragging = true;
        }
        // 48 T1: pan on middle-drag or shift+left-drag — plain left-drag stays
        // orbit exactly as it was, so no existing muscle memory or test breaks.
        // The delta is scaled by radius so a pan feels the same dollied in or
        // out (a fixed pixel delta should cover the same fraction of the view).
        const bool pan_drag =
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 3.0f) ||
            (io.KeyShift &&
             ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f));
        if (pan_drag) {
            sv.nav_dragging = true; // suppresses the pending click-to-pick too
            const float scale = 0.0015f * sv.cam.radius;
            sv.cam.pan(-io.MouseDelta.x * scale, -io.MouseDelta.y * scale);
        } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            sv.nav_dragging = true;
            sv.cam.orbit(-io.MouseDelta.x * 0.008f, -io.MouseDelta.y * 0.008f);
        }
        if (io.MouseWheel != 0.0f)
            sv.cam.dolly(io.MouseWheel > 0.0f ? 1.0f / 1.1f : 1.1f);

        // T1/T2 (47-scene-inspect-and-pickable-overlays): the throttled hover
        // pick — reruns at most once per actual mouse-move pixel, and never
        // while a drag/orbit/pan is in progress (nav_dragging is already
        // current for this frame at this point in the block, so an orbit
        // performs ZERO readbacks, not merely "fewer"). Reuses the same
        // scene_host->pick() single-pixel path the click handler below
        // already calls — no second readback mechanism. Resolves straight to
        // a PickHint (T2) so T4's tooltip only ever reads sv.hover_hint, never
        // re-decodes/re-resolves on its own (one lookup path, like T1's
        // cell-index reuse below it).
        if (!sv.nav_dragging) {
            ImVec2 origin = ImGui::GetItemRectMin();
            ImVec2 hpx(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
            const bool over =
                hpx.x >= 0.0f && hpx.y >= 0.0f &&
                hpx.x < static_cast<float>(fbw) && hpx.y < static_cast<float>(fbh);
            if (over &&
                (hpx.x != sv.hover_px.x || hpx.y != sv.hover_px.y)) {
                sv.hover_px = hpx;
                sv.hover_id = s.scene_host->pick(
                    sv.cam, fbw, fbh, static_cast<int>(hpx.x),
                    static_cast<int>(hpx.y));
                // T3 (47): decode/resolve against the REAL uploaded bands
                // (ground truth from the scene, never re-derived) so a
                // convergence arc or an access spur hovers exactly as it
                // would click.
                const scene3d::PickBands bands = s.scene_host->pick_bands();
                scene3d::Pick hpk =
                    scene3d::decode_pick(sv.hover_id, sv.terr.w, bands);
                sv.hover_hint = scene3d::resolve_pick_hint(
                    sv.terr, sv.traj, sv.conv, hpk, sv.follow_step);
            }
        }

        // T4 (47-scene-inspect-and-pickable-overlays): the hover readout —
        // "what is this, and where would a click send me?" before any
        // navigation happens. Reads the CACHED sv.hover_hint (T2's PickHint,
        // resolved above) every frame the pointer sits over the viewport, not
        // just the frame the throttled pick re-ran on, so the tooltip tracks
        // the cursor smoothly between re-picks. Suppressed during a drag —
        // the hint reflects wherever the pointer was BEFORE the drag/orbit/pan
        // started (T1's throttle stops re-picking mid-drag), so showing it
        // while the view is spinning would read as a stale, misleading claim.
        // Text-only (no icons/shapes), so it survives 2.0x DPI and the CVD
        // palette unchanged. Background (hint.empty) shows nothing — there is
        // nothing to say about "nothing under the cursor" beyond the absence
        // of a tooltip itself.
        if (!sv.nav_dragging && !sv.hover_hint.empty) {
            const scene3d::PickHint &h = sv.hover_hint;
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(h.what.c_str());
            if (!h.where.empty())
                ImGui::TextUnformatted(h.where.c_str());
            if (!h.quantity.empty())
                ImGui::TextUnformatted(h.quantity.c_str());
            if (!h.fidelity.empty())
                ImGui::TextColored(dt_warn_col(), "%s", h.fidelity.c_str());
            const std::string click_line =
                h.target.empty() ? "click -> nothing here"
                                 : "click -> " + h.target;
            ImGui::TextColored(dt_dim_col(), "%s", click_line.c_str());
            ImGui::EndTooltip();
        }

        // T4: the click-release below already suppresses navigation whenever
        // resolve_pick returns nullopt (padding, background, an out-of-range
        // pick) — EXACTLY the same set of cases resolve_pick_hint reports as
        // target=="" (T2's anti-drift tests assert the two can never
        // disagree), so a padding/background click is already a documented
        // no-op here, not a silent nothing-happened. No separate hint.target
        // check is needed; the existing `if (link && ...)` guard below IS
        // that check, transitively.
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !sv.nav_dragging) {
            ImVec2 origin = ImGui::GetItemRectMin();
            int px = static_cast<int>(io.MousePos.x - origin.x);
            int py = static_cast<int>(io.MousePos.y - origin.y);
            if (px >= 0 && py >= 0 && px < fbw && py < fbh) {
                uint32_t id = s.scene_host->pick(sv.cam, fbw, fbh, px, py);
                const scene3d::PickBands bands = s.scene_host->pick_bands();
                scene3d::Pick pk = scene3d::decode_pick(id, sv.terr.w, bands);
                // T3 (50-two-way-brushing): `a.df` is the recording's latest
                // dataflow pass — the SAME stream the trajectory's df_step
                // vertices (if any) came from — so a Vertex pick's address
                // route can search it rather than trust the per-tid ordinal.
                auto link =
                    scene3d::resolve_pick(sv.terr, sv.traj, a.id, pk, sv.conv,
                                          sv.follow_step, &a.df);
                if (link && !dt_nav_go(s.nav, *link))
                    s.status = s.nav.last_error;
            }
        }
    }
}

// --- per-view draw bodies (19 T1) --------------------------------------------
// Each view's exact body, factored out of its old `BeginTabItem` so the two
// containers that host it — the non-docked single-window tab strip
// (draw_recording_tab, below) and the docked panes (draw_docked_shell) — share
// ONE implementation and cannot drift. The fidelity placards move WITH the body
// (D7/F5: restructured, never removed). None of these touches `s.view`; the
// caller sets that (a selected tab, or a focused pane) so a body drawn every
// frame in an always-on pane does not clobber the current-view signal.
static void body_canvas(ShellState &, const Streams *a, const Streams *b) {
    draw_canvas(b ? dt_canvas_build2(*a, *b) : dt_canvas_build(*a));
}
dt_timeline shell_timeline_model(const ShellState &s, const Streams *a,
                                 const Streams *b) {
    // The shared selection projects onto this recording ONLY when it belongs to
    // it (22 T1; selection.rec is the disambiguator). Without this, a step brushed
    // in recording A stays lit on a coincident index in recording B after a tab
    // switch — a selection misattributed to a recording the user never picked in.
    const std::optional<uint32_t> sel_step =
        s.selection.rec == a->id ? s.selection.step : std::nullopt;
    dt_slice cone;
    if (s.cone_active && sel_step)
        // `b` lights the backward cone (what produced the selection), `f` the
        // forward cone (what it feeds) — s.cone_fwd (17-T1). The cone is a derived
        // highlight OF the shared selection (22 T1), not a second selection.
        cone = s.cone_fwd
                   ? dt_slice_forward(a->df.edges, a->df.nsteps, *sel_step)
                   : dt_slice_backward(a->df.edges, a->df.nsteps, *sel_step);
    const dt_slice *lit = s.cone_active ? &cone : nullptr;
    dt_timeline t =
        b ? dt_timeline_build2(*a, *b, lit) : dt_timeline_build(*a, lit);
    // Project the shared selection into the model so the timeline marks the SAME
    // entity the other panes brush (22 T1), but only when it belongs to this
    // recording — a step absent (wrong recording, or out of range) simply does not
    // match a row: "nothing selected" here, never a fabricated row (D7).
    t.selected_step = sel_step;
    // Highlight-all for the global find (22 T3): mark every timeline hit. Find
    // MARKS, never hides — the hit set is drawn as emphasis and every row stays.
    for (const FindHit &h : s.find.hits)
        if (h.view == dt_view::timeline && h.step)
            t.find_hits.push_back(*h.step);
    return t;
}

size_t shell_apply_df_pass(ShellState &s, size_t i) {
    if (i >= s.seg_df.size() || s.seg_df[i].passes.empty())
        return 0;
    const SegmentedDataflow &seg = s.seg_df[i];
    size_t latest = seg.latest();
    int sel = (i < s.df_pass.size()) ? s.df_pass[i] : -1;
    size_t cur = (sel < 0 || static_cast<size_t>(sel) > latest)
                     ? latest
                     : static_cast<size_t>(sel);
    // Always apply the resolved pass: streams[i].df may currently hold a
    // previously pinned pass (clearing a pin must restore the latest), and a live
    // rebuild resets it to the latest (a pin must be re-enforced). The caller only
    // reaches here for a multi-pass recording, so a one-shot pays no copy.
    if (i < s.streams.size())
        s.streams[i].df = seg.passes[cur];
    return cur;
}

// The per-pass invocation pager (40 T2). A continuous dataflow/auto capture is many
// invocation passes in one recording (35 T1); the dataflow views show ONE at a
// time. When the active recording has more than one pass, draw the DISCRETE
// invocation stepper (24 T4's one time-position widget — between two invocations
// the target ran unobserved, so it is deliberate discrete paging, never a faked
// scrub) and apply the choice to the cached Streams::df the views read. Following
// the latest by default (the live default), pinnable to an earlier pass. A one-shot
// recording (one pass) draws nothing — the chrome is byte-identical to pre-40.
static void shell_df_pass_pager(ShellState &s) {
    if (s.active_tab < 0 || static_cast<size_t>(s.active_tab) >= s.seg_df.size())
        return;
    size_t i = static_cast<size_t>(s.active_tab);
    int npasses = static_cast<int>(s.seg_df[i].passes.size());
    if (npasses <= 1)
        return; // one pass (or none): decode already set df; no chrome, no copy
    int latest = npasses - 1;
    int sel = (i < s.df_pass.size()) ? s.df_pass[i] : -1;
    int cur = (sel < 0 || sel > latest) ? latest : sel;
    if (dt_timepos_step("invocation", &cur, npasses,
                        dt_timepos_discrete_reason("invocations")))
        // Stepping to the newest re-enables follow-latest; else pin the choice.
        s.df_pass[i] = (cur == latest) ? -1 : cur;
    size_t applied = shell_apply_df_pass(s, i);
    ImGui::SameLine();
    if (i < s.df_pass.size() && s.df_pass[i] < 0)
        ImGui::TextDisabled("· pass %zu of %d — following the latest",
                            applied + 1, npasses);
    else
        ImGui::TextDisabled("· pass %zu of %d — pinned (latest is %d)",
                            applied + 1, npasses, npasses);
}

// The Loom's reweave input (42 T1): a plain-data view of the Author door's
// last run, borrowed from ShellState::author for exactly one frame. `code` is
// left null (and `code_sha` empty) whenever there is nothing eligible — no
// Author run yet, or one whose bytes never got a sha (author_door.cpp
// withholds it for any arch but x86-64, the resume seam's scope) — so the
// identity latch (loom_reweave_available) correctly refuses downstream with
// no arch-awareness needed at this call site. Reads `frozen_args`/
// `frozen_nargs`, NEVER the live `args`/`nargs` widget fields — those keep
// mutating after a Run with no further Run, so they can silently disagree
// with the args that actually produced `image`/`image_sha`.
static ReweaveSource shell_reweave_source(const ShellState &s) {
    ReweaveSource rw;
    if (!s.author.image_sha.empty()) {
        rw.code = s.author.image.data();
        rw.code_len = s.author.image.size();
        rw.args = s.author.frozen_args;
        rw.nargs = s.author.frozen_nargs;
        rw.code_sha = s.author.image_sha;
    }
    return rw;
}

static void body_timeline(ShellState &s, const Streams *a, const Streams *b) {
    shell_df_pass_pager(s); // 40 T2: pick which invocation pass this view shows
    dt_timeline t = shell_timeline_model(s, a, b);

    // Always-visible overview/minimap (21-spine-navigation.md T3): the whole-trace
    // density above the table, with the current viewport (the ImZoomSlider window)
    // drawn and a click routed OUT through dt_nav_go — a minimap click and a typed
    // go-to land identically (D4). Completes doc-14 T5's timeline-windowing stub.
    const uint32_t nsteps = a->df.nsteps;
    if (s.tl_hi <= s.tl_lo) {
        s.tl_lo = 0;
        s.tl_hi = nsteps; // first-frame default: the whole trace
    }
    draw_timeline_overview(t, nsteps, &s.tl_lo, &s.tl_hi,
                           [&s, a](uint32_t step) {
                               dt_link l;
                               l.view = dt_view::timeline;
                               l.rec = a->id;
                               l.step = step;
                               if (!dt_nav_go(s.nav, l))
                                   s.status = s.nav.last_error;
                           });

    draw_timeline(t);
}
static void body_slice(ShellState &s, const Streams *a, const Streams *b) {
    dt_view_header("slice");
    shell_df_pass_pager(s); // 40 T2: pick which invocation pass this view shows
    draw_slice_view(dt_slice_view_build(*a, s.selection.step));
    if (b != nullptr) {
        // The two-recording state-diff (33 R6 T2): the `statediff` producer now
        // gives the merge an exact per-step basis, gated on routine identity — a
        // wrong-routine pair is refused before any state is merged.
        dt_statediff sd = dt_statediff_build(*a, *b);
        ImGui::Separator();
        if (!sd.err.empty()) {
            ImGui::TextDisabled("slice diff refused: %s", sd.err.c_str());
        } else if (!sd.merged) {
            ImGui::TextDisabled("slice diff needs a statediff stream on both "
                                "sides — %s",
                                sd.note.c_str());
        } else if (sd.steps.empty()) {
            ImGui::TextDisabled(
                "slice diff: the two recordings evolve state identically%s",
                sd.bounded ? " (within the recorded window)" : "");
        } else {
            ImGui::TextDisabled("slice diff: %zu step(s) diverge%s",
                                sd.steps.size(),
                                sd.bounded ? " (bounded)" : "");
            for (const dt_state_step &ss : sd.steps) {
                std::string regs;
                for (const std::string &r : ss.regs)
                    regs += (regs.empty() ? "" : " ") + r;
                ImGui::BulletText("step %u%s %s", ss.step,
                                  ss.bounded ? " [bounded]" : "", regs.c_str());
            }
        }
    }
}
static void body_diff(ShellState &s, const Streams *a, const Streams *b) {
    dt_view_header("diff");
    if (b == nullptr)
        ImGui::TextDisabled("attach a second recording (press d) to compare");
    else
        draw_diff_view(dt_diff_view_build(*a, *b), [&s](const dt_link &l) {
            if (!dt_nav_go(s.nav, l))
                s.status = s.nav.last_error;
        });
}
static void body_observer(ShellState &s, const Recording &r, const Streams *a) {
    // The live views (08), over a recording. They are the SAME code that renders
    // a live session in the Inspect door — how every one is testable without
    // hardware. The `observer` inner tab bar is the ONE remaining sub-level: it
    // sits directly inside its pane, so nesting stays <=2 deep (19 T2).
    size_t i = static_cast<size_t>(s.active_tab);
    if (i < s.observers.size())
        draw_observer(s.observers[i], r, a->id, [&s](const dt_link &l) {
            if (!dt_nav_go(s.nav, l))
                s.status = s.nav.last_error;
        });
}
static void shell_live_weave_banner(const ShellState &s); // defined below
static void body_scrubber(ShellState &s) {
    // The register time-travel scrubber (09-T3). An absent producer draws its own
    // placard (never a register file of zeros), exactly as the standalone draw
    // does. The playhead is the caller's; draw_scrubber returns the moved value.
    dt_view_header("scrubber");
    // 26 T4: a LIVE regstate ring is the real architectural register file, but
    // captured perturbingly (single-step) over a still-growing capture — carry the
    // same perturb+torn caveat the live Loom/Slice show (self-gates to the live
    // tab). The register VALUES are ground truth; the run is not pristine.
    shell_live_weave_banner(s);
    size_t i = static_cast<size_t>(s.active_tab);
    if (i >= s.stepidx.size())
        return;
    // 30 R3 T4: mutable so a synthesise action can REPLACE the (absent) index in
    // place with a re-derived one; `rec` drives that offer's replayability check.
    StepIndex &idx = s.stepidx[i];
    const Recording *rec =
        i < s.ws.recordings.size() ? &s.ws.recordings[i] : nullptr;
    const Streams *a = shell_a(s);
    // 34 T1: the Scrubber joins the shared execution-step brush (22 T1). Seed the
    // playhead from the shared selection when it belongs to THIS recording, so a
    // step brushed anywhere (a timeline/slice/Loom pick, a j/k step, a play tick)
    // seeds the register file here too. Clamped to the ring; a brush in another
    // recording projects to nothing (the selection-scoping gate), and a step past
    // the held prefix shows the Scrubber's own torn edge, never a neighbour.
    const uint64_t maxstep =
        idx.present() && idx.total_steps() > 0 ? idx.total_steps() - 1 : 0;
    if (a)
        if (auto seed = playhead_project(s.selection, a->id, maxstep))
            s.scrubber_playhead[i] = *seed;
    // 34 T3: play/pause the execution-step playhead. The button only toggles the
    // transport; draw_shell advances it centrally so playback continues even when
    // this pane is not the visible tab. Restart from 0 when pressed at the end.
    if (ImGui::SmallButton(s.play.playing ? "Pause###scrubplay"
                                          : "Play###scrubplay")) {
        s.play.playing = !s.play.playing;
        if (s.play.playing && a && a->df.nsteps > 0) {
            const uint64_t cur =
                s.selection.rec == a->id ? s.selection.step.value_or(0) : 0;
            if (cur >= a->df.nsteps - 1) {
                s.selection.set(a->id, 0, std::nullopt);
                s.play.accum = 0.0f;
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("play — step (execution)");
    const uint64_t before = s.scrubber_playhead[i];
    s.scrubber_playhead[i] = draw_scrubber(idx, before, rec);
    // 34 T1: a manual scrub brushes the shared selection through the ONE writer
    // (D4), so the timeline / slice / Loom follow to the same step.
    if (a && s.scrubber_playhead[i] != before)
        s.selection.set(a->id, static_cast<uint32_t>(s.scrubber_playhead[i]),
                        std::nullopt);
}
static void body_abixray(ShellState &s, const Streams *a, const Streams *b) {
    // The ABI x-ray (09-T4): locks the active recording (the SysV leg) against
    // the attached B (the Win64 leg) — the Diff tab's A/B mechanism. The view's
    // own fidelity banners handle an unaligned pair / an absent per-pane producer.
    dt_view_header("abixray");
    if (b == nullptr) {
        ImGui::TextDisabled(
            "attach the Win64 leg (press d) — the ABI x-ray locks this "
            "recording (the SysV leg) against it");
        return;
    }
    size_t ai = static_cast<size_t>(s.active_tab);
    size_t bi = static_cast<size_t>(s.b_index);
    // The rail MUTATES the walk (stop navigation), so it persists across frames;
    // rebuild only when the pair changes.
    std::string key = a->id + "\x1f" + b->id;
    if (s.abixray_key != key) {
        s.abixray_key = key;
        s.abixray_walk = wt_build(s.ws.recordings[ai]);
        s.abixray_playhead = dt_abixray_playhead(s.abixray_walk);
    }
    if (ai < s.stepidx.size() && bi < s.stepidx.size())
        draw_abixray(s.stepidx[ai], s.stepidx[bi], s.abixray_walk,
                     s.abixray_playhead);
}

// The Inspect door's "open this capture in the Loom" hand-off (07): the door
// cannot reach the Workspace, so it posts a path here; the shell opens it and
// asks the tab strip / Home list to select it (want_open_tab) and its Loom
// (want_loom). Shared by both the docked and non-docked shells.
static void handle_inspect_open_request(ShellState &s) {
    if (s.inspect.open_request.empty())
        return;
    std::string path = s.inspect.open_request;
    s.inspect.open_request.clear();
    // 25 T3: opening a saved capture makes it a permanent, persisted file tab.
    // If an ENDED session's live tab was mirroring that same run, it is now a
    // redundant ephemeral copy — retire it BEFORE the open (so the file appends
    // cleanly and its index is stable) and mark the completed recording adopted
    // so shell_sync_live_tab does not re-promote it. A still-growing session
    // keeps its live tab: the saved file is a frozen snapshot of a run still in
    // flight, genuinely distinct.
    if (s.live_tab >= 0 && s.inspect.session.growing() == nullptr) {
        s.live_dismissed_done = s.inspect.session.recordings().size();
        shell_retire_live_tab(s);
    }
    std::string err;
    int idx = shell_open(s, path, err);
    if (idx < 0) {
        s.status = err.empty() ? ("could not open " + path) : err;
    } else {
        s.want_open_tab = idx;
        s.want_loom = true;
        s.show_inspect = false; // we are leaving the door for the Loom
    }
}

// The Learn door's "play this stop" callback: open the recording and route the
// stop through 04's router so a stop and a pasted deep link land identically.
static void learn_open(ShellState &s, const std::string &path, long step) {
    std::string err;
    if (shell_open(s, path, err) < 0 && !err.empty())
        s.status = err;
    dt_link l;
    l.rec = recording_id(path);
    l.view = dt_view::timeline;
    if (step >= 0)
        l.step = static_cast<uint32_t>(step);
    shell_wire_nav(s);
    if (!dt_nav_go(s.nav, l))
        s.status = s.nav.last_error;
}

// --- 20 T1: the data-driven view set -----------------------------------------
// Is a second recording attachable as B (for Diff / ABI x-ray)? True when one is
// already attached, or the workspace holds another the `d` binding can grab.
static bool shell_b_attachable(const ShellState &s) {
    return s.b_index >= 0 || s.ws.recordings.size() >= 2;
}

// Compute the presence set for the active recording × the current mode. The
// per-recording observer deck / regstate index feed the predicate; an unopened
// (a == nullptr) recording yields an empty set (only Summary is drawn).
static std::vector<ViewPresence>
shell_view_presence(const ShellState &s, const Recording &r, const Streams *a) {
    if (a == nullptr)
        return {};
    static const ObserverState kEmptyObs;
    static const StepIndex kEmptySi;
    size_t i = static_cast<size_t>(s.active_tab);
    const ObserverState &obs =
        i < s.observers.size() ? s.observers[i] : kEmptyObs;
    const StepIndex &si = i < s.stepidx.size() ? s.stepidx[i] : kEmptySi;
    bool is_live = s.live_tab >= 0 && s.active_tab == s.live_tab;
    return view_presence(*a, obs, si, r, s.mode, shell_b_attachable(s),
                         is_live);
}

// 25 T5: the live-weave banner. While the active tab is the still-growing live
// capture, the exact-dataflow views (Loom / Slice) carry the graded truth (23):
// the values are real, but the target is being single-stepped (perturbing) and
// the recording is not yet complete (torn). A completed capture drops the
// caveat — it is exact and final, exactly like a replayed file.
static void shell_live_weave_banner(const ShellState &s) {
    if (s.live_tab < 0 || s.active_tab != s.live_tab)
        return;
    if (s.inspect.session.growing() == nullptr)
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, dt_warn_col());
    ImGui::TextWrapped("live weave — the target is being single-stepped "
                       "(perturbing), and this "
                       "recording is still growing (torn).");
    ImGui::PopStyleColor();
}

// Draw one present view's body by id. Only ever called for a present entry with
// a decoded stream (a != nullptr), so the *a dereferences are safe.
static void draw_view_body(ShellState &s, ViewId id, const Recording &r,
                           const Streams *a, const Streams *b) {
    switch (id) {
    case ViewId::Summary:
        draw_summary(r);
        break;
    case ViewId::Canvas:
        body_canvas(s, a, b);
        break;
    case ViewId::Timeline:
        body_timeline(s, a, b);
        break;
    case ViewId::Slice:
        shell_live_weave_banner(s);
        body_slice(s, a, b);
        break;
    case ViewId::Diff:
        body_diff(s, a, b);
        break;
    case ViewId::Observer:
        body_observer(s, r, a);
        break;
    case ViewId::Loom:
        shell_live_weave_banner(
            s); // 25 T5: perturb+torn caveat on a live weave
        shell_df_pass_pager(s); // 40 T2: pick which invocation pass this weave shows
        {
            ReweaveSource rw = shell_reweave_source(s);
            draw_loom(
                s.loom, *a, s.ws, s.active_tab,
                [&s](const dt_link &l) {
                    if (!dt_nav_go(s.nav, l))
                        s.status = s.nav.last_error;
                },
                &s.selection, &s.undo, &rw);
        }
        break;
    case ViewId::Scrubber:
        body_scrubber(s);
        break;
    case ViewId::AbiXray:
        body_abixray(s, a, b);
        break;
    case ViewId::Scene3D:
        draw_scene_overview(s, r, *a);
        break;
    }
}

// The ONE faithful "unavailable views (N)" affordance body (T1 step 4, D7). It
// names every absent view and its verbatim machine reason — the truth "this
// recording cannot fill view X" stays on screen, graded below the present set
// rather than shown as N empty peer tabs. `focus` is the view a keymap request
// named while absent, so it is explained FIRST.
static void draw_unavailable_views(const std::vector<ViewPresence> &vp,
                                   std::optional<dt_view> focus) {
    ImGui::TextWrapped(
        "These views cannot be filled by this recording in this mode. Each "
        "reason below is the machine's, verbatim — the view is graded here, "
        "not "
        "hidden.");
    ImGui::Spacing();
    auto row = [](const ViewPresence &e, bool lead) {
        ImGui::BulletText("%s%s", e.label, lead ? "  (requested)" : "");
        ImGui::Indent();
        ImGui::PushStyleColor(ImGuiCol_Text, dt_warn_col());
        ImGui::TextWrapped("%s", e.reason.c_str());
        ImGui::PopStyleColor();
        ImGui::Unindent();
    };
    // The requested-but-absent view first, then the rest in order.
    if (focus)
        for (const ViewPresence &e : vp)
            if (!e.present && e.view && *e.view == *focus)
                row(e, true);
    for (const ViewPresence &e : vp)
        if (!e.present && !(focus && e.view && *e.view == *focus))
            row(e, false);
}

// One open recording's pane, NON-DOCKED path (the null backend's default and any
// build with no dockspace): the summary chrome (03) then the replay views over
// it (04), as a single-window tab strip — now DATA-DRIVEN (20 T1): only the
// views this recording × mode can fill are emitted as tabs; the rest collapse
// into one "unavailable views (N)" affordance. The docked path
// (draw_docked_shell) distributes these same bodies across real panes instead.
static void draw_recording_tab(ShellState &s, const Recording &r) {
    const Streams *a = shell_a(s);
    const Streams *b = shell_b(s);
    std::vector<ViewPresence> vp = shell_view_presence(s, r, a);

    // Did a 1/2/3/4 keymap request name a view that is ABSENT? It must land on
    // the affordance with that view pre-explained, never a silent no-op (T1 s3).
    std::optional<dt_view> absent_want;
    if (s.want_view)
        for (const ViewPresence &e : vp)
            if (!e.present && e.view && *e.view == *s.want_view)
                absent_want = *s.want_view;
    // 34 T2: the same for a `5` / "View in 3D" request naming an absent ViewId
    // (the 3D overview with no codeimage regions) — routed to the affordance, its
    // machine reason shown, never a silent no-op.
    bool absent_want_id = false;
    if (s.want_view_id)
        for (const ViewPresence &e : vp)
            if (!e.present && e.id == *s.want_view_id)
                absent_want_id = true;

    if (ImGui::BeginTabBar("views")) {
        if (a == nullptr) {
            if (ImGui::BeginTabItem("Summary")) {
                draw_summary(r);
                ImGui::EndTabItem();
            }
        } else {
            for (const ViewPresence &e : vp) {
                if (!e.present)
                    continue;
                ImGuiTabItemFlags fl = 0;
                if (e.view && s.want_view == e.view)
                    fl |= ImGuiTabItemFlags_SetSelected;
                if (s.want_view_id && e.id == *s.want_view_id)
                    fl |= ImGuiTabItemFlags_SetSelected; // 34 T2 (e.g. Scene3D)
                if (e.id == ViewId::Loom && s.want_loom)
                    fl |= ImGuiTabItemFlags_SetSelected;
                if (ImGui::BeginTabItem(e.label, nullptr, fl)) {
                    if (e.view)
                        s.view = *e.view;
                    draw_view_body(s, e.id, r, a, b);
                    ImGui::EndTabItem();
                }
            }
            size_t nabs = view_absent_count(vp);
            if (nabs > 0) {
                std::string label = "unavailable views (" +
                                    std::to_string(nabs) + ")###unavail";
                ImGuiTabItemFlags fl = (absent_want || absent_want_id)
                                           ? ImGuiTabItemFlags_SetSelected
                                           : 0;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, fl)) {
                    draw_unavailable_views(vp, absent_want);
                    ImGui::EndTabItem();
                }
            }
        }
        // App panels, not recording views: always available. ("This host" moved
        // to Home ▸ Details; reach it there or via Help ▸ This host.)
        if (ImGui::BeginTabItem("Backends")) {
            draw_completeness(s.completeness, s.repo_root);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Terms")) {
            dt_view_header("terms");
            dt_terms_pane();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!s.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", s.status.c_str());
    }
}

// The recording-open dialog: a text field + Open/Cancel. A Workspace::open error
// renders verbatim in the dialog — never a silent no-op.
static void draw_open_dialog(ShellState &s) {
    // Pure-ImGui file picker (14-quick-wins.md T7): identical on Linux, macOS and
    // inside docker-desktop — no zenity/osascript, which is what keeps the lane
    // testable. Replaces the bare InputText path field.
    ImGuiFileDialog *fd = ImGuiFileDialog::Instance();
    if (!fd->IsOpened("dlg_open")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ".";
        cfg.flags = ImGuiFileDialogFlags_Modal;
        fd->OpenDialog("dlg_open", "Open a .asmtrace recording", ".asmtrace",
                       cfg);
    }
    if (fd->Display("dlg_open", ImGuiWindowFlags_NoCollapse,
                    ImVec2(520, 360))) {
        if (fd->IsOk()) {
            std::string err;
            int idx = shell_open(s, fd->GetFilePathName(), err);
            if (idx < 0)
                s.open_error = err;
            else {
                s.open_error.clear();
                s.active_tab = idx;
            }
        }
        fd->Close();
        s.open_dialog = false; // done, whether Ok or Cancel
    }
    // The refusal is first-class: surface it in the status bar (the dialog is
    // gone by now), never swallowed.
    if (!s.open_error.empty())
        s.status = "open failed: " + s.open_error;
}

// The advertised keyboard bindings (dt_nav_bindings), acted on HERE — one place,
// so the help overlay and the behaviour cannot drift (17-interaction-testing-
// and-editor.md T1). Called once per frame at the top of draw_shell. `[`/`]` are
// NOT here: they walk a dependence generation and stay view-local (scrubber /
// slice / abixray draws). Every binding is a pure ShellState mutation, so the
// engine tests drive it as a model (a KeyPress then an assert on the state),
// never against pixels.
//
// Guarded on WantTextInput: while a text field has focus (the go-to box, the
// Author editor, a filter) the letters are text, not commands.
static void handle_keymap(ShellState &s) {
    ImGuiIO &io = ImGui::GetIO();
    if (io.WantTextInput)
        return;

    // 1/2/3/4/5 — switch the active recording's view. A keypress cannot select an
    // ImGui tab directly, so record the intent; the view tab bar honours it with
    // SetSelected next pass (want_view, mirroring want_loom). 1-4 are the router
    // views (want_view); 5 is the 3D overview, which has no dt_view spelling, so
    // it routes by ViewId (want_view_id, 34 T2). The two are mutually exclusive —
    // setting one clears the other so a single frame never forces two tabs.
    if (ImGui::IsKeyPressed(ImGuiKey_1)) {
        s.want_view = dt_view::canvas;
        s.want_view_id.reset();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_2)) {
        s.want_view = dt_view::timeline;
        s.want_view_id.reset();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_3)) {
        s.want_view = dt_view::slice;
        s.want_view_id.reset();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_4)) {
        s.want_view = dt_view::diff;
        s.want_view_id.reset();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_5)) {
        s.want_view_id = ViewId::Scene3D;
        s.want_view.reset();
    }

    // y / Ctrl+C — copy a deep link to the current position to the clipboard.
    // Ctrl+C is the convention alias (18-breach-stops.md T1); both are wired.
    if (((ImGui::IsKeyPressed(ImGuiKey_Y) && !io.KeyCtrl) ||
         ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) &&
        s.nav.current)
        ImGui::SetClipboardText(dt_nav_format(*s.nav.current).c_str());

    // Ctrl+G — open the go-to-step/offset modal (its InputText owns the text).
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G))
        s.show_goto = true;

    // Ctrl+Shift+P / Ctrl+P — open the command palette (21-spine-navigation.md
    // T1). Both chords open it (IsKeyChordPressed matches the exact modifiers, so
    // the two do not double-fire); the palette's InputText owns its query text.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_P) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P))
        s.show_palette = true;

    // Ctrl+F — open the global find (22 T3, F17): highlight-all across the
    // timeline (+ minimap seam), match count and aggregate cost, Enter/Shift+Enter
    // cycling. IsKeyChordPressed mirrors Ctrl+G; the find bar owns the query text.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        s.find.open = true;
        s.find.focus_query = true;
    }

    // Ctrl+Z / Ctrl+Y (and Ctrl+Shift+Z) — the app-level undo/redo (22 T4, F12)
    // over reversible view-model state (filter / cone / selection / take set).
    // DISTINCT from the Author editor's own text undo: this whole function returns
    // early on io.WantTextInput, so while the editor holds focus these never fire
    // and its Ctrl+Z stays its own. The two undos own disjoint state.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z) && !io.KeyShift) {
        if (const UndoCommand *c = s.undo.undo())
            undo_apply(s, *c, /*redo=*/false);
    }
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) {
        if (const UndoCommand *c = s.undo.redo())
            undo_apply(s, *c, /*redo=*/true);
    }

    // Ctrl+O — open the file dialog (the File menu advertises this key, 20 T3).
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
        s.open_dialog = true;
        s.open_error.clear();
    }

    // Ctrl+Shift+R — reset the panel layout to the default (T2). The intent is
    // consumed near the dockspace build, so it fires with or without the menu.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R))
        s.want_layout_reset = true;

    // Alt+Left / Alt+Right — walk the router's back/forward history (T6, F11).
    // They land through the SAME dt_nav_go path a fresh navigation takes, so a
    // back-jump is indistinguishable from re-clicking the earlier link.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_LeftArrow))
        if (!dt_nav_back(s.nav))
            s.status = s.nav.last_error;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Alt | ImGuiKey_RightArrow))
        if (!dt_nav_forward(s.nav))
            s.status = s.nav.last_error;

    // Shift+F — fit / frame the current selection (T1; the Perfetto/Tracy gesture,
    // on Shift+F because plain `f` already lights the forward cone). Sets a
    // want_fit intent the active spatial view honours, mirroring want_view.
    if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F))
        s.want_fit = true;

    // W/S/A/D — camera zoom/pan, but ONLY when a spatial pane (timeline / 3D)
    // holds focus (wasd_context, set by the docked shell). This is the labelled
    // context switch that resolves the D-vs-diff conflict: outside a spatial
    // pane `d` keeps its diff meaning (below); inside one WASD drives the camera
    // and `d` does not toggle the diff. The nudges accumulate into testable
    // state the spatial views read.
    if (s.wasd_context) {
        if (ImGui::IsKeyPressed(ImGuiKey_W))
            s.wasd_zoom++;
        if (ImGui::IsKeyPressed(ImGuiKey_S))
            s.wasd_zoom--;
        if (ImGui::IsKeyPressed(ImGuiKey_A))
            s.wasd_pan--;
        if (ImGui::IsKeyPressed(ImGuiKey_D))
            s.wasd_pan++;
    }

    // The rest act on the active recording's selection; with none, nothing to do.
    const Streams *a = shell_a(s);
    if (a == nullptr)
        return;
    // The step space is the dataflow's — the same one the slice/cone index, so a
    // stepped selection lands on a node the slice explorer can actually show.
    const uint32_t maxstep = a->df.nsteps > 0 ? a->df.nsteps - 1 : 0;
    const uint32_t cur = s.selection.step.value_or(0);
    const uint32_t kPage = 20;
    // Snapshot the shared selection (22 T1): any step/off move below bumps the
    // epoch ONCE at the end, so every pane's projection notices the brush moved.
    const auto sel_step0 = s.selection.step;
    const auto sel_off0 = s.selection.off;

    // j/k, Down/Up — next / previous step (clamped to the step space). F10/F11
    // are the debugger-muscle-memory aliases, and `,`/`.` step to the previous /
    // next sibling — in the flat step space the adjacent invocation (18-breach-
    // stops.md T1). All are pure selection moves. The ARROWS defer to the 3D
    // camera when a 3D pane holds focus (22 T2, s.cam_focus): there Up/Down orbit
    // instead of stepping, exactly as WASD defers via wasd_context.
    if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_F10) ||
        ImGui::IsKeyPressed(ImGuiKey_Period) ||
        (!s.cam_focus && ImGui::IsKeyPressed(ImGuiKey_DownArrow)))
        s.selection.step = cur < maxstep ? cur + 1 : maxstep;
    if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_F11) ||
        ImGui::IsKeyPressed(ImGuiKey_Comma) ||
        (!s.cam_focus && ImGui::IsKeyPressed(ImGuiKey_UpArrow)))
        s.selection.step = cur > 0 ? cur - 1 : 0;
    // PgDn/PgUp — page through the step space.
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        s.selection.step = cur + kPage <= maxstep ? cur + kPage : maxstep;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        s.selection.step = cur > kPage ? cur - kPage : 0;

    // Enter — open the slice explorer at the selected step, cone lit.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        s.want_view = dt_view::slice;
        s.cone_active = true;
    }

    // b / f — light the backward / forward cone from the selection; c — clear.
    // `f` (no Shift) is the forward cone; Shift+F is fit-selection, handled above,
    // so the cone must not also fire on the capital letter. Each cone CHANGE is a
    // reversible undo Command (22 T4): the cone is a derived highlight of the
    // selection, so Ctrl+Z restores the exact prior cone state.
    {
        const bool cone_a0 = s.cone_active, cone_f0 = s.cone_fwd;
        if (ImGui::IsKeyPressed(ImGuiKey_B)) {
            s.cone_active = true;
            s.cone_fwd = false;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyShift && !io.KeyCtrl) {
            s.cone_active = true;
            s.cone_fwd = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C) && !io.KeyCtrl)
            s.cone_active = false;
        if (s.cone_active != cone_a0 || s.cone_fwd != cone_f0) {
            UndoCommand cmd;
            cmd.kind = UndoCommand::Kind::Cone;
            cmd.cone_active_before = cone_a0;
            cmd.cone_fwd_before = cone_f0;
            cmd.cone_active_after = s.cone_active;
            cmd.cone_fwd_after = s.cone_fwd;
            s.undo.push(std::move(cmd));
        }
    }

    // d — attach a second recording for the diff (the first OTHER open one), or
    // detach the one attached. x — swap which is A and which is B. In wasd_context
    // (a spatial pane holds focus) `d` is camera-pan instead (handled above), so
    // the diff toggle is suppressed there — the labelled context switch (T1).
    if (ImGui::IsKeyPressed(ImGuiKey_D) && !s.wasd_context) {
        if (s.b_index >= 0) {
            s.b_index = -1;
        } else {
            for (int i = 0; i < static_cast<int>(s.ws.recordings.size()); i++)
                if (i != s.active_tab) {
                    s.b_index = i;
                    break;
                }
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_X) && s.b_index >= 0) {
        // A (the active recording) is owned by the outer recording-tab bar, so a
        // swap asks it to select the old B next frame (want_open_tab, honoured in
        // this same frame) while B takes the old A. Assigning active_tab directly
        // would be overwritten by the tab bar.
        const int old_a = s.active_tab;
        s.want_open_tab = s.b_index;
        s.b_index = old_a;
    }

    // n / p — walk to the next / previous divergent offset of the pair. Needs a
    // B (d); with none, or a pair with no per-offset divergence, it says why
    // rather than moving the selection to a position it cannot justify.
    if ((ImGui::IsKeyPressed(ImGuiKey_N) || ImGui::IsKeyPressed(ImGuiKey_P)) &&
        !io.KeyCtrl) {
        const bool fwd = ImGui::IsKeyPressed(ImGuiKey_N);
        const Streams *b = shell_b(s);
        if (b == nullptr) {
            s.status = "n/p: attach a second recording (d) to walk divergences";
        } else {
            dt_diff d;
            std::string derr;
            if (dt_diff_build(*a, *b, d, derr) && !d.heat.empty()) {
                std::vector<uint64_t> offs;
                offs.reserve(d.heat.size());
                for (const dt_heat_delta &hd : d.heat)
                    offs.push_back(hd.off);
                std::sort(offs.begin(), offs.end());
                const uint64_t curoff =
                    s.selection.off.value_or(fwd ? 0 : UINT64_MAX);
                std::optional<uint64_t> tgt;
                if (fwd) {
                    for (uint64_t o : offs)
                        if (o > curoff) {
                            tgt = o;
                            break;
                        }
                } else {
                    for (auto it = offs.rbegin(); it != offs.rend(); ++it)
                        if (*it < curoff) {
                            tgt = *it;
                            break;
                        }
                }
                if (tgt)
                    s.selection.off = *tgt;
                else
                    s.status = fwd ? "n: at the last divergence"
                                   : "p: at the first divergence";
            } else {
                s.status = derr.empty()
                               ? "n/p: no per-offset divergence in this pair"
                               : derr;
            }
        }
    }

    // 22 T1: if any step/off move above changed the brushed entity, bump the
    // shared selection's epoch ONCE and stamp the active recording, so every
    // pane's projection notices the same brush moved (cross-highlight, not
    // cross-navigate — only the active pane scrolls).
    if (s.selection.step != sel_step0 || s.selection.off != sel_off0) {
        s.selection.rec = a->id;
        ++s.selection.epoch;
    }
}

// The Ctrl+G go-to modal (17-T1): type a step or an asmtrace-link offset and
// jump. It parses with the SAME dt_nav_parse the deep links use and navigates
// via dt_nav_go, so a typed target and a clicked link land identically (D4).
static void draw_goto_modal(ShellState &s) {
    // show_goto stays true for as long as the modal is up (a durable signal the
    // keymap test can assert); it is cleared only when the modal closes (Go /
    // Cancel below).
    if (s.show_goto)
        ImGui::OpenPopup("Go to");
    if (!ImGui::BeginPopupModal("Go to", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        s.show_goto = false;
        return;
    }
    ImGui::TextUnformatted(
        "step number, or an asmtrace-link (v=…&rec=…&step=…):");
    bool go = ImGui::InputText("##goto", s.goto_buf, sizeof s.goto_buf,
                               ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemDefaultFocus();
    go |= ImGui::Button("Go");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        s.show_goto =
            false; // clear the intent so it does not reopen next frame
        ImGui::CloseCurrentPopup();
    }
    if (go) {
        // A bare number is a step on the active recording; anything else is a
        // full link. Either way it lands through dt_nav_go.
        const Streams *a = shell_a(s);
        std::string text = s.goto_buf;
        dt_link link;
        std::string perr;
        bool ok = false;
        char *end = nullptr;
        unsigned long n = std::strtoul(text.c_str(), &end, 0);
        if (end != text.c_str() && end && *end == '\0' && a != nullptr) {
            link.view = s.view;
            link.rec = a->id;
            link.step = static_cast<uint32_t>(n);
            ok = true;
        } else {
            ok = dt_nav_parse(text, link, perr);
        }
        if (ok && dt_nav_go(s.nav, link)) {
            s.goto_buf[0] = '\0';
            s.show_goto =
                false; // jumped — close (and do not reopen next frame)
            ImGui::CloseCurrentPopup();
        } else {
            s.status = ok ? s.nav.last_error : ("go to: " + perr);
        }
    }
    ImGui::EndPopup();
}

// The NON-DOCKED shell (19 T1 step 2): the pre-existing single-window tab layout
// — one "asmtest" window pinned to the viewport, a "main" tab bar of Home / doors
// / one tab per open recording. This is exactly what shipped before the pane
// conversion; the null backend's default (docking OFF) draws it, so a run with no
// dockspace never regresses.
static void draw_windowed_shell(ShellState &s, const ImGuiViewport *vp) {
    // Pin the shell to the whole viewport. Without this the "asmtest" window is a
    // floating panel that ImGui auto-fits to a tiny default size on the first
    // frame (and, with IniFilename disabled in main.cpp, never remembers a
    // resize) — so the app reads as "starts very small" inside the OS frame.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags shell_flags =
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar;
    ImGui::Begin("asmtest", nullptr, shell_flags);

    // Persistent wayfinding chrome (21-spine-navigation.md T2, F9): the "where am
    // I" band lives in the OUTER window, above the tab strip, so it is visible
    // from every tab body. It builds ON doc-18's back/forward affordance (drawn
    // below by draw_breadcrumb), it does not duplicate it.
    draw_wayfinding_bar(s);
    ImGui::Separator();

    // The persistent task rail (20 T2): a fixed home/nav surface, NOT a closeable
    // `BeginTabItem` in the `main` strip. In the docked app it is the kPaneHome
    // pane; here (no dockspace) it is a left child pinned beside the recording tab
    // strip, so it is still drawn every frame and cannot be closed.
    ImGui::BeginChild("homerail", ImVec2(320.0f, 0.0f), true);
    draw_home_rail(s);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("mainarea");

    if (ImGui::BeginTabBar("main", ImGuiTabBarFlags_AutoSelectNewTabs)) {
        // The Author door: full app only, and the render-only build says why
        // rather than hiding the tab (D4's split has to be legible). Its close is
        // routed through shell_request_author_close so an unsaved run raises the
        // save/discard/cancel guard instead of vanishing (T3, F24); the title
        // carries the `*` marker while dirty.
        if (s.show_author) {
            bool author_keep = true;
            std::string atitle = std::string("Author") +
                                 (s.author.dirty ? " *" : "") + "###authortab";
            if (ImGui::BeginTabItem(atitle.c_str(), &author_keep)) {
                draw_author_door(s.author);
                ImGui::EndTabItem();
            }
            if (!author_keep)
                shell_request_author_close(s);
        }

        if (s.show_inspect && ImGui::BeginTabItem("Inspect", &s.show_inspect)) {
            draw_inspect_door(s.inspect);
            ImGui::EndTabItem();
        }

        handle_inspect_open_request(s);

        // The Learn door: bundled walkthroughs, in BOTH binaries (it reads
        // recordings and links no engine — D4).
        if (s.show_learn && ImGui::BeginTabItem("Learn", &s.show_learn)) {
            draw_learn_door(s.learn, [&s](const std::string &path, long step) {
                learn_open(s, path, step);
            });
            ImGui::EndTabItem();
        }

        // One tab per open recording (title = filename); ### keeps the id stable
        // across renames. A closed tab is collected and applied after the loop so
        // the index walk is never invalidated mid-iteration.
        int to_close = -1;
        for (size_t i = 0; i < s.ws.recordings.size(); ++i) {
            const Recording &r = s.ws.recordings[i];
            // A dirty (authored + unsaved) recording carries a trailing `*` (T3,
            // VS-Code style); the ###recN id keeps the tab stable across the
            // rename so it never reorders or duplicates.
            // 21 T2: disambiguate two same-basename recordings in the tab title,
            // sharing the breadcrumb's helper so the band and the tabs agree.
            std::string title = disambiguated_label(s.ws, i) +
                                (r.dirty ? " *" : "") + "###rec" +
                                std::to_string(i);
            bool keep_open = true;
            ImGuiTabItemFlags tf = 0;
            if (s.want_open_tab == static_cast<int>(i))
                tf |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem(title.c_str(), &keep_open, tf)) {
                s.active_tab = static_cast<int>(i);
                draw_recording_tab(s, r);
                ImGui::EndTabItem();
            }
            if (!keep_open)
                to_close = static_cast<int>(i);
        }
        // Consumed: the SetSelected flags above take effect this same frame (a
        // forced outer tab renders its inner tab strip in the same pass), so the
        // jump is complete and the flags must not persist into the next frame.
        s.want_open_tab = -1;
        s.want_loom = false;
        s.want_view
            .reset(); // the 1/2/3/4 view-switch intent is consumed here too
        s.want_view_id.reset(); // the 5 / "View in 3D" intent, likewise (34 T2)
        // want_layout_reset is left for the docked path to consume (both binaries
        // run the docked shell — main.cpp enables docking for the app AND the
        // viewer; only the null test backend takes this windowed path, where there
        // is no dockspace to rebuild, so the intent is inert and observable).
        if (to_close >= 0)
            shell_request_close(
                s, static_cast<size_t>(to_close)); // T3 dirty guard

        ImGui::EndTabBar();
    }
    ImGui::Separator();
    draw_breadcrumb(s); // T6 back/forward history affordance
    ImGui::EndChild();  // mainarea
    ImGui::End();
}

// The DOCKED shell (19 T1/T2/T3 — the keystone): the layout manager's five region
// panes are finally REAL windows the shell `Begin()`s, so the dockspace, presets,
// tear-out and Reset act on windows that exist. The old inner exclusive
// `BeginTabBar("views")` is gone; the panes ARE the view surface, and the
// timeline, the scrubber and the Observer's disassembly can be shown at once.
//
// pane -> region -> content mapping (kept HERE beside the Begin calls so it and
// layout.cpp's DockWindow targets cannot drift, mirroring layout.h:46):
//   kPaneHome      (left)          the doors + a selectable list of open recordings
//   kPaneRecording (center)        Summary / Canvas / Slice / Diff / 3D overview,
//                                  as ONE flat tab bar (no exclusive nesting)
//   kPaneLoom      (center, tab)   the Loom (its loom-detail bar 1 level below)
//   kPaneObserver  (right/bottom)  the Observer deck (its observer bar the only
//                                  remaining sub-level)
//   kPaneTimeline  (bottom-left)   the operand timeline
//   kPaneScrubber  (bottom-right)  the register scrubber
//   kPaneInspector (right)         ABI x-ray / Backends, one flat bar
//   kPaneConnect / kPaneProcesses / kPaneCapture   the Inspect workflow, as three
//                                  dockable panes (host / target picker / capture)
// Every pane opens ON DEMAND (its CONTEXT must be met), CLOSES via its own X, and
// re-opens from View ▸ Panels — so an irrelevant pane never clutters the workspace,
// yet nothing is hidden (the menu still names a not-yet-available pane, greyed, D7).
// A pane whose recording cannot fill it still shows its own placard when open (the
// scrubber over a no-regstate recording, etc.) — that finer fidelity is unchanged.

// --- dockable-pane management (open-on-demand + close + undock + menu) ------
struct PaneDef {
    const char *name;
    bool default_open;
    bool (*context_ok)(const ShellState &);
    const char *unavailable_hint; // why the menu greys it when context is unmet
};
static bool pctx_always(const ShellState &) { return true; }
static bool pctx_recording(const ShellState &s) {
    return shell_a(s) != nullptr;
}
// The Live-capture pane is a per-target control surface: it only means anything
// once a process is picked in the Processes list (selected_pid). Gating it on the
// selection — not just a live host — keeps Capture mode landing on the target
// picker alone; the capture controls appear the moment a row is selected. A host
// is still required (no host, nothing to capture with).
static bool pctx_capture(const ShellState &s) {
    return s.inspect.host_started && s.inspect.selected_pid > 0;
}
// The Save pane only means anything once there is a capture to write (R3): a
// growing recording, or a completed one this session.
static bool pctx_save(const ShellState &s) {
    return inspect_has_capture(s.inspect);
}
// The PT slice pane is available ONLY on an Intel PT host — the host must be able
// to start a live PT capture for there to ever be a hardware-recorded path to
// replay. A non-PT host (this AMD box, an arm64 host, the render-only viewer)
// never shows the tab; the View ▸ Panels entry greys with the library's reason.
// Independent of a host connection or a recording — it is a fact about the CPU.
static bool pctx_pt(const ShellState &) { return inspect_pt_host_available(); }
static const PaneDef kManagedPanes[] = {
    {kPaneHome, true, pctx_always, ""},
    {kPaneConnect, false, pctx_always, ""},
    {kPaneProcesses, false, pctx_always, ""},
    // doc 45 T4: no prior selection needed (pctx_always, like Connect/
    // Processes) — the form is fillable with no host up and no target picked;
    // "Launch & trace" connects and forks in one action (inspect_door.cpp).
    {kPaneLaunch, false, pctx_always, ""},
    {kPaneCapture, false, pctx_capture,
     "pick a process in the Processes pane first"},
    // The Log is a persistent application + session console: open from startup and
    // in every mode (mode_wants_pane), so it never newly-appears — and so never
    // steals the Processes focus — when Capture connects, and so application events
    // have somewhere to land from the first frame. Always available: draw_status
    // renders an idle "no host" session cleanly.
    {kPaneLog, true, pctx_always, ""},
    // Save appears only when there IS a capture to save (R3).
    {kPaneSave, false, pctx_save, "start a capture first — nothing to save yet"},
    // The PT slice appears only on an Intel PT host (pctx_pt). Default closed —
    // Capture mode opens it (mode_wants_pane), but the context gate keeps it
    // hidden off a PT host regardless. The hint is the specific gate, not a generic
    // "unavailable".
    {kPanePtSlice, false, pctx_pt,
     "the PT slice needs an Intel PT host — no PT silicon here to record a path"},
    {kPaneRecording, true, pctx_recording, "open a recording first"},
    {kPaneLoom, true, pctx_recording, "open a recording first"},
    {kPaneObserver, true, pctx_recording, "open a recording first"},
    {kPaneTimeline, true, pctx_recording, "open a recording first"},
    {kPaneScrubber, true, pctx_recording, "open a recording first"},
    // The Inspector stays always-available: it hosts BOTH the ABI x-ray (which
    // needs a recording, and is self-checking per-tab with its own placard) AND the
    // Backends decode-capability panel, which needs NO recording — gating the pane
    // on a recording would hide Backends at startup and show a false "open a
    // recording first" reason for it (D7). So R8's "only available tabs" is served
    // by the other panes' context; the Inspector is genuinely available.
    {kPaneInspector, true, pctx_always, ""},
};
static const PaneDef *pane_def(const char *name) {
    for (const PaneDef &p : kManagedPanes)
        if (std::strcmp(p.name, name) == 0)
            return &p;
    return nullptr;
}
static bool pane_is_open(const ShellState &s, const char *name) {
    auto it = s.pane_open.find(name);
    if (it != s.pane_open.end())
        return it->second;
    const PaneDef *d = pane_def(name);
    return d ? d->default_open : true;
}
static bool pane_shown(const ShellState &s, const char *name) {
    const PaneDef *d = pane_def(name);
    return pane_is_open(s, name) && (d == nullptr || d->context_ok(s));
}

// The panes each task mode leads with (R8/R9). Home is universal; Capture/Inspect
// lead with the live workflow (Processes / Live capture / Log, and Save which its
// own context gates to "when there is something to save"), NOT the replay reading
// panes; Connect stays closed — Capture auto-connects and only the failure path
// reveals it. Replay/Author lead with the reading panes (context still gates them
// on a recording). A pure predicate, so shell_apply_mode_panes is testable.
static bool mode_wants_pane(Mode m, const char *name) {
    if (std::strcmp(name, kPaneHome) == 0)
        return true;
    // The Log is a persistent console — open in every mode so it is already up
    // before Capture connects (it must not newly-appear and steal the Processes
    // focus) and so application events are logged from startup, not only during a
    // live capture.
    if (std::strcmp(name, kPaneLog) == 0)
        return true;
    switch (m) {
    case Mode::Capture:
    case Mode::Inspect:
        // The PT slice joins the capture workflow so a PT host reveals it
        // alongside the others; its context gate (pctx_pt) keeps it hidden off a
        // PT host even though this opens it.
        return std::strcmp(name, kPaneProcesses) == 0 ||
               std::strcmp(name, kPaneCapture) == 0 ||
               std::strcmp(name, kPaneSave) == 0 ||
               std::strcmp(name, kPanePtSlice) == 0;
    case Mode::Open:
        return std::strcmp(name, kPaneRecording) == 0 ||
               std::strcmp(name, kPaneLoom) == 0 ||
               std::strcmp(name, kPaneObserver) == 0 ||
               std::strcmp(name, kPaneTimeline) == 0 ||
               std::strcmp(name, kPaneScrubber) == 0 ||
               std::strcmp(name, kPaneInspector) == 0;
    case Mode::Learn:
        return std::strcmp(name, kPaneRecording) == 0 ||
               std::strcmp(name, kPaneLoom) == 0 ||
               std::strcmp(name, kPaneTimeline) == 0 ||
               std::strcmp(name, kPaneScrubber) == 0;
    case Mode::Author:
        return std::strcmp(name, kPaneRecording) == 0 ||
               std::strcmp(name, kPaneScrubber) == 0;
    case Mode::Launch:
        // doc 45 T4: Launch pane instead of Processes — no table detour — but
        // the same live-capture surroundings (Live capture / Save / PT slice)
        // once a launch resolves into a running session.
        return std::strcmp(name, kPaneLaunch) == 0 ||
               std::strcmp(name, kPaneCapture) == 0 ||
               std::strcmp(name, kPaneSave) == 0 ||
               std::strcmp(name, kPanePtSlice) == 0;
    }
    return false;
}

void shell_apply_mode_panes(ShellState &s, Mode m) {
    for (const PaneDef &p : kManagedPanes)
        s.pane_open[p.name] = mode_wants_pane(m, p.name);
    // Entering Capture/Inspect is a transition where an ongoing capture should
    // re-reveal exactly the viz panes it fills (shell_apply_live_panes runs next
    // frame): clear the one-shot guard so it re-applies rather than leaving the
    // reading panes this call just closed.
    if (m == Mode::Capture || m == Mode::Inspect || m == Mode::Launch)
        s.live_applied_ordinal = -1;
}

// The visualization panes a live capture in `m` fills (R10) — the panes to open
// when that capture starts, so only the tabs it actually produces show. Mirrors
// budget's mode_visualizations() at the pane granularity (kPaneRecording hosts the
// Slice / 3D / Canvas). The viz panes NOT returned are closed for that capture.
static const char *const kAllVizPanes[] = {kPaneRecording, kPaneLoom,
                                           kPaneObserver, kPaneTimeline,
                                           kPaneScrubber};
static std::vector<const char *> mode_viz_panes(LiveMode m) {
    switch (m) {
    case LiveMode::Auto:
        return {kPaneRecording, kPaneLoom, kPaneScrubber, kPaneTimeline,
                kPaneObserver};
    case LiveMode::Dataflow:
        return {kPaneRecording, kPaneLoom, kPaneScrubber, kPaneTimeline};
    case LiveMode::Trace:
        return {kPaneRecording, kPaneTimeline, kPaneObserver};
    case LiveMode::Log:
        return {kPaneObserver, kPaneTimeline};
    case LiveMode::Stream:
        return {kPaneRecording, kPaneTimeline};
    case LiveMode::Sample:
        return {kPaneObserver, kPaneRecording};
    case LiveMode::Tree:
    case LiveMode::Graph:
    case LiveMode::Procs:
    case LiveMode::Watch:
        return {kPaneObserver};
    }
    return {kPaneObserver};
}

// R10: when a live capture appears, open exactly the viz panes that capture fills
// and close the rest — ONCE per capture (live_panes_applied guards it), so a
// manual close after that holds (Q3: set on transitions, never per-frame). The
// running mode comes from the session's own `started` echo, falling back to the
// pending want. Pure model move; test_shell drives it.
void shell_apply_live_panes(ShellState &s) {
    if (s.live_tab < 0) {
        s.live_applied_ordinal = -1; // reset so the NEXT capture re-applies
        return;
    }
    // doc 45 T4: Launch lands in this SAME live-workflow posture (mode_preset,
    // ui/mode.h) — a launched session must reveal its viz panes exactly like
    // an attached one, or the Recording/Loom/Observer/Timeline/Scrubber tabs
    // never open for a capture that began with "Launch & trace".
    if (s.mode != Mode::Capture && s.mode != Mode::Inspect &&
        s.mode != Mode::Launch)
        return;
    // A per-capture identity that survives tab-index shifts (an unrelated tab
    // close decrements live_tab) and a mirrored-completed capture (live_tab stays
    // >= 0 between captures): the count of captures this session has produced —
    // completed recordings plus the one still growing. Each distinct capture bumps
    // it; completing a capture does not (a completed one just moves from the
    // growing slot into recordings), so the pass fires once per capture, no more.
    const LiveSession &sess = s.inspect.session;
    long ord = static_cast<long>(sess.recordings().size()) +
               (sess.growing() != nullptr ? 1 : 0);
    if (ord == s.live_applied_ordinal)
        return;
    LiveMode rm;
    if (!mode_from_name(sess.status().mode, &rm))
        rm = s.inspect.want;
    std::vector<const char *> want = mode_viz_panes(rm);
    for (const char *p : kAllVizPanes) {
        bool on = false;
        for (const char *w : want)
            if (std::strcmp(p, w) == 0)
                on = true;
        s.pane_open[p] = on;
    }
    s.live_applied_ordinal = ord;
}

// The Log pane (docs R2): the live session's current status (moved out of the
// capture pane) above a COLORED scrollback of everything the capture and the
// other tabs emit — every session transition and every nav refusal — so those
// tabs stay uncluttered. Lines are graded on the shared fidelity axis: Error red,
// Warning amber, Success green, Info neutral. Auto-scrolls to the newest line only
// while the reader is already at the bottom (so scrolling back to read holds).
static void draw_log_pane(ShellState &s) {
    // The current session state (the block that used to sit inside the capture
    // pane). It carries its own progress bar / Cancel, so it stays interactive.
    draw_session_status(s.inspect);
    ImGui::Separator();
    ImGui::TextDisabled("log (%zu)", s.log.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear"))
        s.log.clear();
    if (ImGui::BeginChild("logscroll", ImVec2(0, 0), true)) {
        for (const ShellState::LogLine &ln : s.log) {
            ImVec4 col = ln.kind == ToastKind::Error     ? dt_bad_col()
                         : ln.kind == ToastKind::Warning ? dt_maybe_col()
                         : ln.kind == ToastKind::Success ? dt_good_col()
                                                         : ImGui::GetStyleColorVec4(
                                                               ImGuiCol_Text);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextWrapped("%s", ln.text.c_str());
            ImGui::PopStyleColor();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

static void draw_docked_shell(ShellState &s, const ImGuiViewport *vp) {
    // The View menu (Reset + presets) rides a main menu bar; it reserves the top
    // strip, and DockSpaceOverViewport then fills the remaining work area. Both
    // act on the real panes below now — the menu that was inert before this brief.
    if (ImGui::BeginMainMenuBar()) {
        // File (20 T3): Open…, Open Recent ▸ (each reopening to its stored
        // position), and Reopen last workspace.
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open…", "Ctrl+O")) {
                s.open_dialog = true;
                s.open_error.clear();
            }
            if (ImGui::BeginMenu("Open Recent", !s.recents.empty())) {
                for (size_t i = 0; i < s.recents.size(); ++i) {
                    std::string item =
                        base_name(s.recents[i]) + "###mru" + std::to_string(i);
                    if (ImGui::MenuItem(item.c_str()))
                        rail_open_path(s, s.recents[i]);
                }
                ImGui::EndMenu();
            }
            // Reopen last workspace: main.cpp writes the store; here we re-open
            // whatever recents remember that is not already open, so a session
            // resumes without retyping paths.
            if (ImGui::MenuItem("Reopen recent workspace", nullptr, false,
                                !s.recents.empty())) {
                std::vector<std::string> want = s.recents;
                for (auto it = want.rbegin(); it != want.rend(); ++it) {
                    bool already = false;
                    for (const Recording &rec : s.ws.recordings)
                        if (rec.path == *it)
                            already = true;
                    if (!already)
                        rail_open_path(s, *it);
                }
            }
            // Settings (20 T5): moved under File from its old top-level slot, so
            // the menu bar's roots are File / View / Help only.
            ImGui::Separator();
            if (ImGui::MenuItem("Settings"))
                s.show_settings = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            // Reset now routes through the SAME want_layout_reset intent the
            // Ctrl+Shift+R keybinding raises (T2), so the two cannot diverge and
            // Reset works even when the menu bar is not shown.
            if (ImGui::MenuItem("Reset layout", "Ctrl+Shift+R"))
                s.want_layout_reset = true;
            ImGui::Separator();
            for (LayoutPreset p :
                 {LayoutPreset::ReplayInspect, LayoutPreset::Author,
                  LayoutPreset::LiveObserver})
                if (ImGui::MenuItem(layout_preset_name(p)))
                    layout_build(s.dockspace_id, vp->WorkSize, p);
            // Named perspectives (20 T4): the built-in presets seed the map; a
            // user perspective is a saved arrangement over the same panes.
            ImGui::Separator();
            if (ImGui::BeginMenu("Perspectives")) {
                for (const auto &kv : s.perspectives)
                    if (ImGui::MenuItem(kv.first.c_str()))
                        perspective_apply(kv.second, s.dockspace_id,
                                          vp->WorkSize);
                ImGui::Separator();
                ImGui::InputTextWithHint("##persp", "name…", s.persp_name,
                                         sizeof s.persp_name);
                ImGui::SameLine();
                if (ImGui::SmallButton("Save perspective") &&
                    s.persp_name[0] != '\0') {
                    s.perspectives[s.persp_name] = perspective_snapshot();
                    s.ws_dirty = true;
                    s.persp_name[0] = '\0';
                }
                ImGui::EndMenu();
            }
            // Panels: every dockable pane with a checkbox bound to its open state,
            // so a CLOSED pane is re-openable here. A pane whose CONTEXT is not yet
            // met (no recording open / no host connected) is greyed but still named
            // and reasoned — nothing is hidden, only gated on demand (D7).
            ImGui::Separator();
            if (ImGui::BeginMenu("Panels")) {
                for (const PaneDef &p : kManagedPanes) {
                    bool open = pane_is_open(s, p.name);
                    bool ctx = p.context_ok(s);
                    if (ImGui::MenuItem(p.name, nullptr, &open, ctx))
                        s.pane_open[p.name] = open;
                    if (!ctx && *p.unavailable_hint &&
                        ImGui::IsItemHovered(
                            ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", p.unavailable_hint);
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        // Help — its own top-level section: Keyboard bindings (the overlay, moved
        // here from the Home rail), This host (the capability panel, now in Home ▸
        // Details — this opens the Home pane and expands that section via
        // want_details), and About (what this app is).
        if (ImGui::BeginMenu("Help")) {
            // The bool* overload toggles show_help and shows a checkmark, so the
            // menu reflects whether the bindings overlay is currently up.
            ImGui::MenuItem("Keyboard bindings", nullptr, &s.show_help);
            if (ImGui::MenuItem("This host")) {
                s.want_details = true;
                s.pane_open[kPaneHome] = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("About"))
                s.show_about = true;
            ImGui::EndMenu();
        }
        // Persistent wayfinding chrome (21-spine-navigation.md T2, F9): the
        // "where am I" band rides the always-visible menu bar, outside every dock
        // pane, so it survives tab/pane switches. Read-only; the back/forward
        // buttons (doc 18) stay their own control below the reading pane.
        ImGui::Separator();
        draw_wayfinding_bar(s);
        ImGui::EndMainMenuBar();
    }

    s.dockspace_id = ImGui::DockSpaceOverViewport(
        0, vp, ImGuiDockNodeFlags_PassthruCentralNode);
    if (!s.layout_inited) {
        s.layout_inited = true;
        // First run: DockSpaceOverViewport just created an EMPTY leaf node, so a
        // plain `layout_exists` is always true here and would wrongly skip the
        // build (the latent bug that left the panes undocked while they were
        // phantom). Build the default split unless a real (persisted) layout is
        // already present.
        if (layout_needs_default(s.dockspace_id))
            layout_build(s.dockspace_id, vp->WorkSize,
                         LayoutPreset::ReplayInspect);
        // Seed the perspective map (20 T4): the three built-in presets are the
        // starting perspectives; a user "Save perspective" adds an ini snapshot.
        if (s.perspectives.empty())
            for (LayoutPreset p :
                 {LayoutPreset::ReplayInspect, LayoutPreset::Author,
                  LayoutPreset::LiveObserver})
                s.perspectives[layout_preset_name(p)] =
                    perspective_preset_value(layout_preset_name(p));
    }

    // T2 (F2): the always-available Reset intent — the keybinding or the menu —
    // consumed here, where the dockspace id is valid, so it fires with or without
    // the menu bar. Otherwise, once inited, run the zero-visible-pane auto-
    // fallback for the first couple of settled frames: a corrupt or collapsed
    // persisted `build/desktop-imgui.ini` can strand every pane, and rather than
    // showing an empty window we rebuild the shipped default. The short settle
    // window is what keeps it from fighting a user mid-drag (it only checks just
    // after init, not every frame).
    if (s.want_layout_reset) {
        s.want_layout_reset = false;
        s.layout_settle = 0;
        layout_reset(s.dockspace_id, vp->WorkSize);
    } else if (s.layout_settle < 2) {
        s.layout_settle++;
        if (s.layout_settle == 2 && layout_exists(s.dockspace_id) &&
            !layout_any_pane_visible(s.dockspace_id))
            layout_reset(s.dockspace_id, vp->WorkSize);
    }

    // Mode drives perspective (20 T2 step 5): selecting a task mode on the rail
    // sets `pending_preset`; apply it here where the dockspace id is valid, so
    // the label and the pane arrangement never disagree. Consumed once.
    if (s.pending_preset) {
        layout_build(s.dockspace_id, vp->WorkSize, *s.pending_preset);
        s.pending_preset.reset();
    }

    // A capture the Inspect door just saved may ask to open in the Loom; do it
    // before we resolve the active recording so this frame reflects it.
    handle_inspect_open_request(s);
    if (s.want_open_tab >= 0 &&
        s.want_open_tab < static_cast<int>(s.ws.recordings.size()))
        s.active_tab = s.want_open_tab; // the Home list is the docked selector

    const Recording *r =
        (s.active_tab >= 0 &&
         static_cast<size_t>(s.active_tab) < s.ws.recordings.size())
            ? &s.ws.recordings[static_cast<size_t>(s.active_tab)]
            : nullptr;
    const Streams *a = shell_a(s);
    const Streams *b = shell_b(s);
    int to_close = -1;

    // --- kPaneHome (left): the persistent task rail + the open-recording list ---
    if (pane_shown(s, kPaneHome)) {
        bool open = true;
        // Help ▸ This host raised want_details: bring Home forward so its Details
        // section (which draw_home_rail expands this same frame) is visible.
        if (s.want_details)
            ImGui::SetNextWindowFocus();
        if (ImGui::Begin(kPaneHome, &open)) {
            draw_home_rail(
                s); // 20 T2: the task-language rail replaces the doors
            ImGui::Separator();
            ImGui::TextUnformatted("open recordings:");
            if (s.ws.recordings.empty())
                ImGui::TextDisabled("(none yet — pick a task above)");
            for (size_t i = 0; i < s.ws.recordings.size(); ++i) {
                // A descriptive label (R6): name + backend/fidelity/size, live tag
                // when it is the capture. A dirty (authored + unsaved) recording
                // carries a trailing `*`; the ###home id keeps selection stable.
                std::string label = descriptive_recording_label(s, i) +
                                    (s.ws.recordings[i].dirty ? " *" : "") +
                                    "###home" + std::to_string(i);
                if (ImGui::Selectable(label.c_str(),
                                      s.active_tab == static_cast<int>(i)))
                    s.active_tab = static_cast<int>(i);
                ImGui::SameLine();
                ImGui::PushID(static_cast<int>(i));
                // The `x` closes this recording (R7) — the dirty-close guard still
                // stands in front of an unsaved authored one.
                if (ImGui::SmallButton("x"))
                    to_close = static_cast<int>(i);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("close this recording");
                ImGui::PopID();
            }
        }
        ImGui::End();
        if (!open)
            s.pane_open[kPaneHome] = false;
    }

    // The door screens open as their own dockable windows in the docked layout
    // (the door-chooser redesign, F13, is doc 21 — not here).
    if (s.show_author) {
        bool author_keep = true;
        std::string atitle = std::string("Author") +
                             (s.author.dirty ? " *" : "") + "###authorwin";
        if (ImGui::Begin(atitle.c_str(), &author_keep))
            draw_author_door(s.author);
        ImGui::End();
        if (!author_keep)
            shell_request_author_close(s); // T3 dirty guard
    }
    // The Inspect workflow as three dockable panes (Connect / Processes / Live
    // capture) instead of one door window. Poll the serve session ONCE for all
    // three; each pane opens on demand, closes (X), undocks, and is in View ▸
    // Panels. `show_inspect` stays the windowed shell's tab flag.
    s.inspect.session.poll();
    // Cross-pane reveal requests from the Processes pane's row actions (a double-
    // click / right-click with no host, or "Open Connect pane"): the door raises a
    // flag; the docked shell reveals the pane so the target's confirm/status is
    // visible even if the user had closed it.
    if (s.inspect.want_open_connect) {
        s.inspect.want_open_connect = false;
        s.show_inspect = true;
        s.pane_open[kPaneConnect] = true;
    }
    if (s.inspect.want_open_capture) {
        s.inspect.want_open_capture = false;
        s.show_inspect = true;
        s.pane_open[kPaneCapture] = true;
        // Reveal the Log beside it, so a right-click "Trace a function…" (which
        // bypasses shell_select_mode) still gets the session feed alongside.
        s.pane_open[kPaneLog] = true;
    }
    if (pane_shown(s, kPaneConnect)) {
        bool open = true;
        if (ImGui::Begin(kPaneConnect, &open))
            draw_connect_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneConnect] = false;
    }
    if (pane_shown(s, kPaneProcesses)) {
        bool open = true;
        // Consumed once (Capture-mode entry): raise Processes to its dock node's
        // active tab so the target picker is what the user lands on. HOLD the
        // request until the mode's pending_preset has been applied — layout_build
        // re-docks every pane the frame after the mode switch and resets each
        // node's selected tab, so focusing before that rebuild would be clobbered.
        if (s.inspect.want_focus_processes && !s.pending_preset) {
            s.inspect.want_focus_processes = false;
            ImGui::SetNextWindowFocus();
        }
        if (ImGui::Begin(kPaneProcesses, &open))
            draw_processes_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneProcesses] = false;
    }
    // doc 45 T4: the Launch pane — a fourth way to arrive at a target,
    // alongside Connect/Processes/Live-capture. Same want_focus_* idiom as
    // Processes above, guarded the same way against layout_build's rebuild.
    if (pane_shown(s, kPaneLaunch)) {
        bool open = true;
        if (s.inspect.want_focus_launch && !s.pending_preset) {
            s.inspect.want_focus_launch = false;
            ImGui::SetNextWindowFocus();
        }
        if (ImGui::Begin(kPaneLaunch, &open))
            draw_launch_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneLaunch] = false;
    }
    if (pane_shown(s, kPaneCapture)) {
        bool open = true;
        if (ImGui::Begin(kPaneCapture, &open))
            draw_capture_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneCapture] = false;
    }
    // The Log pane (R2): the colored session/status scrollback, split out of the
    // capture pane so that tab stays just controls.
    if (pane_shown(s, kPaneLog)) {
        bool open = true;
        // NoFocusOnAppearing: the persistent Log must never seize focus the frame
        // it first becomes visible — it would pull the user off whatever pane they
        // are working in (the Processes picker on Capture-mode entry).
        if (ImGui::Begin(kPaneLog, &open, ImGuiWindowFlags_NoFocusOnAppearing))
            draw_log_pane(s);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneLog] = false;
    }
    // The Save pane (R3): save-to-.asmtrace, shown only when there is a capture.
    if (pane_shown(s, kPaneSave)) {
        bool open = true;
        if (ImGui::Begin(kPaneSave, &open))
            draw_save_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPaneSave] = false;
    }
    // The PT slice pane: the def-use slice with zero single-steps of the target,
    // shown only on an Intel PT host (pctx_pt). Its own tab now, out of the
    // Live-capture controls.
    if (pane_shown(s, kPanePtSlice)) {
        bool open = true;
        if (ImGui::Begin(kPanePtSlice, &open))
            draw_pt_slice_pane(s.inspect);
        ImGui::End();
        if (!open)
            s.pane_open[kPanePtSlice] = false;
    }
    if (s.show_learn) {
        if (ImGui::Begin("Learn", &s.show_learn))
            draw_learn_door(s.learn, [&s](const std::string &path, long step) {
                learn_open(s, path, step);
            });
        ImGui::End();
    }

    // --- kPaneRecording (center): the light reading views, ONE flat tab bar ---
    bool rec_show = pane_shown(s, kPaneRecording);
    bool rec_open = true;
    if (rec_show && ImGui::Begin(kPaneRecording, &rec_open)) {
        if (r == nullptr) {
            ImGui::TextDisabled("open a recording (a door in Home, or the "
                                "list) to read it here");
        } else if (ImGui::BeginTabBar("recording-views")) {
            // Data-driven (20 T1): this centre pane hosts the light reading views;
            // the timeline, scrubber, Loom, Observer and ABI x-ray are their OWN
            // docked panes (each with its own placard). Gate the hosted subset on
            // presence and collapse its absences into one affordance.
            auto hosts = [](ViewId id) {
                return id == ViewId::Summary || id == ViewId::Canvas ||
                       id == ViewId::Slice || id == ViewId::Diff ||
                       id == ViewId::Scene3D;
            };
            if (a == nullptr) {
                if (ImGui::BeginTabItem("Summary")) {
                    draw_summary(*r);
                    ImGui::EndTabItem();
                }
            } else {
                std::vector<ViewPresence> vp = shell_view_presence(s, *r, a);
                std::optional<dt_view> absent_want;
                if (s.want_view)
                    for (const ViewPresence &e : vp)
                        if (hosts(e.id) && !e.present && e.view &&
                            *e.view == *s.want_view)
                            absent_want = *s.want_view;
                // 34 T2: a `5` / "View in 3D" request for an absent hosted ViewId
                // routes to the affordance here too (the 3D overview lives in this
                // pane, so its absence is explained in this pane's affordance).
                bool absent_want_id = false;
                if (s.want_view_id)
                    for (const ViewPresence &e : vp)
                        if (hosts(e.id) && !e.present &&
                            e.id == *s.want_view_id)
                            absent_want_id = true;
                size_t nabs = 0;
                for (const ViewPresence &e : vp) {
                    if (!hosts(e.id))
                        continue;
                    if (!e.present) {
                        nabs++;
                        continue;
                    }
                    ImGuiTabItemFlags fl = (e.view && s.want_view == e.view)
                                               ? ImGuiTabItemFlags_SetSelected
                                               : 0;
                    if (s.want_view_id && e.id == *s.want_view_id)
                        fl |= ImGuiTabItemFlags_SetSelected; // 34 T2 (Scene3D)
                    if (ImGui::BeginTabItem(e.label, nullptr, fl)) {
                        if (e.view)
                            s.view = *e.view;
                        draw_view_body(s, e.id, *r, a, b);
                        ImGui::EndTabItem();
                    }
                }
                if (nabs > 0) {
                    std::string label = "unavailable views (" +
                                        std::to_string(nabs) + ")###unavail_c";
                    ImGuiTabItemFlags fl = (absent_want || absent_want_id)
                                               ? ImGuiTabItemFlags_SetSelected
                                               : 0;
                    if (ImGui::BeginTabItem(label.c_str(), nullptr, fl)) {
                        for (const ViewPresence &e : vp)
                            if (hosts(e.id) && !e.present) {
                                ImGui::BulletText("%s", e.label);
                                ImGui::Indent();
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                                      dt_warn_col());
                                ImGui::TextWrapped("%s", e.reason.c_str());
                                ImGui::PopStyleColor();
                                ImGui::Unindent();
                            }
                        ImGui::TextDisabled(
                            "(the timeline, scrubber, Loom, Observer and ABI "
                            "x-ray are their own panes — each shows its own "
                            "placard when empty)");
                        ImGui::EndTabItem();
                    }
                }
            }
            ImGui::EndTabBar();
        }
    }
    if (rec_show) {
        ImGui::End();
        if (!rec_open)
            s.pane_open[kPaneRecording] = false;
    }

    // --- kPaneLoom (center, co-docked as a tear-able tab) ---
    bool loom_show = pane_shown(s, kPaneLoom);
    bool loom_open = true;
    if (loom_show && ImGui::Begin(kPaneLoom, &loom_open)) {
        if (a != nullptr) {
            shell_live_weave_banner(
                s); // 25 T5: perturb+torn caveat on a live weave
            shell_df_pass_pager(
                s); // 40 T2: pick which invocation pass this weave shows
            ReweaveSource rw = shell_reweave_source(s);
            draw_loom(
                s.loom, *a, s.ws, s.active_tab,
                [&s](const dt_link &l) {
                    if (!dt_nav_go(s.nav, l))
                        s.status = s.nav.last_error;
                },
                &s.selection, &s.undo, &rw);
        } else
            ImGui::TextDisabled("open a recording to weave its Loom");
    }
    if (loom_show) {
        ImGui::End();
        if (!loom_open)
            s.pane_open[kPaneLoom] = false;
    }

    // --- kPaneObserver: the live/observer deck (its observer bar is the sub-level)
    bool obs_show = pane_shown(s, kPaneObserver);
    bool obs_open = true;
    if (obs_show && ImGui::Begin(kPaneObserver, &obs_open)) {
        if (r != nullptr && a != nullptr)
            body_observer(s, *r, a);
        else
            ImGui::TextDisabled(
                "open a recording to see its live/observer views");
    }
    if (obs_show) {
        ImGui::End();
        if (!obs_open)
            s.pane_open[kPaneObserver] = false;
    }

    // --- kPaneTimeline (bottom-left): the operand timeline ---
    // A spatial pane: while it (or the 3D view inside the recording pane) holds
    // focus, W/S/A/D drive the camera and `d` does NOT toggle the diff — the
    // labelled context switch (T1). Recorded as an explicit ShellState field the
    // keymap reads next frame, not an implicit focus guess.
    bool spatial_focus = false;
    bool tl_show = pane_shown(s, kPaneTimeline);
    bool tl_open = true;
    if (tl_show && ImGui::Begin(kPaneTimeline, &tl_open)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            spatial_focus = true;
        if (a != nullptr)
            body_timeline(s, a, b);
        else
            ImGui::TextDisabled("open a recording to see its operand timeline");
    }
    if (tl_show) {
        ImGui::End();
        if (!tl_open)
            s.pane_open[kPaneTimeline] = false;
    }
    s.wasd_context = spatial_focus;

    // --- kPaneScrubber (bottom-right): the register scrubber ---
    bool sc_show = pane_shown(s, kPaneScrubber);
    bool sc_open = true;
    if (sc_show && ImGui::Begin(kPaneScrubber, &sc_open)) {
        if (a != nullptr)
            body_scrubber(s);
        else
            ImGui::TextDisabled(
                "open a recording to time-travel its registers");
    }
    if (sc_show) {
        ImGui::End();
        if (!sc_open)
            s.pane_open[kPaneScrubber] = false;
    }

    // --- kPaneInspector (right): ABI x-ray / Backends, one flat bar
    bool insp_show = pane_shown(s, kPaneInspector);
    bool insp_open = true;
    if (insp_show && ImGui::Begin(kPaneInspector, &insp_open)) {
        if (ImGui::BeginTabBar("inspector")) {
            if (ImGui::BeginTabItem("ABI x-ray")) {
                if (a != nullptr)
                    body_abixray(s, a, b);
                else
                    ImGui::TextDisabled(
                        "open a recording (and attach a Win64 leg, "
                        "press d) for the ABI x-ray");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Backends")) {
                draw_completeness(s.completeness, s.repo_root);
                ImGui::EndTabItem();
            }
            // "This host" moved to Home ▸ Details (Help ▸ This host jumps there).
            ImGui::EndTabBar();
        }
    }
    if (insp_show) {
        ImGui::End();
        if (!insp_open)
            s.pane_open[kPaneInspector] = false;
    }

    // The back/forward affordance + a nav refusal append below the reading pane —
    // a second Begin on the SAME kPaneRecording window (ImGui appends), so it rides
    // the recording pane's visibility rather than being its own.
    if (rec_show && ImGui::Begin(kPaneRecording)) {
        ImGui::Separator();
        draw_breadcrumb(s); // T6 back/forward history affordance
        if (!s.status.empty())
            ImGui::TextWrapped("%s", s.status.c_str());
    }
    if (rec_show)
        ImGui::End();

    // Port the keymap view-switch intents to FOCUS (19 T1 step 4): the tabbed
    // views were already SetSelected above; here bring their pane forward, and
    // focus the standalone panes the keymap names.
    if (s.want_view) {
        switch (*s.want_view) {
        case dt_view::timeline:
            ImGui::SetWindowFocus(kPaneTimeline);
            break;
        case dt_view::canvas:
        case dt_view::slice:
        case dt_view::diff:
            ImGui::SetWindowFocus(kPaneRecording);
            break;
        default:
            break;
        }
    }
    if (s.want_loom)
        ImGui::SetWindowFocus(kPaneLoom);
    s.want_open_tab = -1;
    s.want_loom = false;
    s.want_view.reset();
    s.want_view_id.reset(); // the 5 / "View in 3D" intent, likewise (34 T2)
    if (to_close >= 0)
        shell_request_close(s, static_cast<size_t>(to_close)); // T3 dirty guard
}

// The minimal back/forward affordance over the router history (18-breach-stops.md
// T6, F11): two buttons that walk dt_nav_back/forward, plus a compact current-
// position label. The persistent wayfinding chrome proper is doc 21; this is the
// emergency-exit-back gesture only. Called inside an existing window.
static void draw_breadcrumb(ShellState &s) {
    ImGui::BeginDisabled(s.nav.back.empty());
    if (ImGui::SmallButton("< back") && !dt_nav_back(s.nav))
        s.status = s.nav.last_error;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !s.nav.back.empty())
        ImGui::SetTooltip("Alt+Left — back to %s",
                          dt_nav_format(s.nav.back.back()).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(s.nav.forward.empty());
    if (ImGui::SmallButton("forward >") && !dt_nav_forward(s.nav))
        s.status = s.nav.last_error;
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && !s.nav.forward.empty())
        ImGui::SetTooltip("Alt+Right — forward to %s",
                          dt_nav_format(s.nav.forward.back()).c_str());
    // The current position is no longer echoed here: the persistent wayfinding
    // band (21-spine-navigation.md T2) now owns the rich "where am I" breadcrumb,
    // so this stays the minimal back/forward affordance (doc 18) it always was —
    // built on, not duplicated.
}

// Keep the Inspect door's least-perturbing default fed from the capability probe
// (T5): the picker defaults to `sample` where AMD IBS is available, else the
// lightest ptrace mode. Under the null test backend caps.rows is empty, so this
// leaves sample_available false and Log is the default.
static void shell_sync_inspect_defaults(ShellState &s) {
    bool ibs = false;
    for (const cap_row &r : s.caps.rows)
        if (r.kind == cap_kind::ibs && r.available)
            ibs = true;
    s.inspect.sample_available = ibs;
}

// The dirty-close guards (18-breach-stops.md T3, F24): a workspace recording with
// unsaved authored output, or the Author door tab, cannot be closed on one click
// — a save/discard/cancel modal stands in front of the erase.
static void draw_close_guards(ShellState &s) {
    if (s.close_pending >= 0)
        ImGui::OpenPopup("Unsaved recording");
    if (ImGui::BeginPopupModal("Unsaved recording", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "This recording has unsaved authored output. Closing "
            "it now would lose the recording.");
        if (ImGui::Button("Save") && s.close_pending >= 0 &&
            s.close_pending < static_cast<int>(s.ws.recordings.size())) {
            Recording &rec =
                s.ws.recordings[static_cast<size_t>(s.close_pending)];
            std::string path =
                rec.path.empty() ? "authored.asmtrace" : rec.path;
            std::string err;
            if (save_recording_file(rec, path, err)) {
                rec.dirty = false;
                shell_discard_close(s); // now clean; discard closes it
            } else {
                s.status = err;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) {
            shell_discard_close(s);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            shell_cancel_close(s);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (s.author_close_guard)
        ImGui::OpenPopup("Unsaved authored run");
    if (ImGui::BeginPopupModal("Unsaved authored run", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "The Author tab has an unsaved run. Save it from the "
            "tab, or discard it?");
        if (ImGui::Button("Discard and close")) {
            s.show_author = false;
            s.author.dirty = false;
            s.author.saved_ok = false;
            s.author_close_guard = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep editing")) {
            s.author_close_guard = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// The Settings pane (20 T5, F6): user text-scale, light theme, remembered window
// size — and a FAITHFUL statement of the a11y scope (ImGui exposes no OS
// screen-reader tree). It also hosts the named filter presets (T4). The pure
// Settings struct is what the tests assert; this only wires the widgets to it.
static void draw_settings(ShellState &s) {
    if (!s.show_settings)
        return;
    if (ImGui::Begin("Settings", &s.show_settings)) {
        ImGui::TextUnformatted("Display");
        ImGui::Separator();
        float ts = s.settings.text_scale;
        if (ImGui::SliderFloat("Text scale", &ts, Settings::kTextScaleMin,
                               Settings::kTextScaleMax, "%.2fx")) {
            s.settings.text_scale = settings_clamp_text_scale(ts);
            s.settings_dirty = true;
        }
        ImGui::TextDisabled(
            "Live via FontGlobalScale; the atlas re-bakes crisp "
            "at the scaled px on a content-scale change.");
        if (ImGui::Checkbox("Light theme", &s.settings.light_theme)) {
            dt_set_light_theme(s.settings.light_theme);
            if (s.settings.light_theme)
                ImGui::StyleColorsLight();
            else
                ImGui::StyleColorsDark();
            s.settings_dirty = true;
        }
        ImGui::Text("Window: %d x %d (remembered on exit)", s.settings.win_w,
                    s.settings.win_h);
        ImGui::Text("Content (DPI) scale: %.2fx",
                    static_cast<double>(s.settings.content_scale));

        // Connection (live capture): the serve-host config the "Capture a live
        // process" flow spawns. Moved here from the Connect pane so it persists;
        // the pane stays as an on-the-fly fallback. Both editors write the same
        // InspectState buffers, so an edit here takes effect on the next connect;
        // mirroring into Settings + settings_dirty is what makes it survive a
        // restart. The Connect pane's own edits stay session-only (it is the
        // fallback), which is why only this section marks the store dirty.
        ImGui::Separator();
        ImGui::TextUnformatted("Connection (live capture)");
        if (ImGui::InputText("asmspy path", s.inspect.asmspy_path,
                             sizeof s.inspect.asmspy_path)) {
            s.settings.asmspy_path = s.inspect.asmspy_path;
            s.settings_dirty = true;
        }
        if (ImGui::InputTextWithHint("ssh host", "blank = local",
                                     s.inspect.ssh_host,
                                     sizeof s.inspect.ssh_host)) {
            s.settings.ssh_host = s.inspect.ssh_host;
            s.settings_dirty = true;
        }
        ImGui::TextDisabled("Used by \"Capture a live process\". Blank path "
                            "resolves $PATH then "
                            "./build/asmspy; from another OS set an ssh host.");

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, dt_warn_col());
        ImGui::TextWrapped(
            "Accessibility: text-scale is the ONLY in-app a11y lever — Dear "
            "ImGui exposes no OS screen-reader tree, so a screen reader cannot "
            "read these panes. This is a recorded platform limit, not an "
            "omission.");
        ImGui::PopStyleColor();

        // Named filter presets (20 T4) over the active recording's syscall
        // filter. "Showing N of M" fidelity stays exactly as the filter renders
        // it — a preset only writes the query string, it never claims coverage.
        ImGui::Separator();
        ImGui::TextUnformatted("Saved filter presets");
        const bool have_active =
            s.active_tab >= 0 &&
            static_cast<size_t>(s.active_tab) < s.observers.size();
        for (const FilterPreset &p : s.presets) {
            ImGui::PushID(p.name.c_str());
            if (ImGui::SmallButton("apply") && have_active) {
                char *buf = s.observers[static_cast<size_t>(s.active_tab)]
                                .syscall_filter;
                filter_preset_apply(
                    p.query, buf,
                    sizeof s.observers[static_cast<size_t>(s.active_tab)]
                        .syscall_filter);
            }
            ImGui::SameLine();
            ImGui::Text("%s = \"%s\"", p.name.c_str(), p.query.c_str());
            ImGui::PopID();
        }
        ImGui::InputTextWithHint("##presetname", "preset name…", s.preset_name,
                                 sizeof s.preset_name);
        ImGui::SameLine();
        if (ImGui::SmallButton("Save syscall filter") &&
            s.preset_name[0] != '\0' && have_active) {
            FilterPreset p;
            p.name = s.preset_name;
            p.query =
                s.observers[static_cast<size_t>(s.active_tab)].syscall_filter;
            s.presets.push_back(std::move(p));
            s.ws_dirty = true;
            s.preset_name[0] = '\0';
        }
    }
    ImGui::End();
}

// Help ▸ About: a standalone "what is this app" window, toggled from the menu
// bar exactly as Settings / Keyboard bindings are. Deliberately spare and faithful
// — it names the app, what it is, and points at the two on-host answers (Details
// and Backends) rather than reciting a version it cannot truthfully print.
static void draw_about(ShellState &s) {
    if (!s.show_about)
        return;
    if (ImGui::Begin("About asmtest", &s.show_about,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("asmtest — desktop");
        ImGui::Separator();
        ImGui::TextWrapped(
            "A viewer and authoring front-end for the asmtest capture library: "
            "replay .asmtrace recordings, attach to a running process, and "
            "author assembly to see faults as data.");
        ImGui::Spacing();
        ImGui::Text("Recording schema: v%d (.asmtrace)", kAsmtraceMajor);
        ImGui::Spacing();
        ImGui::TextDisabled(
            "What this host can capture lives in Home \xE2\x96\xB8 Details "
            "(Help \xE2\x96\xB8 This host); what this build can decode is in "
            "the "
            "Backends panel.");
    }
    ImGui::End();
}

// --- 22 T4: apply an undo Command onto the shell ------------------------------
// Restores the before-value on undo (Ctrl+Z) and the after-value on redo
// (Ctrl+Y). Touches ONLY the reversible view-model fields (filter / cone /
// selection / take set) — never the Author editor's own text buffer, which owns a
// disjoint undo (doc 17 T2). Pure model move, so test_undo drives it headlessly.
void undo_apply(ShellState &s, const UndoCommand &c, bool redo) {
    switch (c.kind) {
    case UndoCommand::Kind::Cone:
        s.cone_active = redo ? c.cone_active_after : c.cone_active_before;
        s.cone_fwd = redo ? c.cone_fwd_after : c.cone_fwd_before;
        break;
    case UndoCommand::Kind::Filter: {
        const std::string &val = redo ? c.filter_after : c.filter_before;
        // Restore onto the recording the edit was MADE on (by id), not whatever
        // tab is active now — a tab switch must not redirect Ctrl+Z to a different
        // recording's filter. If that recording has since been closed, the move
        // has nowhere to land: skip it rather than clobber an unrelated tab.
        int idx = index_of_id(s, c.filter_rec);
        if (idx >= 0 && static_cast<size_t>(idx) < s.observers.size()) {
            auto &obs = s.observers[static_cast<size_t>(idx)];
            std::snprintf(obs.syscall_filter, sizeof obs.syscall_filter, "%s",
                          val.c_str());
            // The detector must not re-record this move — but only its baseline
            // for the ACTIVE tab is meaningful; touching it when we restored a
            // different (background) recording would corrupt the active baseline.
            if (idx == s.active_tab)
                s.undo_filter_seen = val;
        }
        break;
    }
    case UndoCommand::Kind::TakeSet:
        // Restore only onto the recording this edit was made on (by id), like
        // Filter above — LoomState is a single, non-per-recording state that
        // gets cleared outright on a tab switch (42), so an undo/redo from a
        // DIFFERENT recording has nowhere correct to land: skip it rather than
        // paint a stale recording's takes onto whatever is active now.
        if (c.take_rec == s.loom.source_id) {
            s.loom.takes = redo ? c.takes_after : c.takes_before;
            // take_views (42 T4) moves in lockstep with takes, never
            // independently, so the paint overlay stays index-aligned across
            // undo/redo.
            s.loom.take_views = redo ? c.take_views_after : c.take_views_before;
        }
        break;
    case UndoCommand::Kind::Selection:
        s.selection = redo ? c.sel_after : c.sel_before;
        ++s.selection.epoch;
        break;
    }
}

// Coalesce a settled syscall-filter edit into ONE reversible Command (22 T4). It
// records nothing while the field is still being typed (io.WantTextInput) and
// nothing on a recording switch (it re-baselines instead), so Ctrl+Z reverses a
// whole filter change rather than one keystroke at a time.
static void record_filter_undo(ShellState &s) {
    if (s.active_tab < 0 ||
        static_cast<size_t>(s.active_tab) >= s.observers.size()) {
        s.undo_filter_tab = -1;
        return;
    }
    const std::string cur =
        s.observers[static_cast<size_t>(s.active_tab)].syscall_filter;
    if (s.undo_filter_tab != s.active_tab) {
        s.undo_filter_tab =
            s.active_tab; // adopt this recording's filter baseline
        s.undo_filter_seen = cur;
        return;
    }
    if (ImGui::GetIO().WantTextInput || cur == s.undo_filter_seen)
        return;
    UndoCommand cmd;
    cmd.kind = UndoCommand::Kind::Filter;
    cmd.filter_rec = s.streams[static_cast<size_t>(s.active_tab)].id;
    cmd.filter_before = s.undo_filter_seen;
    cmd.filter_after = cur;
    s.undo.push(std::move(cmd));
    s.undo_filter_seen = cur;
}

// --- 22 T3: the global find bar (Ctrl+F) --------------------------------------
// Search-as-measurement (F17): highlight EVERY hit (the timeline paints them; find
// never hides a row — the fidelity distinction from a filter, D7), report the match
// COUNT and the aggregate COST, and cycle with Enter / Shift+Enter. Cycling drives
// dt_nav_go — the ONE spine — never the undo stack (T4's boundary).
static void draw_find_bar(ShellState &s) {
    if (!s.find.open)
        return;
    const Streams *a = shell_a(s);
    if (a == nullptr) {
        s.find.open = false; // nothing to search without an active recording
        return;
    }
    if (ImGui::Begin("Find", &s.find.open)) {
        if (s.find.focus_query) {
            ImGui::SetKeyboardFocusHere();
            s.find.focus_query = false;
        }
        ImGui::SetNextItemWidth(320.0f);
        const bool enter = ImGui::InputTextWithHint(
            "##findq",
            "find mnemonic / address / symbol (measures, never hides)",
            s.find.query, sizeof s.find.query,
            ImGuiInputTextFlags_EnterReturnsTrue);
        // Recompute on a query change only — cheap, but not per frame.
        if (s.find.last_query != std::string(s.find.query)) {
            const ObserverState *obs =
                (s.active_tab >= 0 &&
                 static_cast<size_t>(s.active_tab) < s.observers.size())
                    ? &s.observers[static_cast<size_t>(s.active_tab)]
                    : nullptr;
            find_run(s.find, *a, obs);
        }
        // The measurement, always shown: how many, and at what aggregate cost
        // (summed hot-edge samples + step counts) — a find that MEASURES.
        ImGui::SameLine();
        ImGui::TextDisabled("%zu match%s · cost %llu", s.find.hits.size(),
                            s.find.hits.size() == 1 ? "" : "es",
                            (unsigned long long)s.find.total_cost);
        const bool shift = ImGui::GetIO().KeyShift;
        const bool next = ImGui::Button("Next") || (enter && !shift);
        ImGui::SameLine();
        const bool prev = ImGui::Button("Prev") || (enter && shift);
        if ((next || prev) && !s.find.hits.empty()) {
            if (const FindHit *h = find_cycle(s.find, next))
                if (!dt_nav_go(s.nav, find_hit_link(*h, a->id)))
                    s.status = s.nav.last_error;
        }
        if (s.find.active >= 0 &&
            static_cast<size_t>(s.find.active) < s.find.hits.size()) {
            const FindHit &fh =
                s.find.hits[static_cast<size_t>(s.find.active)];
            ImGui::TextDisabled("match %d/%zu: %s", s.find.active + 1,
                                s.find.hits.size(), fh.label.c_str());
            // 48 T3: "show in 3D" reuses the find's own intent seam rather
            // than adding a second search — but only when the active hit's
            // `off` IS a true absolute address (an abs-basis recording,
            // where `off` is the address by construction, 04's own
            // identity). A rel-basis off needs the anchor first (36/37) and
            // is out of scope for this convenience wiring; the row says so
            // rather than silently misplacing the camera (D7: graded, never
            // hidden).
            if (fh.off.has_value()) {
                const bool have_scene =
                    s.active_tab >= 0 &&
                    static_cast<size_t>(s.active_tab) < s.scenes.size() &&
                    s.scenes[static_cast<size_t>(s.active_tab)].built;
                const bool abs_ok =
                    have_scene &&
                    s.scenes[static_cast<size_t>(s.active_tab)].traj.basis ==
                        "abs";
                ImGui::SameLine();
                if (abs_ok) {
                    if (ImGui::Button("show in 3D##find")) {
                        SceneView &sv =
                            s.scenes[static_cast<size_t>(s.active_tab)];
                        float u = 0.0f, v = 0.0f;
                        if (scene3d::scene_goto_addr(sv.terr.proj, *fh.off, &u,
                                                     &v)) {
                            sv.cam.frame(u, v, sv.cam.radius);
                            sv.hud.nav_note.clear();
                        } else {
                            sv.hud.nav_note =
                                "show in 3D: address not mapped by any "
                                "region in this recording";
                        }
                    }
                } else {
                    ImGui::TextDisabled(
                        "(show in 3D: %s)",
                        have_scene ? "not an abs-basis recording"
                                  : "open the 3D pane first");
                }
            }
        }
        // Minimap seam (doc 21 T3): when the timeline overview/minimap lands, paint
        // s.find.hits there as ticks — a clean seam, not a hard dependency, since
        // the timeline already highlights every hit (body_timeline).
    }
    ImGui::End();
}

void draw_shell(ShellState &s) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();

    // The advertised keyboard bindings, acted on first so a keypress and the
    // matching click land in the same frame (17-T1).
    handle_keymap(s);
    // 22 T2: recompute the 3D-camera focus fresh each frame — the 3D pane sets it
    // true below when its HUD/viewport holds focus, and it must fall back to false
    // when that pane is not even drawn (so arrows resume stepping the selection).
    // handle_keymap already consumed last frame's value above.
    s.cam_focus = false;
    // Keep the Inspect picker's least-perturbing default in step with the probe
    // (T5) before any door draws it.
    shell_sync_inspect_defaults(s);
    // Promote the live capture into the workspace so the docked panes render it
    // (25). Uses the session state last frame's Inspect-door poll() left, so the
    // live views trail the observer deck by at most one frame — imperceptible,
    // and it keeps poll() the door's single responsibility.
    shell_sync_live_tab(s);
    // R10: once a live capture appears, open exactly the viz panes it fills (and
    // close the rest) — once per capture, so only the tabs relevant to THAT capture
    // are up. A no-op outside Capture/Inspect mode and when no capture is live.
    shell_apply_live_panes(s);
    // Entering Capture mode auto-connects the serve host from the saved Settings
    // (asmspy path / ssh host) so the "Capture a live process" button lands on the
    // Processes pane already attached — no Connect detour. Done here, not in
    // shell_select_mode, so that pure seam stays free of the subprocess spawn (the
    // null-backend tests drive shell_select_mode without ever reaching this). A
    // failed connect reveals the Connect pane so its host_error is not swallowed.
    if (s.inspect.want_autoconnect) {
        s.inspect.want_autoconnect = false;
        if (!s.inspect.host_started) {
            inspect_connect(s.inspect);
            if (!s.inspect.host_started)
                s.inspect.want_open_connect = true;
        }
    }
    // 34 T2: the Live-capture "View in 3D overview" handoff — jump from the picked
    // process's growing capture straight to its 3D tab. Consumed centrally so both
    // the docked and windowed shells honour it (a pure model move, tested direct).
    shell_consume_scene_handoff(s);
    // 34 T3: advance the execution-step play/pause transport once per frame, over
    // the active recording's dataflow step space, brushing selection.step through
    // the ONE writer so the timeline / slice / Loom / Scrubber animate together.
    // Advanced here (not in body_scrubber) so playback continues regardless of
    // which pane is visible. The 3D overview's terrain-time transport is advanced
    // separately in draw_scene_overview over its own axis (a different clock).
    if (s.play.playing) {
        const Streams *pa = shell_a(s);
        if (pa && pa->df.nsteps > 0) {
            const uint64_t maxstep = pa->df.nsteps - 1;
            const uint64_t cur =
                s.selection.rec == pa->id ? s.selection.step.value_or(0) : 0;
            const uint64_t nxt =
                transport_tick(s.play, cur, maxstep, ImGui::GetIO().DeltaTime);
            if (nxt != cur || s.selection.rec != pa->id)
                s.selection.set(pa->id, static_cast<uint32_t>(nxt),
                                std::nullopt);
        } else {
            s.play.playing = false; // nothing to play in this recording
        }
    }
    // Keep the fidelity-chrome theme flag + the live text-scale in step with the
    // Settings model every frame (20 T5): cheap, and it means a restored setting
    // takes effect without a special apply path.
    dt_set_light_theme(s.settings.light_theme);
    settings_apply_text_scale(s.settings, ImGui::GetIO());

    // Docking (13-foundation-moves.md T2): when enabled (the real app), the shell
    // draws real dockable panes (19). The null test backend leaves it OFF by
    // default, so the single-window tab layout draws unchanged — flip it ON in a
    // test to exercise the panes (that is exactly what test_shell's docked case
    // does).
    const bool docking =
        (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0;
    if (docking)
        draw_docked_shell(s, vp);
    else
        draw_windowed_shell(s, vp);

    if (s.open_dialog)
        draw_open_dialog(s);
    if (s.show_help) {
        ImGui::Begin("Keyboard bindings", &s.show_help);
        draw_bindings_help();
        ImGui::End();
    }
    draw_goto_modal(s);   // Ctrl+G (17-T1)
    draw_palette(s);      // Ctrl+Shift+P / Ctrl+P command palette (21 T1)
    draw_close_guards(s); // T3 dirty-close save/discard/cancel
    draw_settings(s);     // 20 T5 the Settings pane (File ▸ Settings)
    draw_about(s);        // Help ▸ About
    draw_find_bar(s);     // 22 T3 the global find (Ctrl+F)
    // 22 T4: coalesce a settled syscall-filter edit into one reversible Command,
    // AFTER the shell drew (and the user may have edited) the filter this frame.
    record_filter_undo(s);

    // A nav refusal (or any status-bar line) is "additional information" from the
    // other tabs — mirror each CHANGE into the Log so the one scrollback holds it
    // all. Graded a Warning: the status bar carries refusals, not successes.
    if (!s.status.empty() && s.status != s.last_logged_status) {
        shell_log_push(s, s.status, ToastKind::Warning);
        shell_log_command_hint(s, s.status); // no-op unless the status names a gate
        s.last_logged_status = s.status;
    }

    // Live-session toasts (16 T1): raise one per TRANSITION, then remember this
    // frame's feedback state so the next comparison is against it (never
    // re-toast an unchanged state). Guarded on a current context so the
    // null-backend tests, which drive draw_shell directly, stay silent and
    // deterministic. The decision is the pure, tested live_session_toasts();
    // this only maps the neutral ToastKind onto ImGuiNotify, attaches the
    // Open-in-Loom button when a toast carries a path, and queues it.
    if (ImGui::GetCurrentContext()) {
        FeedbackInputs cur;
        cur.status = s.inspect.session.status();
        cur.saved_ok = s.inspect.saved_ok;
        cur.saved_statistical = s.inspect.saved_statistical;
        cur.saved_path = s.inspect.saved_path;
        cur.save_status = s.inspect.save_status;
        for (const SessionToast &t :
             live_session_toasts(s.prev_feedback, cur)) {
            // Every transition is a Log line first — a toast IS a log entry, so
            // the Log pane carries the full history the trimmed toasts no longer
            // pop. Then trim the pop-ups to ERRORS only (per the Log decision): the
            // rest lives in the Log, not as transient overlays that steal focus.
            shell_log_push(s, t.text, t.kind);
            if (t.kind != ToastKind::Error && t.open_path.empty())
                continue;
            ImGuiToastType ty =
                t.kind == ToastKind::Error     ? ImGuiToastType::Error
                : t.kind == ToastKind::Warning ? ImGuiToastType::Warning
                : t.kind == ToastKind::Success ? ImGuiToastType::Success
                                               : ImGuiToastType::Info;
            if (t.open_path.empty()) {
                ImGui::InsertNotification(
                    ImGuiToast(ty, 5000, "%s", t.text.c_str()));
            } else {
                // An exact save: offer "Open in Loom", which feeds the path back
                // through the SAME open_request door the panes use. `s` outlives
                // every queued toast (main owns it for the whole loop), so a
                // pointer capture is safe; the path is captured by value.
                InspectState *ins = &s.inspect;
                std::string path = t.open_path;
                ImGui::InsertNotification(ImGuiToast(
                    ty, 8000, "Open in Loom",
                    [ins, path]() { ins->open_request = path; }, "%s",
                    t.text.c_str()));
            }
        }
        // Echo the terminal fix for a newly-changed gate reason into the Log, so
        // a perf-gated skip or a refusal leaves its command in the scrollback
        // after the toast dismisses. Guarded on the reason CHANGING (as the toasts
        // are) so it logs once per transition, never per frame.
        if (cur.status.skip_reason != s.prev_feedback.status.skip_reason)
            shell_log_command_hint(s, cur.status.skip_reason);
        if (cur.status.last_err != s.prev_feedback.status.last_err)
            shell_log_command_hint(s, cur.status.last_err);
        s.prev_feedback = cur;
        // Draw the queued toasts last, so they float over every pane.
        ImGui::RenderNotifications();
    }
}

} // namespace asmdesk
