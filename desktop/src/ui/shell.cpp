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
#include <cstdlib>   // std::strtoul — the go-to modal's step parse (17 T1)

#include "analysis/diff.h"  // dt_diff_build — n/p divergence walk (17 T1)
#include "analysis/slice.h"
#include "live/inspect.h" // live_session_toasts (16 T1)
#include "scene3d/hud.h"
#include "scene3d/pick.h"
#include "space/projection.h"
#include "views/abixray.h"
#include "views/views_draw.h"

namespace asmdesk {

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
    shell_wire_nav(s);
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
            s.selected_step = l.step;
            s.selected_off = l.off;
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

// The three doors on the home screen. Learn opens the open dialog; Author and
// Inspect are placeholders (behaviour in docs 06/08) — disabled with the reason
// in the render-only viewer, opening an empty named tab in the full app.
static void draw_doors(ShellState &s) {
    ImGui::TextUnformatted("asmtest desktop — choose a door");
    ImGui::Spacing();

    if (ImGui::Button("Learn"))
        s.show_learn = true;
    ImGui::SameLine();
    ImGui::TextDisabled("play a bundled walkthrough — no deps, no root");

    if (ImGui::Button("Open a recording...")) {
        s.open_dialog = true;
        s.open_error.clear();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("replay a .asmtrace you already have");

    if (ImGui::Button("Author"))
        s.show_author = true;
    ImGui::SameLine();
    ImGui::TextDisabled("type assembly, run it, see faults as data");
    // Inspect is in BOTH binaries — the one door where D9 pays off visibly.
    // It links no engine: it reads /proc itself and captures by spawning
    // `asmspy --serve` as a subprocess, so the render-only viewer hosts live
    // sessions with its `ldd` still free of every tracer.
    if (ImGui::Button("Inspect"))
        s.show_inspect = true;
    ImGui::SameLine();
    ImGui::TextDisabled("attach to a running process — see why not when you "
                        "cannot");
    ImGui::Spacing();
    if (ImGui::Button("Keyboard bindings"))
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

void draw_scene_overview(ShellState &s, const Recording &r, const Streams &a) {
    size_t i = static_cast<size_t>(s.active_tab);
    if (s.active_tab < 0 || i >= s.scenes.size())
        return;
    SceneView &sv = s.scenes[i];

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

    // Re-slice the terrain when the playhead moved (or on first build). O(touched
    // cells), far under frame budget for a golden recording (T2 step 3).
    if (sv.slice_t != sv.hud.t) {
        sv.slice = sv.terr.slice(sv.hud.t);
        sv.slice_t = sv.hud.t;
    }
    sv.hud.playhead_moved = false;

    // No plane to draw without regions — say so, never a blank void.
    if (!sv.has_regions) {
        ImGui::TextUnformatted(
            "no address-space regions in this recording — the 3D overview needs "
            "codeimage events (or a live maps snapshot) to place the plane. The "
            "provenance, trajectory and legend above still read.");
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
        return;
    }
    if (!s.scene_host->ready()) {
        ImGui::TextDisabled("3D scene did not initialise: %s",
                            s.scene_host->error());
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
        return;
    }
    // GL renders bottom-left origin; flip V so the image reads upright in ImGui.
    ImGui::Image(tex, ImVec2(static_cast<float>(fbw), static_cast<float>(fbh)),
                 ImVec2(0, 1), ImVec2(1, 0));

    // Camera + pick, only while the pointer is over the viewport. A left-drag
    // orbits; the wheel dollies; a click that did NOT drag is a pick — read the
    // id under the cursor and drill OUT to the flat 2D view through 04's router
    // (3D to find, 2D to read).
    if (ImGui::IsItemHovered()) {
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

// One open recording's pane: the summary chrome (03), then the replay views
// over it (04). Each builder is called fresh per frame from the decoded streams
// — they are pure and cheap at corpus scale; the threshold at which that stops
// being true is the worker-thread hand-off 04 describes, which lands with the
// first PT-scale recording.
static void draw_recording_tab(ShellState &s, const Recording &r) {
    if (ImGui::BeginTabBar("views")) {
        if (ImGui::BeginTabItem("Summary")) {
            draw_summary(r);
            ImGui::EndTabItem();
        }
        const Streams *a = shell_a(s);
        const Streams *b = shell_b(s);
        if (a != nullptr) {
            // The 1/2/3/4 keymap asks a view to select itself via want_view
            // (handle_keymap); honour it here with SetSelected, exactly as the
            // Loom tab honours want_loom. want_view is cleared once per frame
            // after this tab bar (below).
            auto view_flags = [&s](dt_view v) -> ImGuiTabItemFlags {
                return s.want_view == v ? ImGuiTabItemFlags_SetSelected : 0;
            };
            if (ImGui::BeginTabItem("Canvas", nullptr, view_flags(dt_view::canvas))) {
                s.view = dt_view::canvas;
                draw_canvas(b ? dt_canvas_build2(*a, *b) : dt_canvas_build(*a));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Timeline", nullptr, view_flags(dt_view::timeline))) {
                s.view = dt_view::timeline;
                dt_slice cone;
                if (s.cone_active && s.selected_step)
                    // `b` lights the backward cone (what produced the selection),
                    // `f` the forward cone (what it feeds) — s.cone_fwd (17-T1).
                    cone = s.cone_fwd
                               ? dt_slice_forward(a->df.edges, a->df.nsteps,
                                                  *s.selected_step)
                               : dt_slice_backward(a->df.edges, a->df.nsteps,
                                                   *s.selected_step);
                const dt_slice *lit = s.cone_active ? &cone : nullptr;
                draw_timeline(b ? dt_timeline_build2(*a, *b, lit)
                                : dt_timeline_build(*a, lit));
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Slice", nullptr, view_flags(dt_view::slice))) {
                s.view = dt_view::slice;
                draw_slice_view(dt_slice_view_build(*a, s.selected_step));
                if (b != nullptr)
                    // Never a fake merged graph: the two-recording slice needs
                    // the Wave-2 state-diff producer, and until it exists this
                    // says so instead of inventing one.
                    ImGui::TextDisabled(
                        "showing A only — slice diff lands with the "
                        "state-diff producer (Wave 2)");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diff", nullptr, view_flags(dt_view::diff))) {
                s.view = dt_view::diff;
                if (b == nullptr)
                    ImGui::TextDisabled(
                        "attach a second recording (press d) to "
                        "compare");
                else
                    draw_diff_view(dt_diff_view_build(*a, *b),
                                   [&s](const dt_link &l) {
                                       if (!dt_nav_go(s.nav, l))
                                           s.status = s.nav.last_error;
                                   });
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Observer")) {
                // The live views (08), over a recording. They are the SAME
                // code that renders a live session in the Inspect door — which
                // is how every one of them is testable without hardware.
                size_t i = static_cast<size_t>(s.active_tab);
                if (i < s.observers.size())
                    draw_observer(s.observers[i], r, a->id,
                                  [&s](const dt_link &l) {
                                      if (!dt_nav_go(s.nav, l))
                                          s.status = s.nav.last_error;
                                  });
                ImGui::EndTabItem();
            }
            ImGuiTabItemFlags loom_flags = 0;
            if (s.want_loom)
                loom_flags |= ImGuiTabItemFlags_SetSelected;
            if (ImGui::BeginTabItem("Loom", nullptr, loom_flags)) {
                // The Phase-2 flagship: the recording as a spacetime fabric.
                // It weaves from the SAME decoded streams the other views use,
                // and refuses — with its reason on screen and no partial
                // drawing — for a recording whose producer was statistical or
                // carried no per-step values.
                draw_loom(s.loom, *a, s.ws, s.active_tab);
                ImGui::EndTabItem();
            }
            // The register time-travel scrubber (09-T3), surfaced. Over this
            // recording's `regstate` ring; an absent producer draws its own
            // placard (never a register file of zeros), exactly as the standalone
            // draw does. The playhead is the caller's — the slider and `[` / `]`
            // keys move it, and draw_scrubber returns the moved value to persist.
            if (ImGui::BeginTabItem("Scrubber")) {
                size_t i = static_cast<size_t>(s.active_tab);
                if (i < s.stepidx.size())
                    s.scrubber_playhead[i] =
                        draw_scrubber(s.stepidx[i], s.scrubber_playhead[i]);
                ImGui::EndTabItem();
            }
            // The ABI x-ray (09-T4), surfaced. It locks TWO scrubber panes to one
            // playhead — the active recording is the SysV leg, the attached B
            // (press `d`) the Win64 leg — reusing the Diff tab's A/B mechanism.
            // With no B attached it shows the same "attach a second recording"
            // placard shape; the view's own honesty banners handle an unaligned
            // pair or an absent per-pane producer.
            if (ImGui::BeginTabItem("ABI x-ray")) {
                if (b == nullptr) {
                    ImGui::TextDisabled(
                        "attach the Win64 leg (press d) — the ABI x-ray locks "
                        "this recording (the SysV leg) against it");
                } else {
                    size_t ai = static_cast<size_t>(s.active_tab);
                    size_t bi = static_cast<size_t>(s.b_index);
                    // The rail MUTATES the walk (stop navigation), so it persists
                    // across frames; rebuild only when the pair changes.
                    std::string key = a->id + "\x1f" + b->id;
                    if (s.abixray_key != key) {
                        s.abixray_key = key;
                        s.abixray_walk = wt_build(s.ws.recordings[ai]);
                        s.abixray_playhead =
                            dt_abixray_playhead(s.abixray_walk);
                    }
                    if (ai < s.stepidx.size() && bi < s.stepidx.size())
                        draw_abixray(s.stepidx[ai], s.stepidx[bi],
                                     s.abixray_walk, s.abixray_playhead);
                }
                ImGui::EndTabItem();
            }
            // The 3D spacetime overview (doc 10), surfaced. The pure space/ models
            // weave engine-free; the scene itself needs a live GL context, drawn
            // by s.scene_host (threaded from main.cpp, null under the null test
            // backend — the pane then shows the models + HUD + a placard). Every
            // pick drills OUT to a flat 2D view through 04's router.
            if (ImGui::BeginTabItem("3D overview")) {
                draw_scene_overview(s, r, *a);
                ImGui::EndTabItem();
            }
        }
        if (ImGui::BeginTabItem("Backends")) {
            draw_completeness(s.completeness, s.repo_root);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("This host")) {
            // What THIS machine can do and why not, straight from the
            // library's status APIs (06 T6). The render-only viewer shows the
            // loaded recording's provenance instead and says so.
            draw_capability_panel(s.caps, &r);
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

    // y — copy a deep link to the current position to the clipboard.
    if (ImGui::IsKeyPressed(ImGuiKey_Y) && s.nav.current)
        ImGui::SetClipboardText(dt_nav_format(*s.nav.current).c_str());

    // Ctrl+G — open the go-to-step/offset modal (its InputText owns the text).
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G))
        s.show_goto = true;

    // The rest act on the active recording's selection; with none, nothing to do.
    const Streams *a = shell_a(s);
    if (a == nullptr)
        return;
    // The step space is the dataflow's — the same one the slice/cone index, so a
    // stepped selection lands on a node the slice explorer can actually show.
    const uint32_t maxstep = a->df.nsteps > 0 ? a->df.nsteps - 1 : 0;
    const uint32_t cur = s.selected_step.value_or(0);
    const uint32_t kPage = 20;

    // j/k, Down/Up — next / previous step (clamped to the step space).
    if (ImGui::IsKeyPressed(ImGuiKey_J) || ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        s.selected_step = cur < maxstep ? cur + 1 : maxstep;
    if (ImGui::IsKeyPressed(ImGuiKey_K) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        s.selected_step = cur > 0 ? cur - 1 : 0;
    // PgDn/PgUp — page through the step space.
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
        s.selected_step = cur + kPage <= maxstep ? cur + kPage : maxstep;
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
        s.selected_step = cur > kPage ? cur - kPage : 0;

    // Enter — open the slice explorer at the selected step, cone lit.
    if (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
        s.want_view = dt_view::slice;
        s.cone_active = true;
    }

    // b / f — light the backward / forward cone from the selection; c — clear.
    if (ImGui::IsKeyPressed(ImGuiKey_B)) { s.cone_active = true; s.cone_fwd = false; }
    if (ImGui::IsKeyPressed(ImGuiKey_F)) { s.cone_active = true; s.cone_fwd = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_C)) s.cone_active = false;

    // d — attach a second recording for the diff (the first OTHER open one), or
    // detach the one attached. x — swap which is A and which is B.
    if (ImGui::IsKeyPressed(ImGuiKey_D)) {
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
    if (ImGui::IsKeyPressed(ImGuiKey_N) || ImGui::IsKeyPressed(ImGuiKey_P)) {
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
                    s.selected_off.value_or(fwd ? 0 : UINT64_MAX);
                std::optional<uint64_t> tgt;
                if (fwd) {
                    for (uint64_t o : offs)
                        if (o > curoff) { tgt = o; break; }
                } else {
                    for (auto it = offs.rbegin(); it != offs.rend(); ++it)
                        if (*it < curoff) { tgt = *it; break; }
                }
                if (tgt)
                    s.selected_off = *tgt;
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

void draw_shell(ShellState &s) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();

    // The advertised keyboard bindings, acted on first so a keypress and the
    // matching click land in the same frame (17-T1).
    handle_keymap(s);

    // Docking (13-foundation-moves.md T2): when enabled (the real app; the null
    // test backend leaves it OFF, so everything below is skipped and the shell
    // draws exactly as before), host a full-viewport dockspace with a passthru
    // central node so panes can be torn out and re-docked, and build the shipped
    // default layout on first run (unless a layout was already persisted).
    const bool docking =
        (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) != 0;
    ImGuiID dockspace_id = 0;
    if (docking) {
        dockspace_id = ImGui::DockSpaceOverViewport(
            0, vp, ImGuiDockNodeFlags_PassthruCentralNode);
        if (!s.layout_inited) {
            s.layout_inited = true;
            if (!layout_exists(dockspace_id))
                layout_build(dockspace_id, vp->WorkSize,
                             LayoutPreset::ReplayInspect);
        }
    }

    // Pin the shell to the whole viewport. Without this the "asmtest" window is a
    // floating panel that ImGui auto-fits to a tiny default size on the first
    // frame (and, with IniFilename disabled in main.cpp, never remembers a
    // resize) — so the app reads as "starts very small" inside the OS frame.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags shell_flags = ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoTitleBar;
    if (docking)
        shell_flags |= ImGuiWindowFlags_MenuBar; // the View menu below
    ImGui::Begin("asmtest", nullptr, shell_flags);
    if (docking && ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Reset layout"))
                layout_build(dockspace_id, vp->WorkSize,
                             LayoutPreset::ReplayInspect);
            ImGui::Separator();
            for (LayoutPreset p : {LayoutPreset::ReplayInspect,
                                   LayoutPreset::Author,
                                   LayoutPreset::LiveObserver})
                if (ImGui::MenuItem(layout_preset_name(p)))
                    layout_build(dockspace_id, vp->WorkSize, p);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("main", ImGuiTabBarFlags_AutoSelectNewTabs)) {
        if (ImGui::BeginTabItem("Home")) {
            s.active_tab = -1;
            draw_doors(s);
            ImGui::EndTabItem();
        }

        // The Learn door: bundled walkthroughs, in BOTH binaries (it reads
        // recordings and links no engine — D4).
        // The Author door: full app only, and the render-only build says why
        // rather than hiding the tab (D4's split has to be legible).
        if (s.show_author && ImGui::BeginTabItem("Author", &s.show_author)) {
            draw_author_door(s.author);
            ImGui::EndTabItem();
        }

        if (s.show_inspect && ImGui::BeginTabItem("Inspect", &s.show_inspect)) {
            draw_inspect_door(s.inspect);
            ImGui::EndTabItem();
        }

        // A capture the Inspect door just saved wants to open in the Loom. The
        // door cannot reach the Workspace, so it posts the path here and the
        // shell opens it exactly as the recording-open dialog would, then jumps
        // the tab strip to it (want_open_tab) and to its Loom (want_loom).
        if (!s.inspect.open_request.empty()) {
            std::string path = s.inspect.open_request;
            s.inspect.open_request.clear();
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

        if (s.show_learn && ImGui::BeginTabItem("Learn", &s.show_learn)) {
            draw_learn_door(s.learn, [&s](const std::string &path, long step) {
                // Route through 04's router rather than reaching into the
                // views: a stop and a pasted deep link must land identically.
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
            });
            ImGui::EndTabItem();
        }

        // One tab per open recording (title = filename); ### keeps the id stable
        // across renames. A closed tab is collected and applied after the loop so
        // the index walk is never invalidated mid-iteration.
        int to_close = -1;
        for (size_t i = 0; i < s.ws.recordings.size(); ++i) {
            const Recording &r = s.ws.recordings[i];
            std::string title =
                base_name(r.path) + "###rec" + std::to_string(i);
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
        if (to_close >= 0)
            shell_close(s, static_cast<size_t>(to_close));

        // Placeholder door tabs (full app only; empty until docs 06/08).
        for (size_t i = 0; i < s.door_tabs.size(); ++i) {
            std::string title = s.door_tabs[i] + "###door" + std::to_string(i);
            if (ImGui::BeginTabItem(title.c_str())) {
                ImGui::TextDisabled("%s — view lands in a later doc",
                                    s.door_tabs[i].c_str());
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();

    if (s.open_dialog)
        draw_open_dialog(s);
    if (s.show_help) {
        ImGui::Begin("Keyboard bindings", &s.show_help);
        draw_bindings_help();
        ImGui::End();
    }
    draw_goto_modal(s); // Ctrl+G (17-T1)

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
