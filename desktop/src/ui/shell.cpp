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
#include "ImGuiNotify.hpp"
#include "ImGuiFileDialog.h" // pure-ImGui open dialog (14 T7)

#include <algorithm> // std::sort/std::swap for the keymap (17 T1)
#include <cstdio>    // std::snprintf — undo_apply's filter restore (22 T4)
#include <cstdlib>   // std::strtoul — the go-to modal's step parse (17 T1)

#include "analysis/diff.h"  // dt_diff_build — n/p divergence walk (17 T1)
#include "analysis/slice.h"
#include "live/inspect.h" // live_session_toasts (16 T1)
#include "scene3d/hud.h"
#include "scene3d/pick.h"
#include "space/projection.h"
#include "ui/legend.h"        // shared semantic legend (24 T1/T2)
#include "ui/palette.h"       // command palette over the spine (21 T1)
#include "ui/perspectives.h"  // named dock perspectives + filter presets (20 T4)
#include "ui/terms.h"         // domain-term-first headings + Terms pane (24 T3)
#include "ui/theme.h"         // dt_set_light_theme (20 T5); dt_maybe_col coarse-scrub degrade (23 T4)
#include "ui/view_presence.h" // data-driven view set + honest absence (20 T1)
#include "ui/wayfinding.h"    // persistent breadcrumb + disambiguation (21 T2)
#include "views/abixray.h"
#include "views/views_draw.h"

namespace asmdesk {

// Defined below draw_shell, used by both shells' status areas (18-T6 breadcrumb).
static void draw_breadcrumb(ShellState &s);

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
        s.scrubber_playhead[i] = 0;
        s.scenes[i] = SceneView{};
        s.live_built_events = ~static_cast<uint64_t>(0); // force the first build
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
    shell_wire_nav(s); // the decoded stream id is now live — point the router at it
}

// The dirty-close guard (18-breach-stops.md T3, F24): a clean recording closes
// on the spot; a DIRTY (authored + unsaved) one raises the save/discard/cancel
// choice instead of erasing, so authored output is never lost to one click.
void shell_request_close(ShellState &s, size_t idx) {
    if (idx >= s.ws.recordings.size())
        return;
    if (s.ws.recordings[idx].dirty)
        s.close_pending = static_cast<int>(idx); // raise the guard, do not erase
    else
        shell_close(s, idx);
}

void shell_discard_close(ShellState &s) {
    if (s.close_pending < 0 ||
        s.close_pending >= static_cast<int>(s.ws.recordings.size()))
        return;
    size_t idx = static_cast<size_t>(s.close_pending);
    s.close_pending = -1;
    shell_close(s, idx); // shell_close resets close_pending if it were still set
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

// --- 20 T2: select a task mode (the seam the rail CTAs + the tests drive) -----
void shell_select_mode(ShellState &s, Mode m) {
    s.mode = m;
    s.pending_preset = mode_preset(m); // the docked frame applies it (T2 step 5)
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
        s.show_inspect = true;
        break;
    case Mode::Author:
        s.show_author = true;
        break;
    }
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
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
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

    ImGui::Separator();
    if (ImGui::SmallButton("Settings"))
        s.show_settings = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Keyboard bindings"))
        s.show_help = !s.show_help;
}

// A recording's summary pane: provenance chrome + the honesty banner + per-kind
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
// honest top-down 2D-ish fallback. Routed through the SAME Camera methods the
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
            "(the second channel keeps sampled evidence honestly distinct). Use "
            "3D to FIND a place, then the flat 2D views to READ it.",
            [] { dt_semantic_legend(); }, sv.primer);
    }

    // Weave the pure, engine-free models once per recording (lazy — a recording
    // whose 3D tab is never opened pays nothing). The coarse rung's plane comes
    // from the recording's `codeimage` code regions (08-T7); a replay file with
    // none yields an empty plane, which the pane SAYS rather than drawing a void.
    if (!sv.built) {
        std::vector<space::Region> regs = space::regions_from_codeimage(r);
        sv.has_regions = !regs.empty();
        sv.terr =
            space::build_terrain(space::build_projection(std::move(regs)), r);
        sv.traj = space::build_trajectories(r);
        sv.conv = space::detect_convergences(sv.traj, sv.terr.proj);
        sv.hud.nsteps = sv.terr.nsteps;
        sv.hud.t = sv.terr.nsteps; // show the whole trace by default
        sv.built = true;
    }

    // The HUD (its own window): provenance chips, playhead, layer toggles, camera
    // presets, region legend. Pure ImGui — drawn even under the null backend. It
    // reports the user's intent back through sv.hud; we apply it here (04's rule:
    // the view keeps no state the model does not).
    sv.hud.nsteps = sv.terr.nsteps;
    scene3d::draw_scene_hud(sv.hud, sv.terr, sv.traj);
    if (sv.hud.req_reset_view)
        sv.cam.reset();
    if (sv.hud.req_top_down)
        sv.cam.top_down();
    sv.hud.req_reset_view = sv.hud.req_top_down = false;

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
            sv.scrub_pending = true;           // finish the full slice next frame
        } else {
            sv.slice = sv.terr.slice(sv.hud.t); // the full slice lands
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
            "no address-space regions in this recording — the 3D overview needs "
            "codeimage events (or a live maps snapshot) to place the plane. The "
            "provenance, trajectory and legend above still read.");
        // The keyboard viewport target still exists (22 T2), so a keyboard-only
        // analyst can Tab in and orbit the camera even with no plane to draw.
        sv.viewport_focus = scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }

    // The GL scene is drawn by the host threaded in from main.cpp. It is absent
    // under the null test backend (and any run with no GL context): the models +
    // HUD above are the whole pane there, with this placard in place of the
    // viewport. draw_shell links no GL — every GL touch is behind this pointer.
    if (s.scene_host == nullptr) {
        ImGui::TextDisabled(
            "3D viewport unavailable — no GL context in this build/run "
            "(headless test, or a viewer with no display). The scene's models, "
            "provenance and legend above are fully woven.");
        // The Tab-reachable focus target + keyboard camera exist here too (22 T2):
        // this is precisely the null-backend path, where moving the pure Camera
        // with no GL is what makes the keyboard camera headlessly testable.
        sv.viewport_focus = scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }
    if (!s.scene_host->ready()) {
        ImGui::TextDisabled("3D scene did not initialise: %s",
                            s.scene_host->error());
        sv.viewport_focus = scene_viewport_target(ImGui::GetContentRegionAvail());
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
    f.key = std::hash<std::string>{}(a.id);
    f.slice_t = sv.slice_t;
    f.cam = sv.cam;
    f.layers = sv.hud.layers;
    f.fbw = fbw;
    f.fbh = fbh;

    ImTextureID tex = s.scene_host->render(f);
    if (!tex) {
        ImGui::TextDisabled("3D scene produced no frame on this driver");
        sv.viewport_focus = scene_viewport_target(ImGui::GetContentRegionAvail());
        return;
    }
    // The viewport is ONE focusable hit-target (22 T2): the InvisibleButton is
    // both the Tab-reach focus target (a keyboard-only analyst can reach it) and
    // the mouse hit-target for orbit/dolly/pick; the GL texture is drawn over it.
    // GL renders bottom-left origin; flip V so the image reads upright in ImGui.
    ImVec2 vp_origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(
        "3d-viewport",
        ImVec2(static_cast<float>(fbw), static_cast<float>(fbh)));
    sv.viewport_focus = ImGui::IsItemFocused();
    const bool vp_hover = ImGui::IsItemHovered();
    ImGui::GetWindowDrawList()->AddImage(
        tex, vp_origin,
        ImVec2(vp_origin.x + static_cast<float>(fbw),
               vp_origin.y + static_cast<float>(fbh)),
        ImVec2(0, 1), ImVec2(1, 0));

    // Camera + pick, only while the pointer is over the viewport. A left-drag
    // orbits; the wheel dollies; a click that did NOT drag is a pick — read the
    // id under the cursor and drill OUT to the flat 2D view through 04's router
    // (3D to find, 2D to read).
    if (vp_hover) {
        ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            sv.nav_dragging = false;
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f)) {
            sv.nav_dragging = true;
            sv.cam.orbit(-io.MouseDelta.x * 0.008f, -io.MouseDelta.y * 0.008f);
        }
        if (io.MouseWheel != 0.0f)
            sv.cam.dolly(io.MouseWheel > 0.0f ? 1.0f / 1.1f : 1.1f);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !sv.nav_dragging) {
            ImVec2 origin = ImGui::GetItemRectMin();
            int px = static_cast<int>(io.MousePos.x - origin.x);
            int py = static_cast<int>(io.MousePos.y - origin.y);
            if (px >= 0 && py >= 0 && px < fbw && py < fbh) {
                uint32_t id = s.scene_host->pick(sv.cam, fbw, fbh, px, py);
                scene3d::Pick pk = scene3d::decode_pick(id, sv.terr.w);
                auto link = scene3d::resolve_pick(sv.terr, sv.traj, a.id, pk);
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
// ONE implementation and cannot drift. The honesty placards move WITH the body
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

static void body_timeline(ShellState &s, const Streams *a, const Streams *b) {
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
    draw_timeline_overview(t, nsteps, &s.tl_lo, &s.tl_hi, [&s, a](uint32_t step) {
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
    draw_slice_view(dt_slice_view_build(*a, s.selection.step));
    if (b != nullptr)
        // Never a fake merged graph: the two-recording slice needs the Wave-2
        // state-diff producer, and until it exists this says so.
        ImGui::TextDisabled("showing A only — slice diff lands with the "
                            "state-diff producer (Wave 2)");
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
    if (i < s.stepidx.size())
        s.scrubber_playhead[i] =
            draw_scrubber(s.stepidx[i], s.scrubber_playhead[i]);
}
static void body_abixray(ShellState &s, const Streams *a, const Streams *b) {
    // The ABI x-ray (09-T4): locks the active recording (the SysV leg) against
    // the attached B (the Win64 leg) — the Diff tab's A/B mechanism. The view's
    // own honesty banners handle an unaligned pair / an absent per-pane producer.
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
static std::vector<ViewPresence> shell_view_presence(const ShellState &s,
                                                     const Recording &r,
                                                     const Streams *a) {
    if (a == nullptr)
        return {};
    static const ObserverState kEmptyObs;
    static const StepIndex kEmptySi;
    size_t i = static_cast<size_t>(s.active_tab);
    const ObserverState &obs =
        i < s.observers.size() ? s.observers[i] : kEmptyObs;
    const StepIndex &si = i < s.stepidx.size() ? s.stepidx[i] : kEmptySi;
    bool is_live = s.live_tab >= 0 && s.active_tab == s.live_tab;
    return view_presence(*a, obs, si, r, s.mode, shell_b_attachable(s), is_live);
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
    ImGui::TextWrapped(
        "live weave — the target is being single-stepped (perturbing), and this "
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
        shell_live_weave_banner(s); // 25 T5: perturb+torn caveat on a live weave
        draw_loom(
            s.loom, *a, s.ws, s.active_tab,
            [&s](const dt_link &l) { if (!dt_nav_go(s.nav, l)) s.status = s.nav.last_error; },
            &s.selection, &s.undo);
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

// The ONE honest "unavailable views (N)" affordance body (T1 step 4, D7). It
// names every absent view and its verbatim machine reason — the truth "this
// recording cannot fill view X" stays on screen, graded below the present set
// rather than shown as N empty peer tabs. `focus` is the view a keymap request
// named while absent, so it is explained FIRST.
static void draw_unavailable_views(const std::vector<ViewPresence> &vp,
                                   std::optional<dt_view> focus) {
    ImGui::TextWrapped(
        "These views cannot be filled by this recording in this mode. Each "
        "reason below is the machine's, verbatim — the view is graded here, not "
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
                ImGuiTabItemFlags fl =
                    absent_want ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(label.c_str(), nullptr, fl)) {
                    draw_unavailable_views(vp, absent_want);
                    ImGui::EndTabItem();
                }
            }
        }
        // App panels, not recording views: always available.
        if (ImGui::BeginTabItem("Backends")) {
            draw_completeness(s.completeness, s.repo_root);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("This host")) {
            draw_capability_panel(s.caps, &r);
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
    if (fd->Display("dlg_open", ImGuiWindowFlags_NoCollapse, ImVec2(520, 360))) {
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

    // 1/2/3/4 — switch the active recording's view. A keypress cannot select an
    // ImGui tab directly, so record the intent; the view tab bar honours it with
    // SetSelected next pass (want_view, mirroring want_loom).
    if (ImGui::IsKeyPressed(ImGuiKey_1)) s.want_view = dt_view::canvas;
    if (ImGui::IsKeyPressed(ImGuiKey_2)) s.want_view = dt_view::timeline;
    if (ImGui::IsKeyPressed(ImGuiKey_3)) s.want_view = dt_view::slice;
    if (ImGui::IsKeyPressed(ImGuiKey_4)) s.want_view = dt_view::diff;

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
        if (ImGui::IsKeyPressed(ImGuiKey_W)) s.wasd_zoom++;
        if (ImGui::IsKeyPressed(ImGuiKey_S)) s.wasd_zoom--;
        if (ImGui::IsKeyPressed(ImGuiKey_A)) s.wasd_pan--;
        if (ImGui::IsKeyPressed(ImGuiKey_D)) s.wasd_pan++;
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
        if (ImGui::IsKeyPressed(ImGuiKey_B)) { s.cone_active = true; s.cone_fwd = false; }
        if (ImGui::IsKeyPressed(ImGuiKey_F) && !io.KeyShift && !io.KeyCtrl) { s.cone_active = true; s.cone_fwd = true; }
        if (ImGui::IsKeyPressed(ImGuiKey_C) && !io.KeyCtrl) s.cone_active = false;
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
                if (i != s.active_tab) { s.b_index = i; break; }
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
                        if (o > curoff) { tgt = o; break; }
                } else {
                    for (auto it = offs.rbegin(); it != offs.rend(); ++it)
                        if (*it < curoff) { tgt = *it; break; }
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
    ImGui::TextUnformatted("step number, or an asmtrace-link (v=…&rec=…&step=…):");
    bool go = ImGui::InputText("##goto", s.goto_buf, sizeof s.goto_buf,
                               ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SetItemDefaultFocus();
    go |= ImGui::Button("Go");
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        s.show_goto = false; // clear the intent so it does not reopen next frame
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
            s.show_goto = false; // jumped — close (and do not reopen next frame)
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
        s.want_view.reset(); // the 1/2/3/4 view-switch intent is consumed here too
        // want_layout_reset is left for the docked path to consume (both binaries
        // run the docked shell — main.cpp enables docking for the app AND the
        // viewer; only the null test backend takes this windowed path, where there
        // is no dockspace to rebuild, so the intent is inert and observable).
        if (to_close >= 0)
            shell_request_close(s, static_cast<size_t>(to_close)); // T3 dirty guard

        ImGui::EndTabBar();
    }
    ImGui::Separator();
    draw_breadcrumb(s); // T6 back/forward history affordance
    ImGui::EndChild();   // mainarea
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
//   kPaneInspector (right)         ABI x-ray / Backends / This host, one flat bar
// With no active recording a pane shows its own placard rather than vanishing —
// data-driven pane HIDING is doc 20, so nothing is silently dropped here (D7).
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
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            // Reset now routes through the SAME want_layout_reset intent the
            // Ctrl+Shift+R keybinding raises (T2), so the two cannot diverge and
            // Reset works even when the menu bar is not shown.
            if (ImGui::MenuItem("Reset layout", "Ctrl+Shift+R"))
                s.want_layout_reset = true;
            ImGui::Separator();
            for (LayoutPreset p : {LayoutPreset::ReplayInspect,
                                   LayoutPreset::Author,
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
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Settings"))
            s.show_settings = true;
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
            layout_build(s.dockspace_id, vp->WorkSize, LayoutPreset::ReplayInspect);
        // Seed the perspective map (20 T4): the three built-in presets are the
        // starting perspectives; a user "Save perspective" adds an ini snapshot.
        if (s.perspectives.empty())
            for (LayoutPreset p : {LayoutPreset::ReplayInspect,
                                   LayoutPreset::Author,
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
    if (ImGui::Begin(kPaneHome)) {
        draw_home_rail(s); // 20 T2: the task-language rail replaces the doors
        ImGui::Separator();
        ImGui::TextUnformatted("open recordings:");
        if (s.ws.recordings.empty())
            ImGui::TextDisabled("(none yet — pick a task above)");
        for (size_t i = 0; i < s.ws.recordings.size(); ++i) {
            // A dirty (authored + unsaved) recording carries a trailing `*` (T3).
            std::string label = disambiguated_label(s.ws, i) +
                                (s.ws.recordings[i].dirty ? " *" : "") +
                                "###home" + std::to_string(i);
            if (ImGui::Selectable(label.c_str(), s.active_tab == static_cast<int>(i)))
                s.active_tab = static_cast<int>(i);
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::SmallButton("x"))
                to_close = static_cast<int>(i);
            ImGui::PopID();
        }
    }
    ImGui::End();

    // The door screens open as their own dockable windows in the docked layout
    // (the door-chooser redesign, F13, is doc 21 — not here).
    if (s.show_author) {
        bool author_keep = true;
        std::string atitle =
            std::string("Author") + (s.author.dirty ? " *" : "") + "###authorwin";
        if (ImGui::Begin(atitle.c_str(), &author_keep))
            draw_author_door(s.author);
        ImGui::End();
        if (!author_keep)
            shell_request_author_close(s); // T3 dirty guard
    }
    if (s.show_inspect) {
        if (ImGui::Begin("Inspect", &s.show_inspect))
            draw_inspect_door(s.inspect);
        ImGui::End();
    }
    if (s.show_learn) {
        if (ImGui::Begin("Learn", &s.show_learn))
            draw_learn_door(s.learn, [&s](const std::string &path, long step) {
                learn_open(s, path, step);
            });
        ImGui::End();
    }

    // --- kPaneRecording (center): the light reading views, ONE flat tab bar ---
    if (ImGui::Begin(kPaneRecording)) {
        if (r == nullptr) {
            ImGui::TextDisabled(
                "open a recording (a door in Home, or the list) to read it here");
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
                size_t nabs = 0;
                for (const ViewPresence &e : vp) {
                    if (!hosts(e.id))
                        continue;
                    if (!e.present) {
                        nabs++;
                        continue;
                    }
                    ImGuiTabItemFlags fl =
                        (e.view && s.want_view == e.view)
                            ? ImGuiTabItemFlags_SetSelected
                            : 0;
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
                    ImGuiTabItemFlags fl =
                        absent_want ? ImGuiTabItemFlags_SetSelected : 0;
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
    ImGui::End();

    // --- kPaneLoom (center, co-docked as a tear-able tab) ---
    if (ImGui::Begin(kPaneLoom)) {
        if (a != nullptr) {
            shell_live_weave_banner(s); // 25 T5: perturb+torn caveat on a live weave
            draw_loom(
                s.loom, *a, s.ws, s.active_tab,
                [&s](const dt_link &l) { if (!dt_nav_go(s.nav, l)) s.status = s.nav.last_error; },
                &s.selection, &s.undo);
        } else
            ImGui::TextDisabled("open a recording to weave its Loom");
    }
    ImGui::End();

    // --- kPaneObserver: the live/observer deck (its observer bar is the sub-level)
    if (ImGui::Begin(kPaneObserver)) {
        if (r != nullptr && a != nullptr)
            body_observer(s, *r, a);
        else
            ImGui::TextDisabled("open a recording to see its live/observer views");
    }
    ImGui::End();

    // --- kPaneTimeline (bottom-left): the operand timeline ---
    // A spatial pane: while it (or the 3D view inside the recording pane) holds
    // focus, W/S/A/D drive the camera and `d` does NOT toggle the diff — the
    // labelled context switch (T1). Recorded as an explicit ShellState field the
    // keymap reads next frame, not an implicit focus guess.
    bool spatial_focus = false;
    if (ImGui::Begin(kPaneTimeline)) {
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
            spatial_focus = true;
        if (a != nullptr)
            body_timeline(s, a, b);
        else
            ImGui::TextDisabled("open a recording to see its operand timeline");
    }
    ImGui::End();
    s.wasd_context = spatial_focus;

    // --- kPaneScrubber (bottom-right): the register scrubber ---
    if (ImGui::Begin(kPaneScrubber)) {
        if (a != nullptr)
            body_scrubber(s);
        else
            ImGui::TextDisabled("open a recording to time-travel its registers");
    }
    ImGui::End();

    // --- kPaneInspector (right): ABI x-ray / Backends / This host, one flat bar
    if (ImGui::Begin(kPaneInspector)) {
        if (ImGui::BeginTabBar("inspector")) {
            if (ImGui::BeginTabItem("ABI x-ray")) {
                if (a != nullptr)
                    body_abixray(s, a, b);
                else
                    ImGui::TextDisabled("open a recording (and attach a Win64 leg, "
                                        "press d) for the ABI x-ray");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Backends")) {
                draw_completeness(s.completeness, s.repo_root);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("This host")) {
                if (r != nullptr)
                    draw_capability_panel(s.caps, r);
                else
                    ImGui::TextDisabled(
                        "open a recording to read this host against it");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();

    // The back/forward affordance + a nav refusal land below the reading pane.
    if (ImGui::Begin(kPaneRecording)) {
        ImGui::Separator();
        draw_breadcrumb(s); // T6 back/forward history affordance
        if (!s.status.empty())
            ImGui::TextWrapped("%s", s.status.c_str());
    }
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
        ImGui::TextWrapped("This recording has unsaved authored output. Closing "
                           "it now would lose the recording.");
        if (ImGui::Button("Save") && s.close_pending >= 0 &&
            s.close_pending < static_cast<int>(s.ws.recordings.size())) {
            Recording &rec = s.ws.recordings[static_cast<size_t>(s.close_pending)];
            std::string path = rec.path.empty() ? "authored.asmtrace" : rec.path;
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
        ImGui::TextWrapped("The Author tab has an unsaved run. Save it from the "
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
// size — and an HONEST statement of the a11y scope (ImGui exposes no OS
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
        ImGui::TextDisabled("Live via FontGlobalScale; the atlas re-bakes crisp "
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

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, dt_warn_col());
        ImGui::TextWrapped(
            "Accessibility: text-scale is the ONLY in-app a11y lever — Dear "
            "ImGui exposes no OS screen-reader tree, so a screen reader cannot "
            "read these panes. This is a recorded platform limit, not an "
            "omission.");
        ImGui::PopStyleColor();

        // Named filter presets (20 T4) over the active recording's syscall
        // filter. "Showing N of M" honesty stays exactly as the filter renders
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
        s.loom.takes = redo ? c.takes_after : c.takes_before;
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
        s.undo_filter_tab = s.active_tab; // adopt this recording's filter baseline
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
// never hides a row — the honesty distinction from a filter, D7), report the match
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
            "##findq", "find mnemonic / address / symbol (measures, never hides)",
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
            static_cast<size_t>(s.find.active) < s.find.hits.size())
            ImGui::TextDisabled(
                "match %d/%zu: %s", s.find.active + 1, s.find.hits.size(),
                s.find.hits[static_cast<size_t>(s.find.active)].label.c_str());
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
    // Keep the honesty-chrome theme flag + the live text-scale in step with the
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
    draw_settings(s);     // 20 T5 the Settings pane
    draw_find_bar(s);     // 22 T3 the global find (Ctrl+F)
    // 22 T4: coalesce a settled syscall-filter edit into one reversible Command,
    // AFTER the shell drew (and the user may have edited) the filter this frame.
    record_filter_undo(s);

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
        for (const SessionToast &t : live_session_toasts(s.prev_feedback, cur)) {
            ImGuiToastType ty = t.kind == ToastKind::Error     ? ImGuiToastType::Error
                                : t.kind == ToastKind::Warning ? ImGuiToastType::Warning
                                : t.kind == ToastKind::Success ? ImGuiToastType::Success
                                                               : ImGuiToastType::Info;
            if (t.open_path.empty()) {
                ImGui::InsertNotification(ImGuiToast(ty, 5000, "%s", t.text.c_str()));
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
        s.prev_feedback = cur;
        // Draw the queued toasts last, so they float over every pane.
        ImGui::RenderNotifications();
    }
}

} // namespace asmdesk
