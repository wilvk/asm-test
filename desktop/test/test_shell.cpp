// test_shell.cpp — the shell's honesty behaviour + a headless render smoke
// (03-desktop-shell.md T6). shell_banner is D7 as behaviour: non-null for a
// truncated / dropped / torn recording, null for a clean one. Then draw_shell is
// driven for 3 null-backend frames over a Workspace of the fixtures to prove no
// path crashes without a display. No GLFW, no GL, no engines.
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // 19: FindWindowByName + ImGuiWindow::{WasActive,DockId}

#include "doc/recording.h"
#include "ui/layout.h" // 19: kPane* names, DockLayout, LayoutPreset, layout_build
#include "ui/shell.h"
#include "views/abixray.h"    // 09-T4: the surfaced ABI x-ray tab's model
#include "views/slice_view.h" // 09-T5: assert the blame link's backward cone
#include "walkthrough.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}
static std::string fx(const char *name) {
    return std::string(ASMTEST_FIXTURE_DIR) + "/" + name;
}
static std::string gd(const char *name) {
    return std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
}

// shell_banner over a single fixture that must load.
static void banner_case(const char *what, const char *file, bool want_banner) {
    std::string err;
    auto rec = load_recording_file(fx(file), err);
    if (!rec) {
        std::fprintf(stderr, "FAIL %s: fixture did not load: %s\n", what,
                     err.c_str());
        failures++;
        return;
    }
    const char *b = shell_banner(*rec);
    if (want_banner)
        check(what, b != nullptr, "expected a banner, got none");
    else
        check(what, b == nullptr, "expected NO banner, got one");
}

int main() {
    // D7 as behaviour: the dishonest fixtures banner, the clean one does not.
    banner_case("banner/truncated", "truncated.asmtrace", true);
    banner_case("banner/dropped", "dropped.asmtrace", true);
    banner_case("banner/torn-tail", "torn-tail.asmtrace", true);
    banner_case("banner/clean", "min-trace.asmtrace", false);

    // Headless render smoke: open every loadable fixture into one ShellState and
    // run 3 null-backend frames of the full shell (home doors + tabs + summary +
    // the open dialog + a door tab) — nothing may crash without a display.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    ShellState s;
    const char *loadable[] = {"min-trace.asmtrace",    "truncated.asmtrace",
                              "dropped.asmtrace",      "redacted.asmtrace",
                              "unknown-kind.asmtrace", "torn-tail.asmtrace"};
    for (const char *f : loadable) {
        std::string err;
        s.ws.open(fx(f), err); // rejects can't happen for these; ignore err
    }
    check("smoke/opened", s.ws.recordings.size() == 6, "all six should open");
    s.open_dialog = true;            // exercise the open-dialog draw path
    s.door_tabs.push_back("Author"); // exercise a placeholder door tab
    // The Inspect door (07 T4/T5) draws in BOTH binaries, so its whole path —
    // the /proc list, the patch bay's budget decision, the live status — runs
    // headlessly here with no serve host connected. That "not connected yet"
    // state is the one every user sees first and the easiest to leave broken.
    s.show_inspect = true;

    for (int frame = 0; frame < 3; frame++) {
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        draw_shell(s);
        ImGui::Render();
        if (ImGui::GetDrawData() == nullptr) {
            std::fprintf(stderr, "FAIL smoke/frame %d: null draw data\n",
                         frame);
            failures++;
        }
    }

    // A verbatim open error must surface (never a silent no-op).
    {
        std::string err;
        int idx = s.ws.open(fx("does-not-exist.asmtrace"), err);
        check("open-error/idx", idx == -1, "missing file should return -1");
        check("open-error/msg", !err.empty(), "missing file should set err");
    }

    ImGui::DestroyContext();

    // --- 09-T5: the blame deep-link intake socket, end to end ---------------
    // A blame link (v=blame&rec=…&step=N) is the failure-attribution entry
    // point. There is no Wave-2 blame PRODUCER yet, so this drives a hand-
    // authored fixture carrying a `blame`-kind attachment: routing the link must
    // open the SLICE explorer at the failure step with the backward cone ("what
    // produced this wrong value") pre-selected. RE-POINT `rec`/`step` at a
    // produced recording when the Wave-2 backward-slice blame producer lands.
    {
        ShellState bs;
        std::string err;
        int idx = shell_open(bs, fx("blame-attribution.asmtrace"), err);
        check("blame/opened", idx >= 0, err.c_str());

        dt_link l;
        l.view = dt_view::blame;
        l.rec = "blame-attribution.asmtrace";
        l.step = 3; // the failing step named by the fixture's blame event

        check("blame/navigates", dt_nav_go(bs.nav, l),
              bs.nav.last_error.c_str());
        check("blame/opens the slice explorer", bs.view == dt_view::slice,
              "blame must resolve onto the slice explorer, not a view of its "
              "own");
        check("blame/lands on the failure step",
              bs.selected_step.value_or(999) == 3, "wrong step");
        check("blame/lights the cone", bs.cone_active,
              "the backward cone must be active at the failure step");

        // The backward cone is actually pre-selected: the producers of the wrong
        // value light `back`, the failure itself `both`, and the unrelated ebx
        // chain stays `dimmed`. This is the whole "zero UI work left for Wave 2"
        // claim, asserted over the real slice builder.
        const Streams *a = shell_a(bs);
        check("blame/active stream decoded", a != nullptr, "no active stream");
        if (a != nullptr) {
            dt_slice_view v = dt_slice_view_build(*a, bs.selected_step);
            auto style = [&](uint32_t step) {
                for (const dt_slice_node &n : v.nodes)
                    if (n.step == step)
                        return n.style;
                return dt_cone::none;
            };
            check("blame/failure step is the cone origin",
                  style(3) == dt_cone::both, "the failure is in both cones");
            check("blame/its producers light the backward cone",
                  style(0) == dt_cone::back && style(2) == dt_cone::back,
                  "steps 0 and 2 produced the failing value");
            check("blame/unrelated steps stay dimmed",
                  style(1) == dt_cone::dimmed && style(4) == dt_cone::dimmed,
                  "the ebx chain neither produced nor consumed the value");
        }
    }

    // --- 09-T3/T4: the surfaced Scrubber + ABI x-ray tabs are WIRED ---------
    // The draw halves are covered by test_scrubber_draw / test_abixray_draw;
    // this pins the SHELL wiring that feeds them: shell_open builds the regstate
    // seek index parallel to the workspace, the playhead vector stays in step,
    // and the A/B pair the ABI x-ray tab reads produces an aligned, present
    // x-ray through the very same calls the tab makes.
    {
        ImGui::CreateContext();
        ImGuiIO &io2 = ImGui::GetIO();
        io2.IniFilename = nullptr;
        unsigned char *p2 = nullptr;
        int w2 = 0, h2 = 0;
        io2.Fonts->GetTexDataAsRGBA32(&p2, &w2, &h2);

        ShellState s2;
        std::string err;
        // The SysV leg (active) + its Win64 leg (the B attachment the x-ray reads).
        int isv = shell_open(s2, gd("abixray-make_pair-sysv.asmtrace"), err);
        check("wire/sysv opened", isv >= 0, err.c_str());
        int iw64 = shell_open(s2, gd("abixray-make_pair-win64.asmtrace"), err);
        check("wire/win64 opened", iw64 >= 0, err.c_str());
        // A plain regstate recording, for the standalone scrubber tab.
        int ias = shell_open(s2, gd("add_signed.asmtrace"), err);
        check("wire/regstate opened", ias >= 0, err.c_str());

        // The parallel-vector invariant every per-recording view relies on.
        size_t n = s2.ws.recordings.size();
        check("wire/stepidx parallel", s2.stepidx.size() == n,
              "stepidx must be parallel to the workspace");
        check("wire/playhead parallel", s2.scrubber_playhead.size() == n,
              "the scrubber playhead vector must be parallel to the workspace");

        // The scrubber's producer is PRESENT for a regstate recording — the tab
        // shows the deck, not the absent placard.
        if (ias >= 0 && static_cast<size_t>(ias) < s2.stepidx.size())
            check("wire/scrubber present",
                  s2.stepidx[static_cast<size_t>(ias)].present(),
                  "add_signed carries a regstate ring");

        // The ABI x-ray tab's exact feed: SysV index (active) locked against the
        // Win64 index (B) at the walkthrough's playhead. A make_pair pair is
        // step-aligned and present — the tab draws the two locked panes, not a
        // refusal banner.
        if (isv >= 0 && iw64 >= 0) {
            wt_model walk =
                wt_build(s2.ws.recordings[static_cast<size_t>(isv)]);
            uint64_t ph = dt_abixray_playhead(walk);
            dt_abixray x = dt_abixray_seek(
                s2.stepidx[static_cast<size_t>(isv)],
                s2.stepidx[static_cast<size_t>(iw64)], walk, ph);
            check("wire/abixray present", x.present,
                  "both legs carry a regstate producer");
            check("wire/abixray aligned", x.aligned,
                  "the SysV and Win64 legs of a pair share one step space");
        }

        // Drive the shell over this workspace (SysV active, Win64 attached as B):
        // every tab BUTTON — Scrubber and ABI x-ray included — is emitted each
        // frame, so the tab strip must not crash with the pair loaded.
        s2.active_tab = isv;
        s2.b_index = iw64;
        for (int frame = 0; frame < 3; frame++) {
            io2.DisplaySize = ImVec2(1280, 720);
            io2.DeltaTime = 1.0f / 60.0f;
            ImGui::NewFrame();
            draw_shell(s2);
            ImGui::Render();
            if (ImGui::GetDrawData() == nullptr) {
                std::fprintf(stderr, "FAIL wire/frame %d: null draw data\n",
                             frame);
                failures++;
            }
        }

        // Closing a recording keeps every parallel vector aligned to the
        // workspace — a stale stepidx would seek the wrong recording's registers.
        shell_close(s2, static_cast<size_t>(isv));
        check("wire/close keeps stepidx parallel",
              s2.stepidx.size() == s2.ws.recordings.size(),
              "shell_close must erase the stepidx slot too");
        check("wire/close keeps playhead parallel",
              s2.scrubber_playhead.size() == s2.ws.recordings.size(),
              "shell_close must erase the playhead slot too");

        ImGui::DestroyContext();
    }

    // --- doc 10: the surfaced 3D-overview pane is WIRED ---------------------
    // The GL scene (test_scene_fbo) and the pick router (test_drillin) are pinned
    // elsewhere; this pins the SHELL wiring that feeds the pane: shell_open keeps
    // the per-recording SceneView vector parallel to the workspace, and
    // draw_scene_overview weaves the pure engine-free models (projection +
    // terrain + trajectory) and draws the HUD under the null backend with NO GL
    // host attached — the exact path a headless run and the render-only viewer
    // take. A golden scene recording (codeimage + abs trace) builds real regions;
    // a codeimage-less recording takes the honest "no regions" placard.
    {
        ImGui::CreateContext();
        ImGuiIO &io3 = ImGui::GetIO();
        io3.IniFilename = nullptr;
        unsigned char *p3 = nullptr;
        int w3 = 0, h3 = 0;
        io3.Fonts->GetTexDataAsRGBA32(&p3, &w3, &h3);
        io3.DisplaySize = ImVec2(1280, 720);
        io3.DeltaTime = 1.0f / 60.0f;

        ShellState s3;
        check("scene/no host by default", s3.scene_host == nullptr,
              "the null backend must leave scene_host null");
        std::string err;
        int igs = shell_open(s3, gd("scene-abs-loop.asmtrace"), err);
        check("scene/golden opened", igs >= 0, err.c_str());
        int imt = shell_open(s3, fx("min-trace.asmtrace"), err);
        check("scene/min-trace opened", imt >= 0, err.c_str());

        // Parallel to the workspace, like every other per-recording vector.
        check("scene/scenes parallel", s3.scenes.size() == s3.ws.recordings.size(),
              "the SceneView vector must be parallel to the workspace");

        // Drive the pane for the golden scene (codeimage + abs trace): the models
        // weave and the HUD draws, all with no GL host — the placard stands in for
        // the viewport, nothing crashes.
        if (igs >= 0) {
            s3.active_tab = igs;
            const Streams *a = shell_a(s3);
            check("scene/golden stream decoded", a != nullptr, "no stream");
            ImGui::NewFrame();
            ImGui::Begin("t3");
            if (a != nullptr)
                draw_scene_overview(s3, s3.ws.recordings[static_cast<size_t>(igs)],
                                    *a);
            ImGui::End();
            ImGui::Render();

            const SceneView &sv = s3.scenes[static_cast<size_t>(igs)];
            check("scene/models woven", sv.built, "the pane must build the models");
            check("scene/regions placed", sv.has_regions,
                  "a codeimage recording must place code regions on the plane");
            check("scene/terrain sized", sv.terr.w > 0 && sv.terr.h > 0,
                  "the terrain plane must be sized");
            check("scene/abs terrain has heat", !sv.terr.code.empty(),
                  "the abs trace must populate code cells");
            check("scene/exact trajectory", !sv.traj.refused() &&
                                                !sv.traj.trajectories.empty() &&
                                                sv.traj.basis == "abs",
                  "an abs recording must weave one exact trajectory");
            check("scene/slice cut at the playhead", sv.slice_t == sv.hud.t,
                  "the terrain slice must track the HUD playhead");
        }

        // The codeimage-less recording takes the "no regions" placard path
        // without weaving a plane — and still without a crash under the null
        // backend.
        if (imt >= 0) {
            s3.active_tab = imt;
            const Streams *a = shell_a(s3);
            ImGui::NewFrame();
            ImGui::Begin("t3b");
            if (a != nullptr)
                draw_scene_overview(s3, s3.ws.recordings[static_cast<size_t>(imt)],
                                    *a);
            ImGui::End();
            ImGui::Render();
            const SceneView &sv = s3.scenes[static_cast<size_t>(imt)];
            check("scene/no-regions is honest", sv.built && !sv.has_regions,
                  "a codeimage-less recording must take the no-regions placard");
        }

        // Closing keeps the SceneView vector parallel — a stale slice would drive
        // the wrong recording's plane.
        shell_close(s3, static_cast<size_t>(igs));
        check("scene/close keeps scenes parallel",
              s3.scenes.size() == s3.ws.recordings.size(),
              "shell_close must erase the SceneView slot too");

        ImGui::DestroyContext();
    }

    // --- 19 (dockable panes keystone): the docked shell draws REAL kPane* panes -
    // Every block above ran the NON-DOCKED path — the null backend leaves docking
    // off, so `draw_shell` drew the single-window tab layout and the dockspace,
    // presets and Reset acted on phantom windows. This block flips
    // ImGuiConfigFlags_DockingEnable on its OWN context (IniFilename still null —
    // file-free and deterministic, doc 13:394) and pins the thing that was false
    // before this brief: each region layout.cpp docks is a window the shell
    // Begin()s, visible and active, and the timeline, the scrubber and the
    // Observer deck (which hosts the disassembly) can all be shown at once —
    // impossible under the old exclusive "views" tab bar.
    {
        ImGui::CreateContext();
        ImGuiIO &iod = ImGui::GetIO();
        iod.IniFilename = nullptr; // still file-free — DockBuilder driven in-proc
        iod.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        unsigned char *pd = nullptr;
        int wd = 0, hd = 0;
        iod.Fonts->GetTexDataAsRGBA32(&pd, &wd, &hd);
        iod.DisplaySize = ImVec2(1280, 720);
        iod.DeltaTime = 1.0f / 60.0f;

        auto frame = [](ShellState &st) {
            ImGui::NewFrame();
            draw_shell(st);
            ImGui::Render();
        };
        auto dockid = [](const char *n) -> ImGuiID {
            ImGuiWindow *w = ImGui::FindWindowByName(n);
            return w ? w->DockId : 0;
        };
        auto active = [](const char *n) -> bool {
            ImGuiWindow *w = ImGui::FindWindowByName(n);
            return w != nullptr && w->WasActive;
        };
        // DockBuilder mutates the dock tree and needs the frame's current window
        // (the implicit Debug window ImGui sets up in NewFrame), so it must run
        // INSIDE a frame — exactly as test_layout drives it, and as the View menu
        // does mid-draw_shell. This applies a preset the menu's own way, then
        // settles two frames so the panes adopt the new nodes.
        auto apply_preset = [&](ShellState &st, LayoutPreset p) {
            ImGui::NewFrame();
            layout_build(st.dockspace_id, ImVec2(1280, 720), p);
            ImGui::Render();
            frame(st);
            frame(st);
        };

        ShellState ds;
        std::string err;
        // A codeimage recording (so the Observer deck carries a Disassembly tab)
        // + a producer-absent one (min-trace: no regstate ring, no codeimage — the
        // placard fixtures).
        int icode = shell_open(ds, gd("scene-abs-loop.asmtrace"), err);
        check("dock/codeimage opened", icode >= 0, err.c_str());
        int imin = shell_open(ds, fx("min-trace.asmtrace"), err);
        check("dock/min-trace opened", imin >= 0, err.c_str());
        ds.active_tab = icode;

        // Frame 1 builds the default ReplayInspect layout and publishes the
        // dockspace id; a second lets docking settle.
        frame(ds);
        check("dock/dockspace published", ds.dockspace_id != 0,
              "the docked shell must publish its DockSpaceOverViewport id");
        frame(ds);

        // Arrange the panes into DISTINCT nodes (the brief's T2 "dock them into
        // distinct nodes") so each asserted pane is a sole, unambiguously visible
        // occupant: Home | center(Recording+Loom) | Inspector / Observer |
        // Timeline | Scrubber. Driven INSIDE a frame (DockBuilder needs the
        // current window).
        ImGui::NewFrame();
        {
            ImGuiID root = ds.dockspace_id;
            ImGui::DockBuilderRemoveNode(root);
            ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(root, ImVec2(1280, 720));
            ImGuiID c = root;
            ImGuiID nleft =
                ImGui::DockBuilderSplitNode(c, ImGuiDir_Left, 0.18f, nullptr, &c);
            ImGuiID nright =
                ImGui::DockBuilderSplitNode(c, ImGuiDir_Right, 0.24f, nullptr, &c);
            ImGuiID nrightb = ImGui::DockBuilderSplitNode(nright, ImGuiDir_Down,
                                                          0.5f, nullptr, &nright);
            ImGuiID nbot =
                ImGui::DockBuilderSplitNode(c, ImGuiDir_Down, 0.30f, nullptr, &c);
            ImGuiID nbot2 = ImGui::DockBuilderSplitNode(nbot, ImGuiDir_Right, 0.5f,
                                                        nullptr, &nbot);
            ImGui::DockBuilderDockWindow(kPaneHome, nleft);
            ImGui::DockBuilderDockWindow(kPaneRecording, c);
            ImGui::DockBuilderDockWindow(kPaneLoom, c);
            ImGui::DockBuilderDockWindow(kPaneInspector, nright);
            ImGui::DockBuilderDockWindow(kPaneObserver, nrightb);
            ImGui::DockBuilderDockWindow(kPaneTimeline, nbot);
            ImGui::DockBuilderDockWindow(kPaneScrubber, nbot2);
            ImGui::DockBuilderFinish(root);
        }
        ImGui::Render();
        frame(ds);
        frame(ds);

        // T1 — each of the five region panes layout.cpp docks EXISTS and was
        // ACTIVE, i.e. actually Begin()'d. This is the exact thing that was false
        // before 19: no view was Begin()'d under any kPane* name.
        const char *core[] = {kPaneHome, kPaneRecording, kPaneScrubber,
                              kPaneInspector, kPaneTimeline};
        for (const char *name : core) {
            check("dock/pane exists", ImGui::FindWindowByName(name) != nullptr,
                  name);
            check("dock/pane was active", active(name), name);
        }

        // T2 — the refutation of "only one view visible" (F2/F9): the timeline,
        // the scrubber and the Observer deck are all active in ONE frame, in
        // distinct dock nodes. The old exclusive "views" tab bar could never make
        // three siblings active at once.
        check("dock/timeline+scrubber+observer coexist",
              active(kPaneTimeline) && active(kPaneScrubber) &&
                  active(kPaneObserver),
              "the three sibling panes must be simultaneously active");
        check("dock/coexist distinct nodes",
              dockid(kPaneTimeline) != dockid(kPaneScrubber) &&
                  dockid(kPaneTimeline) != dockid(kPaneObserver) &&
                  dockid(kPaneScrubber) != dockid(kPaneObserver),
              "the three panes must occupy distinct dock nodes");

        // T1 honesty — the placards survive the move into the panes (D7). Switch
        // the active recording to the producer-absent min-trace and drive frames;
        // the scrubber pane is active (so it drew) while its regstate producer is
        // absent — it drew the placard, not a register file of zeros.
        ds.active_tab = imin;
        frame(ds);
        frame(ds);
        check("dock/scrubber placard producer-absent",
              imin >= 0 && static_cast<size_t>(imin) < ds.stepidx.size() &&
                  !ds.stepidx[static_cast<size_t>(imin)].present(),
              "min-trace has no regstate ring — the scrubber pane must placard");
        check("dock/scrubber pane active for placard", active(kPaneScrubber),
              "the scrubber pane must be active to have drawn its placard");
        // The 3D overview's no-regions placard likewise survives into the pane
        // body: min-trace has no codeimage, so draw_scene_overview weaves the
        // models but takes the "no address-space regions" placard path.
        {
            const Streams *am = shell_a(ds);
            ImGui::NewFrame();
            ImGui::Begin("dock-probe-3d");
            if (am != nullptr)
                draw_scene_overview(
                    ds, ds.ws.recordings[static_cast<size_t>(imin)], *am);
            ImGui::End();
            ImGui::Render();
            check("dock/3D placard producer-absent",
                  static_cast<size_t>(imin) < ds.scenes.size() &&
                      ds.scenes[static_cast<size_t>(imin)].built &&
                      !ds.scenes[static_cast<size_t>(imin)].has_regions,
                  "min-trace has no codeimage — the 3D pane must take the "
                  "no-regions placard");
        }

        // T3 — presets + Reset rearrange the REAL panes. Build ReplayInspect, note
        // the timeline/scrubber dock nodes, switch to LiveObserver and assert both
        // MOVED (a preset acting on visible windows), then Reset and assert it
        // RESTORES them. Both are the exact calls the View menu makes.
        apply_preset(ds, LayoutPreset::ReplayInspect);
        ImGuiID tl_ri = dockid(kPaneTimeline);
        ImGuiID sc_ri = dockid(kPaneScrubber);
        check("dock/preset docked timeline", tl_ri != 0,
              "the timeline pane must be docked in ReplayInspect");
        apply_preset(ds, LayoutPreset::LiveObserver);
        check("dock/preset moves timeline", dockid(kPaneTimeline) != tl_ri,
              "a preset switch must move the timeline pane to a different node");
        check("dock/preset moves scrubber", dockid(kPaneScrubber) != sc_ri,
              "a preset switch must move the scrubber pane to a different node");
        // Reset layout (the menu's own call) restores the default assignment.
        apply_preset(ds, LayoutPreset::ReplayInspect);
        check("dock/reset restores timeline", dockid(kPaneTimeline) == tl_ri,
              "Reset must restore the timeline pane's node");
        check("dock/reset restores scrubber", dockid(kPaneScrubber) == sc_ri,
              "Reset must restore the scrubber pane's node");

        ImGui::DestroyContext();
    }

    if (failures) {
        std::fprintf(stderr, "test_shell: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_shell: PASS\n");
    return 0;
}
