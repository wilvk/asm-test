// test_shell.cpp — the shell's fidelity behaviour + a headless render smoke
// (03-desktop-shell.md T6). shell_banner is D7 as behaviour: non-null for a
// truncated / dropped / torn recording, null for a clean one. Then draw_shell is
// driven for 3 null-backend frames over a Workspace of the fixtures to prove no
// path crashes without a display. No GLFW, no GL, no engines.
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // 19: FindWindowByName + ImGuiWindow::{WasActive,DockId}

#include "doc/recording.h"
#include "doc/workspace_state.h" // 20 T3: capture/restore round-trip
#include "scene3d/hud.h"         // 36 T4: placement_chips (fidelity chrome)
#include "ui/layout.h" // 19: kPane* names, DockLayout, LayoutPreset, layout_build
#include "ui/shell.h"
#include "ui/view_presence.h" // 20 T1: the data-driven view set
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

// 25-live-model-wiring.md: look up one view's presence entry by id (every id is
// always in the vector, so a caller checks `->present`, never null for these).
static const ViewPresence *find_view(const std::vector<ViewPresence> &vp,
                                     ViewId id) {
    for (const ViewPresence &e : vp)
        if (e.id == id)
            return &e;
    return nullptr;
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
    // D7 as behaviour: the low-fidelity fixtures banner, the clean one does not.
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
    s.open_dialog = true;   // exercise the open-dialog draw path
    s.show_settings = true; // exercise the Settings pane draw path (20 T5)
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
              bs.selection.step.value_or(999) == 3, "wrong step");
        check("blame/lights the cone", bs.cone_active,
              "the backward cone must be active at the failure step");

        // The backward cone is actually pre-selected: the producers of the wrong
        // value light `back`, the failure itself `both`, and the unrelated ebx
        // chain stays `dimmed`. This is the whole "zero UI work left for Wave 2"
        // claim, asserted over the real slice builder.
        const Streams *a = shell_a(bs);
        check("blame/active stream decoded", a != nullptr, "no active stream");
        if (a != nullptr) {
            dt_slice_view v = dt_slice_view_build(*a, bs.selection.step);
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

            // 33 R6 T1: the PRODUCED `blame` event is decoded, and its cone is
            // EXACTLY the backward slice the consumer would compute — the
            // producer and the client-side slicer agree.
            check("blame/event decoded", a->blame.size() == 1,
                  "one blame event expected");
            if (a->blame.size() == 1) {
                const BlameAttr &bl = a->blame[0];
                dt_slice want =
                    dt_slice_backward(a->df.edges, a->df.nsteps, bl.step);
                check("blame/cone == backward slice", bl.cone == want.steps,
                      "the emitted cone must equal asmtest_slice_backward");
                check("blame/traced value is not born_untraced",
                      !bl.born_untraced, "step 3 has traced producers");
            }
        }
    }

    // --- 33 R6 T1: the PRODUCED blame goldens carry a faithful cone -----------
    // blame-df-chain: a fully-traced value chain (born_untraced false, the cone
    // is the whole backward slice). blame-untraced: identity(a)=a, whose value
    // came from an ARGUMENT — no traced producer, so the cone is the sink ALONE
    // and born_untraced fires. The fidelity distinction the socket renders.
    {
        std::string err;
        auto chain = load_recording_file(gd("blame-df-chain.asmtrace"), err);
        check("blame-golden/chain opens", chain.has_value(), err.c_str());
        if (chain) {
            Streams s = decode_streams(*chain);
            check("blame-golden/chain has one blame", s.blame.size() == 1,
                  "one blame event");
            if (s.blame.size() == 1) {
                const BlameAttr &bl = s.blame[0];
                check("blame-golden/chain is traced", !bl.born_untraced,
                      "the value has traced producers");
                check("blame-golden/chain cone is the full slice",
                      bl.cone ==
                          dt_slice_backward(s.df.edges, s.df.nsteps, bl.step)
                              .steps,
                      "the emitted cone must equal the backward slice");
                check("blame-golden/chain cone is more than the sink",
                      bl.cone.size() > 1, "a traced chain has ancestors");
            }
        }
        auto un = load_recording_file(gd("blame-untraced.asmtrace"), err);
        check("blame-golden/untraced opens", un.has_value(), err.c_str());
        if (un) {
            Streams s = decode_streams(*un);
            check("blame-golden/untraced has one blame", s.blame.size() == 1,
                  "one blame event");
            if (s.blame.size() == 1) {
                const BlameAttr &bl = s.blame[0];
                check("blame-golden/untraced is born_untraced",
                      bl.born_untraced,
                      "an argument-derived value has no traced producer");
                check("blame-golden/untraced cone is the sink alone, non-empty",
                      bl.cone.size() == 1 && bl.cone[0] == bl.step,
                      "provenance starts at instrumentation — never an empty "
                      "cone");
            }
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
    // a codeimage-less recording takes the faithful "no regions" placard.
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
        // 36 T4: the LIVE dataflow shape — absolute codeimage + region-relative
        // df_step, NO trace. Before 36 this opened onto an empty, unlabelled
        // plane; now the terrain draws a df_step residency rung anchored to the
        // span and the trajectory anchors the offsets onto it.
        int igd = shell_open(s3, gd("scene-df-loop.asmtrace"), err);
        check("scene/df-loop opened", igd >= 0, err.c_str());
        // 37 T6: the TWO-span shape 36 must refuse and 37 resolves from the wire.
        int igd2 = shell_open(s3, gd("scene-df-two-span.asmtrace"), err);
        check("scene/df-two-span opened", igd2 >= 0, err.c_str());

        // Parallel to the workspace, like every other per-recording vector.
        check("scene/scenes parallel",
              s3.scenes.size() == s3.ws.recordings.size(),
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
                draw_scene_overview(
                    s3, s3.ws.recordings[static_cast<size_t>(igs)], *a);
            ImGui::End();
            ImGui::Render();

            const SceneView &sv = s3.scenes[static_cast<size_t>(igs)];
            check("scene/models woven", sv.built,
                  "the pane must build the models");
            check("scene/regions placed", sv.has_regions,
                  "a codeimage recording must place code regions on the plane");
            check("scene/terrain sized", sv.terr.w > 0 && sv.terr.h > 0,
                  "the terrain plane must be sized");
            check("scene/abs terrain has heat", !sv.terr.code.empty(),
                  "the abs trace must populate code cells");
            check("scene/exact trajectory",
                  !sv.traj.refused() && !sv.traj.trajectories.empty() &&
                      sv.traj.basis == "abs",
                  "an abs recording must weave one exact trajectory");
            check("scene/slice cut at the playhead", sv.slice_t == sv.hud.t,
                  "the terrain slice must track the HUD playhead");

            // 36 T4: the abs golden is fully placed — no placement chip fires
            // for it (all its chip branches are for rel/df/unplaced recordings).
            auto abs_chips = scene3d::placement_chips(sv.terr, sv.traj);
            check("scene/abs draws no placement chip (fully placed abs path)",
                  abs_chips.empty(),
                  "an abs recording raised a placement chip");
        }

        // 36 T4: drive the pane for the LIVE dataflow golden and assert the df
        // shape renders end to end — a non-empty terrain from the df_step rung
        // and a fully-anchored PC path — where before 36 the tab was mute.
        if (igd >= 0) {
            s3.active_tab = igd;
            const Streams *a = shell_a(s3);
            check("scene/df stream decoded", a != nullptr, "no stream");
            ImGui::NewFrame();
            ImGui::Begin("t3df");
            if (a != nullptr)
                draw_scene_overview(
                    s3, s3.ws.recordings[static_cast<size_t>(igd)], *a);
            ImGui::End();
            ImGui::Render();

            const SceneView &sv = s3.scenes[static_cast<size_t>(igd)];
            check("scene/df regions placed", sv.has_regions,
                  "a codeimage recording must place code regions on the plane");
            check("scene/df terrain has steps", sv.terr.nsteps > 0,
                  "the df_step stream is a real time axis");
            check("scene/df height source is df_step",
                  sv.terr.height_source == "df_step",
                  ("got '" + sv.terr.height_source + "'").c_str());
            check("scene/df terrain has a residency rung",
                  !sv.terr.code.empty(),
                  "the df_step rung must populate code cells");
            check("scene/df path is rel and anchored",
                  sv.traj.basis == "rel" && sv.traj.anchored,
                  "the df PC path must anchor to the single span");
            check("scene/df path fully placed",
                  sv.traj.pc_placed == sv.traj.pc_points &&
                      sv.traj.pc_points > 0,
                  (std::to_string(sv.traj.pc_placed) + "/" +
                   std::to_string(sv.traj.pc_points))
                      .c_str());

            // The fidelity chrome: the df capture raises the residency chip and —
            // now that 37 T1 tags scene-df-loop's df_step with rbase — the WIRE
            // placement chip (not the derived-placement one), and NEITHER "NOT
            // PLACED" refusal. Deleting a chip branch fails one of these checks.
            check("scene/df anchor_source is wire (rbase-tagged golden)",
                  sv.traj.anchor_source == "wire",
                  ("got '" + sv.traj.anchor_source + "'").c_str());
            auto chips = scene3d::placement_chips(sv.terr, sv.traj);
            bool df_residency = false, wire = false, not_placed = false;
            for (const scene3d::PlacementChip &c : chips) {
                if (c.text.find("single-step residency (df_step)") !=
                    std::string::npos)
                    df_residency = true;
                if (c.text.find("stated on the wire (rbase)") !=
                    std::string::npos)
                    wire = true;
                if (c.text.find("NOT PLACED") != std::string::npos)
                    not_placed = true;
            }
            check("scene/df chip: single-step residency (df_step)",
                  df_residency,
                  "the df height rung must label itself, not claim coverage");
            check("scene/df chip: wire placement (rbase stated on the wire)",
                  wire,
                  "a wire-tagged df capture must say its span is wire-stated");
            check("scene/df chip: nothing refused as NOT PLACED", !not_placed,
                  "a placed df capture must raise no NOT PLACED refusal");
        }

        // 37 T6: the TWO-span golden renders a placed path where 36 alone renders
        // a labelled-empty plane — the end-to-end proof rbase resolves a multi-
        // span capture that the single-codeimage anchor must refuse.
        if (igd2 >= 0) {
            s3.active_tab = igd2;
            const Streams *a = shell_a(s3);
            check("scene/two-span stream decoded", a != nullptr, "no stream");
            ImGui::NewFrame();
            ImGui::Begin("t3two");
            if (a != nullptr)
                draw_scene_overview(
                    s3, s3.ws.recordings[static_cast<size_t>(igd2)], *a);
            ImGui::End();
            ImGui::Render();

            const SceneView &sv = s3.scenes[static_cast<size_t>(igd2)];
            // Two codeimage spans ⇒ resolve_anchor alone refuses; rbase resolves.
            check("scene/two-span: two code regions on the plane",
                  sv.terr.proj.regions.size() == 2,
                  "the two codeimage spans must both place regions");
            check("scene/two-span: path anchored from the WIRE (not refused)",
                  sv.traj.anchored && sv.traj.anchor_source == "wire",
                  ("anchored=" + std::to_string(sv.traj.anchored) + " src='" +
                   sv.traj.anchor_source + "'")
                      .c_str());
            check("scene/two-span: every vertex placed (both spans)",
                  sv.traj.pc_placed == sv.traj.pc_points &&
                      sv.traj.pc_points > 0,
                  (std::to_string(sv.traj.pc_placed) + "/" +
                   std::to_string(sv.traj.pc_points))
                      .c_str());
            check("scene/two-span: the df rung placed residency cells",
                  sv.terr.height_source == "df_step" && !sv.terr.code.empty(),
                  "the multi-span df rung must place cells via rbase");
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
                draw_scene_overview(
                    s3, s3.ws.recordings[static_cast<size_t>(imt)], *a);
            ImGui::End();
            ImGui::Render();
            const SceneView &sv = s3.scenes[static_cast<size_t>(imt)];
            check(
                "scene/no-regions is faithful", sv.built && !sv.has_regions,
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

    // 36 T4: placement_chips is PURE, so each fidelity-chrome branch is asserted
    // directly (no ImGui frame needed) — deleting any one branch fails a named
    // check here, which the golden-driven cases above cannot fully cover (no
    // committed golden refuses placement; that is 37 T6's two-span golden).
    {
        using asmdesk::space::TerrainModel;
        using asmdesk::space::TrajectorySet;
        auto has = [](const std::vector<scene3d::PlacementChip> &cs,
                      const char *sub, scene3d::PlacementChip::Sev sev) {
            for (const auto &c : cs)
                if (c.sev == sev && c.text.find(sub) != std::string::npos)
                    return true;
            return false;
        };
        { // HEIGHTS NOT PLACED (terrain anchor refused) + df residency label.
            TerrainModel t;
            t.anchor_error = "two or more codeimage code spans";
            t.height_source = "df_step";
            auto cs = scene3d::placement_chips(t, TrajectorySet{});
            check("chip/HEIGHTS NOT PLACED",
                  has(cs, "HEIGHTS NOT PLACED", scene3d::PlacementChip::Bad),
                  "anchor_error must raise HEIGHTS NOT PLACED");
            check("chip/df residency label",
                  has(cs, "single-step residency (df_step)",
                      scene3d::PlacementChip::Warn),
                  "height_source df_step must label the rung");
        }
        { // PATH NOT PLACED (nothing of the path projects).
            TrajectorySet tr;
            tr.pc_points = 4;
            tr.pc_placed = 0;
            check("chip/PATH NOT PLACED",
                  has(scene3d::placement_chips(TerrainModel{}, tr),
                      "PATH NOT PLACED", scene3d::PlacementChip::Bad),
                  "pc_placed==0 with points must raise PATH NOT PLACED");
        }
        { // K of N off-plane (partial placement — the 4096-byte clamp).
            TrajectorySet tr;
            tr.pc_points = 4;
            tr.pc_placed = 3;
            check("chip/K of N off-plane",
                  has(scene3d::placement_chips(TerrainModel{}, tr), "off-plane",
                      scene3d::PlacementChip::Warn),
                  "a partial placement must raise the K of N chip");
        }
        { // fully anchored rel path, single-span (36) -> derived-placement chip.
            TrajectorySet tr;
            tr.pc_points = 4;
            tr.pc_placed = 4;
            tr.anchored = true;
            tr.anchor_source = "single-span";
            check("chip/derived placement (single-span)",
                  has(scene3d::placement_chips(TerrainModel{}, tr),
                      "anchored to the codeimage span",
                      scene3d::PlacementChip::Warn),
                  "a single-span anchor must say derived placement");
        }
        { // 37 T5: a WIRE-stated placement is a distinct, stronger claim.
            TrajectorySet tr;
            tr.pc_points = 4;
            tr.pc_placed = 4;
            tr.anchored = true;
            tr.anchor_source = "wire";
            check(
                "chip/wire placement (rbase)",
                has(scene3d::placement_chips(TerrainModel{}, tr),
                    "stated on the wire (rbase)", scene3d::PlacementChip::Warn),
                "a wire-stated placement must say so, distinctly from derived");
        }
        { // 37 T5: a MIXED recording names both mechanisms.
            TrajectorySet tr;
            tr.pc_points = 4;
            tr.pc_placed = 4;
            tr.anchored = true;
            tr.anchor_source = "mixed";
            check("chip/mixed placement (wire + anchor)",
                  has(scene3d::placement_chips(TerrainModel{}, tr), "(mixed)",
                      scene3d::PlacementChip::Warn),
                  "a mixed recording must name both mechanisms");
        }
        { // fully placed ABS path -> no chip at all.
            TrajectorySet tr;
            tr.pc_points = 4;
            tr.pc_placed = 4;
            tr.anchored = false; // abs: placed but not anchored
            check("chip/abs raises no chip",
                  scene3d::placement_chips(TerrainModel{}, tr).empty(),
                  "a fully placed abs path must raise no placement chip");
        }
        // 36 T4 defect 1 (review): the BASIS chip is now pure, so its fallback is
        // testable — reverting the `: traj.basis` fallback fails the df-only case.
        { // abs canvas basis -> the abs chip.
            TerrainModel t;
            t.basis = "abs";
            auto bc = scene3d::basis_chip(t, TrajectorySet{});
            check("chip/basis abs",
                  bc.sev == scene3d::PlacementChip::Ok &&
                      bc.text.find("abs: true address-space path") !=
                          std::string::npos,
                  bc.text.c_str());
        }
        { // rel canvas basis -> the rel chip.
            TerrainModel t;
            t.basis = "rel";
            auto bc = scene3d::basis_chip(t, TrajectorySet{});
            check("chip/basis rel",
                  bc.sev == scene3d::PlacementChip::Ok &&
                      bc.text.find("routine-relative") != std::string::npos,
                  bc.text.c_str());
        }
        { // THE defect-1 case: a df-only recording has an EMPTY canvas basis, so
            // the basis chip MUST fall back to the trajectory's basis (rel).
            TerrainModel t; // t.basis == "" (df-only: no trace canvas)
            TrajectorySet tr;
            tr.basis = "rel";
            auto bc = scene3d::basis_chip(t, tr);
            check(
                "chip/basis df-only falls back to traj.basis (25 T6 rel chip)",
                bc.sev == scene3d::PlacementChip::Ok &&
                    bc.text.find("routine-relative") != std::string::npos,
                ("reverting the traj.basis fallback drops the rel chip: '" +
                 bc.text + "'")
                    .c_str());
        }
        { // mixed basis -> the refusal chip.
            TerrainModel t;
            t.basis_error = "mixed";
            auto bc = scene3d::basis_chip(t, TrajectorySet{});
            check("chip/basis mixed -> EXACT TERRAIN REFUSED",
                  bc.sev == scene3d::PlacementChip::Bad &&
                      bc.text.find("EXACT TERRAIN REFUSED") !=
                          std::string::npos,
                  bc.text.c_str());
        }
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
        iod.IniFilename =
            nullptr; // still file-free — DockBuilder driven in-proc
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
            ImGuiID nleft = ImGui::DockBuilderSplitNode(c, ImGuiDir_Left, 0.18f,
                                                        nullptr, &c);
            ImGuiID nright = ImGui::DockBuilderSplitNode(c, ImGuiDir_Right,
                                                         0.24f, nullptr, &c);
            ImGuiID nrightb = ImGui::DockBuilderSplitNode(
                nright, ImGuiDir_Down, 0.5f, nullptr, &nright);
            ImGuiID nbot = ImGui::DockBuilderSplitNode(c, ImGuiDir_Down, 0.30f,
                                                       nullptr, &c);
            ImGuiID nbot2 = ImGui::DockBuilderSplitNode(nbot, ImGuiDir_Right,
                                                        0.5f, nullptr, &nbot);
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

        // T1 fidelity — the placards survive the move into the panes (D7). Switch
        // the active recording to the producer-absent min-trace and drive frames;
        // the scrubber pane is active (so it drew) while its regstate producer is
        // absent — it drew the placard, not a register file of zeros.
        ds.active_tab = imin;
        frame(ds);
        frame(ds);
        check(
            "dock/scrubber placard producer-absent",
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
        check(
            "dock/preset moves timeline", dockid(kPaneTimeline) != tl_ri,
            "a preset switch must move the timeline pane to a different node");
        check(
            "dock/preset moves scrubber", dockid(kPaneScrubber) != sc_ri,
            "a preset switch must move the scrubber pane to a different node");
        // Reset layout (the menu's own call) restores the default assignment.
        apply_preset(ds, LayoutPreset::ReplayInspect);
        check("dock/reset restores timeline", dockid(kPaneTimeline) == tl_ri,
              "Reset must restore the timeline pane's node");
        check("dock/reset restores scrubber", dockid(kPaneScrubber) == sc_ri,
              "Reset must restore the scrubber pane's node");

        // Pane visibility (open-on-demand + close + reopen). Entering Inspect
        // opens the Connect/Processes panes; the Live-capture pane stays hidden
        // until a host connects (CONTEXT gating, not user-close); closing the
        // scrubber pane hides it, and reopening it (what View ▸ Panels does) brings
        // it back — a recording is still open, so its context is met.
        ds.show_inspect = true;
        ds.pane_open[kPaneConnect] = true;
        ds.pane_open[kPaneProcesses] = true;
        ds.pane_open[kPaneCapture] = true; // but no serve host is connected
        frame(ds);
        frame(ds);
        check("pane/inspect opens on demand",
              active(kPaneConnect) && active(kPaneProcesses),
              "entering Inspect must show the Connect + Processes panes");
        check("pane/capture needs a host", !active(kPaneCapture),
              "Live capture must stay hidden until a serve host connects");
        ds.pane_open[kPaneScrubber] = false;
        frame(ds);
        frame(ds);
        check("pane/close hides", !active(kPaneScrubber),
              "closing the scrubber pane (its X) must hide it");
        ds.pane_open[kPaneScrubber] = true;
        frame(ds);
        frame(ds);
        check("pane/menu reopens", active(kPaneScrubber),
              "reopening (View ▸ Panels) must bring the scrubber pane back");

        // The Live-capture pane is a per-target control surface: a connected host
        // is necessary but not sufficient — it stays hidden until a process is
        // picked (selected_pid), so Capture mode lands on the target picker alone.
        ds.inspect.host_started = true;
        ds.pane_open[kPaneLog] = true;
        ds.pane_open[kPaneCapture] = true;
        frame(ds);
        frame(ds);
        check("pane/capture needs a selected process",
              !active(kPaneCapture),
              "Live capture must stay hidden with a host but no process picked");
        // Render the split-out capture panes with a host up AND a target selected,
        // so ImGui validates their Begin / EndChild / EndDisabled balance headlessly
        // (an imbalance aborts via IM_ASSERT — asserts are on, the build has no
        // -DNDEBUG). The Log draws its scrollback child + the moved session-status
        // block; the Live-capture pane draws the reordered patch bay + the 3D handoff.
        ds.inspect.selected_pid = 4242;
        frame(ds);
        frame(ds);
        check("pane/log renders with a host", active(kPaneLog),
              "the Log pane shows (and draws cleanly) once a host connects");
        check("pane/capture renders with a host and a target",
              active(kPaneCapture),
              "the Live-capture controls show once a host connects and a "
              "process is picked");
        // Tree mode reveals the tree-filter editor INSIDE the patch bay (it lives
        // beside the mode picker now, not in a separate control): render a frame in
        // that mode so ImGui validates the added widgets' scope balance too.
        ds.inspect.want = LiveMode::Tree;
        frame(ds);
        frame(ds);
        check("pane/capture renders the tree filter in tree mode",
              active(kPaneCapture),
              "the patch bay's tree-filter editor must draw cleanly for a tree "
              "capture");

        // The PT slice is its OWN pane now, CONTEXT-GATED to an Intel PT host
        // (pctx_pt) — not a host connection but a fact about the CPU. Opening it
        // must show it IFF this host can start a live PT capture, both directions:
        // on the usual non-PT test host that means hidden, and on a PT host it
        // shows. Asserting against inspect_pt_host_available() keeps the check
        // correct on either. The pure PT-host verdict itself is proved with
        // synthetic facts in test_obs_ptslice — no PT silicon needed there.
        ds.pane_open[kPanePtSlice] = true;
        frame(ds);
        frame(ds);
        check("pane/pt-slice tracks the PT-host gate",
              active(kPanePtSlice) == inspect_pt_host_available(),
              "the PT slice pane must show exactly when the host can capture PT");
        ds.inspect.host_started = false;
        ds.inspect.selected_pid = 0;

        // Its BODY draws cleanly everywhere (it self-gates internally with the
        // disclosure + a disabled Replay button), independent of the host gate —
        // render it in a plain window so ImGui validates its Begin / EndDisabled
        // balance headlessly. This is the coverage that used to ride the capture
        // pane before the PT slice moved out; an imbalance aborts via IM_ASSERT.
        ImGui::NewFrame();
        if (ImGui::Begin("pt-slice-body-probe"))
            draw_pt_slice_pane(ds.inspect);
        ImGui::End();
        ImGui::Render();

        ImGui::DestroyContext();
    }

    // --- 18-T3: the Author save-guard (dirty / close / title), pure model ---
    // No ImGui context needed: the guard seams are pure ShellState moves (F24).
    // A clean recording closes on the spot; a DIRTY (authored + unsaved) one
    // raises the save/discard/cancel guard instead of erasing; save clears dirty
    // and it then closes; discard erases; the Author door tab has its own guard.
    {
        ShellState cs;
        std::string err;
        int i0 = shell_open(cs, fx("min-trace.asmtrace"), err);
        int i1 = shell_open(cs, fx("dropped.asmtrace"), err);
        check("t3/opened", i0 >= 0 && i1 >= 0, err.c_str());
        size_t n = cs.ws.recordings.size();

        // A clean recording closes immediately, no guard.
        cs.ws.recordings[static_cast<size_t>(i0)].dirty = false;
        shell_request_close(cs, static_cast<size_t>(i0));
        check("t3/clean-closes-immediately",
              cs.ws.recordings.size() == n - 1 && cs.close_pending == -1,
              "a clean recording must close without a prompt");

        // The remaining recording, marked dirty, must NOT be erased on a close
        // request — the guard is raised instead.
        cs.ws.recordings[0].dirty = true;
        shell_request_close(cs, 0);
        check("t3/dirty-not-erased", cs.ws.recordings.size() == n - 1,
              "a dirty close must not silently drop the recording");
        check("t3/dirty-raises-guard", cs.close_pending == 0,
              "a dirty close must raise the save/discard/cancel guard");

        // Cancel keeps the recording and drops the guard.
        shell_cancel_close(cs);
        check("t3/cancel-keeps",
              cs.close_pending == -1 && cs.ws.recordings.size() == n - 1,
              "cancel must keep it open");

        // A save clears dirty; the same close then completes cleanly.
        cs.ws.recordings[0].dirty = false; // stand-in for a successful save
        shell_request_close(cs, 0);
        check("t3/saved-closes-clean",
              cs.ws.recordings.size() == n - 2 && cs.close_pending == -1,
              "a saved recording closes without a guard");

        // Discard erases a dirty recording after the guard is raised.
        int j0 = shell_open(cs, fx("min-trace.asmtrace"), err);
        cs.ws.recordings[static_cast<size_t>(j0)].dirty = true;
        shell_request_close(cs, static_cast<size_t>(j0));
        check("t3/discard-guard", cs.close_pending == j0,
              "a dirty close raises the guard");
        size_t before = cs.ws.recordings.size();
        shell_discard_close(cs);
        check("t3/discard-erases",
              cs.ws.recordings.size() == before - 1 && cs.close_pending == -1,
              "discard must erase the entry");

        // The Author door tab's own guard: a dirty run raises it rather than
        // closing; a clean tab closes on request.
        cs.show_author = true;
        cs.author.dirty = true;
        bool closed = shell_request_author_close(cs);
        check("t3/author-dirty-guard",
              !closed && cs.author_close_guard && cs.show_author,
              "a dirty Author tab must raise the guard, not close");
        cs.author_close_guard = false;
        cs.author.dirty = false;
        closed = shell_request_author_close(cs);
        check("t3/author-clean-closes", closed && !cs.show_author,
              "a clean Author tab closes on request");
    }

    // --- 20 T2: auto-land Learn on empty + mode drives pending_preset -------
    {
        ShellState es;
        check("20t2/auto-land-learn", es.mode == Mode::Learn,
              "an empty workspace must auto-land in Learn (the dependency-free "
              "path)");
        check("20t2/no-preset-pending", !es.pending_preset.has_value(),
              "a fresh ShellState requests no perspective yet");
        // Selecting each mode sets mode + the matching pending_preset (the seam
        // the docked frame applies — asserted here, not the DockBuilder tree).
        shell_select_mode(es, Mode::Author);
        check("20t2/author-mode", es.mode == Mode::Author, "mode must update");
        check("20t2/author-preset",
              es.pending_preset.value_or(LayoutPreset::LiveObserver) ==
                  LayoutPreset::Author,
              "Author mode requests the Author perspective");
        shell_select_mode(es, Mode::Capture);
        check("20t2/capture-preset",
              es.pending_preset.value_or(LayoutPreset::Author) ==
                  LayoutPreset::LiveObserver,
              "Capture mode requests the LiveObserver perspective");
        // Capture lands on the Processes pane and asks draw_shell to auto-connect
        // from the saved Settings, rather than forcing the Connect pane open (it
        // stays a fallback, revealed only if the connect fails).
        check("20t2/capture-opens-processes", es.pane_open[kPaneProcesses],
              "Capture must open the Processes pane");
        check("20t2/capture-autoconnects", es.inspect.want_autoconnect,
              "Capture must request an auto-connect from saved Settings");
        check("20t2/capture-focuses-processes", es.inspect.want_focus_processes,
              "Capture must ask draw_shell to focus the Processes pane");
        check("20t2/capture-no-forced-connect", !es.pane_open[kPaneConnect],
              "Capture must not force the Connect pane open");
        shell_select_mode(es, Mode::Learn);
        check("20t2/learn-preset",
              es.pending_preset.value_or(LayoutPreset::LiveObserver) ==
                  LayoutPreset::ReplayInspect,
              "Learn mode requests the ReplayInspect perspective");
    }

    // --- 20 T1: the view set flips with the recording's data ----------------
    {
        // min-trace: a bare recording — Summary/Canvas/Timeline present; Loom /
        // Scrubber / 3D absent, EACH with a non-empty machine reason (D7).
        ShellState vs;
        std::string err;
        int im = shell_open(vs, fx("min-trace.asmtrace"), err);
        check("20t1/min opened", im >= 0, err.c_str());
        vs.active_tab = im;
        const Streams *a = shell_a(vs);
        check("20t1/min decoded", a != nullptr, "no stream");
        if (a != nullptr) {
            const StepIndex &si = vs.stepidx[static_cast<size_t>(im)];
            const ObserverState &obs = vs.observers[static_cast<size_t>(im)];
            auto vp = view_presence(*a, obs, si,
                                    vs.ws.recordings[static_cast<size_t>(im)],
                                    Mode::Open, /*b_attachable=*/false);
            auto find = [&](ViewId id) -> const ViewPresence * {
                for (const ViewPresence &e : vp)
                    if (e.id == id)
                        return &e;
                return nullptr;
            };
            check("20t1/summary present",
                  find(ViewId::Summary) && find(ViewId::Summary)->present,
                  "Summary is always present");
            check("20t1/canvas present",
                  find(ViewId::Canvas) && find(ViewId::Canvas)->present,
                  "Canvas is in the lean default");
            check("20t1/timeline present",
                  find(ViewId::Timeline) && find(ViewId::Timeline)->present,
                  "Timeline is in the lean default");
            // The reveal-when-present views are absent for a bare recording, and
            // each names WHY (never an empty "unavailable").
            for (ViewId id :
                 {ViewId::Loom, ViewId::Scrubber, ViewId::Scene3D}) {
                const ViewPresence *e = find(id);
                check("20t1/absent", e && !e->present,
                      "a reveal view must be absent for a bare recording");
                check("20t1/absent-named", e && !e->reason.empty(),
                      "an absent view must carry a non-empty machine reason");
            }
            // The affordance count equals the number of absent entries.
            size_t nabs = view_absent_count(vp);
            size_t counted = 0;
            for (const ViewPresence &e : vp)
                if (!e.present)
                    counted++;
            check("20t1/absent-count", nabs == counted && nabs > 0,
                  "unavailable-views (N) must equal the absent entries");
        }

        // A codeimage-bearing recording flips 3D to PRESENT (the same gate the
        // scene pane reads).
        ShellState cs;
        int ic = shell_open(cs, gd("scene-abs-loop.asmtrace"), err);
        check("20t1/codeimage opened", ic >= 0, err.c_str());
        cs.active_tab = ic;
        const Streams *ca = shell_a(cs);
        if (ca != nullptr) {
            auto vp = view_presence(*ca, cs.observers[static_cast<size_t>(ic)],
                                    cs.stepidx[static_cast<size_t>(ic)],
                                    cs.ws.recordings[static_cast<size_t>(ic)],
                                    Mode::Open, false);
            bool scene = false;
            for (const ViewPresence &e : vp)
                if (e.id == ViewId::Scene3D)
                    scene = e.present;
            check("20t1/codeimage flips 3D present", scene,
                  "a codeimage recording must make the 3D overview present");
        }
    }

    // --- 20 T3: the workspace round-trips through capture -> restore ---------
    {
        ShellState a;
        std::string err;
        int i0 = shell_open(a, fx("min-trace.asmtrace"), err);
        int i1 = shell_open(a, fx("dropped.asmtrace"), err);
        check("20t3/opened two", i0 >= 0 && i1 >= 0, err.c_str());
        a.active_tab = i1;
        a.selection.step = 2;
        a.view = dt_view::timeline;
        // Snapshot to a WorkspaceState, serialise + parse, then restore into a
        // fresh shell: the same open set and active recording come back.
        WorkspaceState ws = shell_capture_workspace(a);
        check("20t3/capture open set", ws.open.size() == 2,
              "both recordings must be captured");
        check("20t3/capture recents", ws.recents.size() == 2,
              "both opens land in recents");
        std::string json = workspace_state_serialize(ws);
        WorkspaceState parsed;
        check("20t3/round-trips", workspace_state_parse(json, parsed),
              "the serialised store must parse");
        check("20t3/open set stable", parsed.open == ws.open,
              "the open set must survive serialise->parse");

        ShellState b;
        shell_restore_workspace(b, parsed);
        check("20t3/restore reopens", b.ws.recordings.size() == 2,
              "restore must reopen both recordings");
        check("20t3/restore active",
              b.active_tab == 1 && b.selection.step.value_or(999) == 2,
              "restore must land on the prior active recording + step");

        // A vanished recording is KEPT in recents with an error, never dropped.
        WorkspaceState gone;
        gone.open.push_back(std::string(ASMTEST_FIXTURE_DIR) +
                            "/does-not-exist.asmtrace");
        ShellState c;
        shell_restore_workspace(c, gone);
        check("20t3/vanished kept in recents", c.recents.size() == 1,
              "a vanished recording must stay in recents");
        check("20t3/vanished names the error", !c.status.empty(),
              "a vanished recording must surface its load error (D7)");
    }

    // --- 25-live-model-wiring.md T1/T2/T4/T7: the live capture as a workspace
    // tab. A fed `dataflow` session (exact provenance + df_step, no `end` ->
    // growing) must be promoted into ws.recordings with its full parallel model,
    // so view_presence offers the SAME Loom / Slice a replayed file would —
    // while the Scrubber stays faithfully absent (no live regstate producer) and
    // the ephemeral live tab is never persisted. Pure model: no ImGui context.
    {
        static const char *kExactHeader =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},)"
            R"("provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"})";
        ShellState ls;
        ls.mode = Mode::Capture; // the live-capture posture (Observer offered)
        LiveSession &sess = ls.inspect.session;
        sess.feed_line(R"({"k":"cmd","cmd":"start","mode":"dataflow"})");
        sess.feed_line(
            R"({"k":"session","state":"started","mode":"dataflow","pid":1234,"params":{}})");
        sess.feed_line(kExactHeader);

        // Header only, no df_step yet: the tab exists but Loom is absent — proof
        // the gate is data-driven, not a live-vs-replay flag.
        shell_sync_live_tab(ls);
        check("25/live tab created", ls.live_tab >= 0,
              "a growing capture must be promoted into ws.recordings");
        check("25/live is the only tab", ls.ws.recordings.size() == 1,
              "one live tab, no file tabs");
        check("25/live tab auto-selected", ls.active_tab == ls.live_tab,
              "the panes must land on the live capture from Home");
        {
            size_t i = static_cast<size_t>(ls.live_tab);
            auto vp =
                view_presence(ls.streams[i], ls.observers[i], ls.stepidx[i],
                              ls.ws.recordings[i], ls.mode, false, true);
            const ViewPresence *loom = find_view(vp, ViewId::Loom);
            check("25/no df -> Loom absent", loom && !loom->present,
                  "the Loom needs df_step values it does not have yet");
        }

        // Feed df_step: the exact-dataflow views must light live. This live
        // session did NOT arm --steps (no regstate), so the Scrubber stays absent
        // — but now with the LIVE absent-reason (26 T4), distinct from a saved
        // emulator recording's.
        sess.feed_line(
            R"({"k":"df_step","step":0,"off":0,"disasm":"mov eax, edi","ops":[{"space":"reg","reg":35,"size":4,"write":true,"value_valid":true,"value":40}]})");
        sess.feed_line(
            R"({"k":"df_step","step":1,"off":2,"disasm":"add eax, esi","ops":[{"space":"reg","reg":35,"size":4,"write":true,"value_valid":true,"value":42}]})");
        uint64_t built_before = ls.live_built_events;
        shell_sync_live_tab(ls);
        check("25/rebuild on growth", ls.live_built_events != built_before,
              "growth must move the built-event watermark and re-decode");
        {
            size_t i = static_cast<size_t>(ls.live_tab);
            check(
                "25/df decoded live", ls.streams[i].df.present(),
                "the live tab's df stream must decode the fed df_step events");
            auto vp =
                view_presence(ls.streams[i], ls.observers[i], ls.stepidx[i],
                              ls.ws.recordings[i], ls.mode, false, true);
            const ViewPresence *loom = find_view(vp, ViewId::Loom);
            const ViewPresence *slice = find_view(vp, ViewId::Slice);
            const ViewPresence *scrub = find_view(vp, ViewId::Scrubber);
            check("25/Loom live", loom && loom->present,
                  "an exact dataflow capture must weave a Loom live");
            check("25/Slice live", slice && slice->present,
                  "df_step present -> the slice explorer must be offered live");
            check("25/Scrubber absent without --steps",
                  scrub && !scrub->present,
                  "this live capture did not arm --steps -> no regstate ring");
            check(
                "26/Scrubber live-no-steps reason",
                scrub &&
                    scrub->reason.find("live session") != std::string::npos &&
                    scrub->reason.find("--steps") != std::string::npos,
                "the live Scrubber's absent-reason must name the live session "
                "AND its --steps opt-in (26 T4)");
        }

        // 34 T2: the "View in 3D overview" handoff — from the picked process's
        // growing capture straight to its 3D tab. With a live tab up, the intent
        // jumps the active tab to it and requests the 3D inner view (want_view_id).
        {
            ls.inspect.want_scene = true;
            ls.want_open_tab = -1;
            ls.want_view_id.reset();
            shell_consume_scene_handoff(ls);
            check("34/handoff jumps to the live tab",
                  ls.want_open_tab == ls.live_tab,
                  "the handoff must select the live capture's outer tab");
            check(
                "34/handoff requests the 3D inner tab",
                ls.want_view_id.has_value() &&
                    *ls.want_view_id == ViewId::Scene3D,
                "want_view_id must name the 3D overview (no dt_view spelling)");
            check("34/handoff consumes the intent", !ls.inspect.want_scene,
                  "the one-frame intent must be cleared once honoured");
        }
        // The no-live-tab branch is faithful, never a silent no-op (D7): status set,
        // no tab jump. A fresh state (no session) is the "attach first" case.
        {
            ShellState empty;
            empty.inspect.want_scene = true;
            shell_consume_scene_handoff(empty);
            check("34/handoff no-tab sets status",
                  empty.want_open_tab < 0 && !empty.status.empty() &&
                      empty.status.find("attach") != std::string::npos,
                  "with no live tab the handoff explains, it does not silently "
                  "do nothing");
        }

        // T4: the ephemeral, path-less live tab is never written to the store.
        WorkspaceState wss = shell_capture_workspace(ls);
        check("25/live tab not persisted", wss.open.empty(),
              "the path-less live tab must not enter the persisted open set");

        // A completed session freezes the tab; reset() tears it down.
        sess.feed_line(
            R"({"k":"end","events":2,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        sess.feed_line(
            R"({"k":"session","state":"stopped","mode":"dataflow","events":2,"reason":"stop"})");
        shell_sync_live_tab(ls);
        check("25/completed capture kept",
              ls.live_tab >= 0 && ls.ws.recordings.size() == 1,
              "a stopped session's last recording stays as a frozen tab");
        sess.shutdown();
        sess.reset();
        shell_sync_live_tab(ls);
        check("25/teardown on reset",
              ls.live_tab == -1 && ls.ws.recordings.empty(),
              "reset() must drop the ephemeral live tab");
    }

    // --- 26 T5.3: a live --dataflow --steps capture lights the Scrubber ------
    // The live regstate producer (26) emits one `regstate` per df_step under the
    // user_regs@x86_64/sysv descriptor. Feeding them makes the live tab's
    // StepIndex present, so the Scrubber time-travels a LIVE capture — the last
    // live-vs-replay gap closed. Pure model (no ImGui): assert the StepIndex and
    // that view_presence now OFFERS the Scrubber.
    {
        static const char *kHdr =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},)"
            R"("provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"})";
        ShellState ls;
        ls.mode = Mode::Capture;
        LiveSession &sess = ls.inspect.session;
        sess.feed_line(R"({"k":"cmd","cmd":"start","mode":"dataflow"})");
        sess.feed_line(
            R"({"k":"session","state":"started","mode":"dataflow","pid":26,"params":{"steps":true}})");
        sess.feed_line(kHdr);
        // Interleaved df_step + regstate, exactly as the shared sink emits them.
        sess.feed_line(
            R"({"k":"df_step","step":0,"off":0,"disasm":"mov rax, rdi","ops":[{"space":"reg","reg":35,"size":8,"write":true,"value_valid":true,"value":6}]})");
        sess.feed_line(
            R"({"k":"regstate","desc":"user_regs@x86_64/sysv","values":{"rax":0,"rbx":0,"rcx":0,"rdx":0,"rsi":7,"rdi":6,"rbp":0,"rsp":140737488347000,"r8":0,"r9":0,"r10":0,"r11":0,"r12":0,"r13":0,"r14":0,"r15":0,"rip":94476548243590,"rflags":514}})");
        sess.feed_line(
            R"({"k":"df_step","step":1,"off":3,"disasm":"add rax, rsi","ops":[{"space":"reg","reg":35,"size":8,"write":true,"value_valid":true,"value":13}]})");
        sess.feed_line(
            R"({"k":"regstate","desc":"user_regs@x86_64/sysv","values":{"rax":6,"rbx":0,"rcx":0,"rdx":0,"rsi":7,"rdi":6,"rbp":0,"rsp":140737488347000,"r8":0,"r9":0,"r10":0,"r11":0,"r12":0,"r13":0,"r14":0,"r15":0,"rip":94476548243593,"rflags":514}})");
        shell_sync_live_tab(ls);
        check("26/live tab created", ls.live_tab >= 0,
              "a --steps live capture must be promoted like any other");
        size_t i = static_cast<size_t>(ls.live_tab);
        const StepIndex &si = ls.stepidx[i];
        check("26/Scrubber present live", si.present(),
              "a live regstate ring must make the Scrubber present");
        check("26/regstate descriptor faithful",
              si.desc == "user_regs@x86_64/sysv",
              "the live Scrubber must carry the ptrace-source descriptor id");
        check("26/two held steps", si.count() == 2,
              "two fed regstate events -> two seek-able steps");
        {
            const RegFile *s0 = si.at_step(0);
            const RegFile *s1 = si.at_step(1);
            const RegField *rax0 = s0 ? s0->find("rax") : nullptr;
            const RegField *rax1 = s1 ? s1->find("rax") : nullptr;
            check("26/rax time-travels",
                  rax0 && rax1 && rax0->value == 0 && rax1->value == 6,
                  "the register file must move step-to-step (rax 0 -> 6)");
        }
        {
            auto vp =
                view_presence(ls.streams[i], ls.observers[i], ls.stepidx[i],
                              ls.ws.recordings[i], ls.mode, false, true);
            const ViewPresence *scrub = find_view(vp, ViewId::Scrubber);
            check("26/Scrubber offered live", scrub && scrub->present,
                  "with a live regstate ring the Scrubber is a present view");
        }
        // The path-less live tab still never persists (26 keeps 25's guard).
        WorkspaceState wss = shell_capture_workspace(ls);
        check("26/live+steps not persisted", wss.open.empty(),
              "a --steps live tab is still ephemeral and path-less");
    }

    // --- 25 T3: dedup the live tab against save->reopen ----------------------
    // Once an ended session's capture is opened as a permanent saved FILE tab,
    // the ephemeral live tab of the same run must retire and not resurrect from
    // the still-present completed recording; a fresh session still promotes.
    {
        static const char *kHdr =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},)"
            R"("provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"})";
        ShellState ls;
        ls.mode = Mode::Capture;
        LiveSession &sess = ls.inspect.session;
        sess.feed_line(R"({"k":"cmd","cmd":"start","mode":"dataflow"})");
        sess.feed_line(
            R"({"k":"session","state":"started","mode":"dataflow","pid":7,"params":{}})");
        sess.feed_line(kHdr);
        sess.feed_line(
            R"({"k":"df_step","step":0,"off":0,"disasm":"nop","ops":[]})");
        sess.feed_line(
            R"({"k":"end","events":1,"truncated":false,"drops":{"lost":0,"throttled":false}})");
        sess.feed_line(
            R"({"k":"session","state":"stopped","mode":"dataflow","events":1,"reason":"stop"})");
        shell_sync_live_tab(ls);
        check("25t3/frozen tab",
              ls.live_tab >= 0 && ls.ws.recordings.size() == 1,
              "a completed capture is mirrored as a frozen tab until adopted");

        // Simulate "Open in Loom" adopting the capture as a permanent file tab.
        ls.live_dismissed_done = sess.recordings().size();
        shell_retire_live_tab(ls);
        check("25t3/retired on adopt",
              ls.live_tab == -1 && ls.ws.recordings.empty(),
              "adopting the capture must retire the ephemeral live tab");
        shell_sync_live_tab(ls);
        check(
            "25t3/no resurrection", ls.live_tab == -1,
            "the adopted (dismissed) completed recording must not re-promote");

        // A fresh session after disconnect still promotes (the watermark reset).
        sess.shutdown();
        sess.reset();
        shell_sync_live_tab(ls); // empty host -> clears the dedup watermark
        sess.feed_line(R"({"k":"cmd","cmd":"start","mode":"dataflow"})");
        sess.feed_line(
            R"({"k":"session","state":"started","mode":"dataflow","pid":8,"params":{}})");
        sess.feed_line(kHdr);
        sess.feed_line(
            R"({"k":"df_step","step":0,"off":0,"disasm":"nop","ops":[]})");
        shell_sync_live_tab(ls);
        check("25t3/fresh session re-promotes", ls.live_tab >= 0,
              "a new capture after disconnect must promote a fresh live tab");
    }

    // --- 25 T6: live 3D presence + the camera survives a live re-weave -------
    {
        static const char *kHdr =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy","version":"1.1.0"},)"
            R"("provenance":{"backend":"ptrace-dataflow","exact":true,"trust":"exact"},"arch":"x86_64"})";
        ShellState ls;
        ls.mode = Mode::Capture;
        LiveSession &sess = ls.inspect.session;
        sess.feed_line(R"({"k":"cmd","cmd":"start","mode":"dataflow"})");
        sess.feed_line(
            R"({"k":"session","state":"started","mode":"dataflow","pid":9,"params":{}})");
        sess.feed_line(kHdr);
        sess.feed_line(
            R"({"k":"codeimage","base":4198400,"len":8,"version":1,"when":1,"bytes":"f30f1efa554889e5"})");
        shell_sync_live_tab(ls);
        size_t i = static_cast<size_t>(ls.live_tab);
        {
            auto vp =
                view_presence(ls.streams[i], ls.observers[i], ls.stepidx[i],
                              ls.ws.recordings[i], ls.mode, false);
            const ViewPresence *s3d = find_view(vp, ViewId::Scene3D);
            check("25t6/3D live", s3d && s3d->present,
                  "a live capture carrying codeimage regions must offer 3D");
        }
        // Set a camera sentinel, grow the capture, and re-sync: the re-weave must
        // drop the woven geometry but keep the user's interactive camera.
        ls.scenes[i].cam.yaw = 2.5f;
        sess.feed_line(
            R"({"k":"df_step","step":0,"off":0,"disasm":"nop","ops":[]})");
        shell_sync_live_tab(ls);
        check("25t6/camera preserved", ls.scenes[i].cam.yaw == 2.5f,
              "a live 3D re-weave must keep the camera, not reset the orbit");
    }

    // --- fix 6: the syscall-filter undo is recording-SCOPED ------------------
    // (a96f64a) A Filter UndoCommand carries `filter_rec` (the recording id the
    // edit was made on); undo_apply restores onto THAT recording by id, never onto
    // whichever tab is active when Ctrl+Z is pressed. Before the fix undo_apply
    // wrote to s.observers[active_tab] unconditionally, so a tab switch redirected
    // the undo to the wrong recording — restoring nothing on A and clobbering B.
    {
        ShellState s;
        std::string err;
        int a = shell_open(s, gd("sum_via_rbx.asmtrace"), err);
        int b = shell_open(s, gd("add_signed.asmtrace"), err);
        check("fix6/opened two", a == 0 && b == 1, err.c_str());
        check("fix6/observers parallel", s.observers.size() == 2,
              "each open recording gets its own Observer deck");

        // The edit was made on A: A now shows "openat"; B has its OWN unrelated
        // filter "read" (so we can tell which deck a stray write lands on).
        std::snprintf(s.observers[0].syscall_filter,
                      sizeof s.observers[0].syscall_filter, "%s", "openat");
        std::snprintf(s.observers[1].syscall_filter,
                      sizeof s.observers[1].syscall_filter, "%s", "read");

        // The command record_filter_undo would push for that edit: scoped to A by
        // id, before="" (A's baseline), after="openat".
        s.active_tab = a;
        UndoCommand cmd;
        cmd.kind = UndoCommand::Kind::Filter;
        cmd.filter_rec =
            s.streams[static_cast<size_t>(a)].id; // A's id, NOT a tab
        cmd.filter_before = "";
        cmd.filter_after = "openat";
        s.undo.push(std::move(cmd));
        check("fix6/command carries the recording id",
              !s.undo.cmds.empty() &&
                  s.undo.cmds.back().filter_rec == s.streams[0].id,
              "the Filter command is stamped with recording A's id, not a tab "
              "idx");

        // Now switch to B and undo — the exact sequence that used to corrupt B.
        s.active_tab = b;
        const UndoCommand *un = s.undo.undo();
        check("fix6/undo pops the filter command", un != nullptr,
              "one on the stack");
        if (un)
            undo_apply(s, *un, /*redo=*/false);
        check("fix6/undo restores A by id",
              std::string(s.observers[0].syscall_filter).empty(),
              "Ctrl+Z after a tab switch must restore A's filter (to its "
              "baseline)");
        check(
            "fix6/undo leaves B untouched",
            std::string(s.observers[1].syscall_filter) == "read",
            "the active tab B's own filter must NOT be clobbered by A's undo");

        // Redo lands back on A too (by id), still not on B.
        const UndoCommand *re = s.undo.redo();
        check("fix6/redo replays the filter command", re != nullptr,
              "redoable");
        if (re)
            undo_apply(s, *re, /*redo=*/true);
        check("fix6/redo re-applies onto A by id",
              std::string(s.observers[0].syscall_filter) == "openat",
              "Ctrl+Y after a tab switch must re-apply A's filter onto A");
        check("fix6/redo still leaves B untouched",
              std::string(s.observers[1].syscall_filter) == "read",
              "redo must not touch the active tab B either");

        // A command naming a recording that has since been closed has nowhere to
        // land: undo_apply skips it rather than clobbering the active tab.
        UndoCommand gone;
        gone.kind = UndoCommand::Kind::Filter;
        gone.filter_rec = "closed-recording.asmtrace"; // not in s.streams
        gone.filter_before = "";
        gone.filter_after = "mmap";
        undo_apply(s, gone, /*redo=*/true);
        check("fix6/closed recording is a no-op, not a clobber",
              std::string(s.observers[1].syscall_filter) == "read" &&
                  std::string(s.observers[0].syscall_filter) == "openat",
              "an undo for a closed recording must not write to any live tab");
    }

    // --- 42: the Loom takes/take_views undo is recording-SCOPED, same shape as
    // fix 6 above. Unlike s.observers (a per-recording array), s.loom is a
    // SINGLE non-per-tab state that CLEARS OUTRIGHT on a tab switch (the
    // review's Bug B fix) rather than stashing a per-recording slot — so a
    // stale TakeSet command from a closed-over recording must be refused by
    // id (take_rec), never replayed onto whatever recording the Loom shows
    // now, or Ctrl+Z would resurrect recording A's takes onto B's canvas. ---
    {
        ShellState s;
        std::string err;
        int a = shell_open(s, gd("sum_via_rbx.asmtrace"), err);
        int b = shell_open(s, gd("add_signed.asmtrace"), err);
        check("42/opened two", a == 0 && b == 1, err.c_str());

        // The Loom is showing A; one take gets added there.
        s.loom.source_id = s.streams[static_cast<size_t>(a)].id;
        loom_take_node_t n;
        n.label = "arg0 := 11";
        loom_take_view_t v;
        v.node = n;
        UndoCommand cmd;
        cmd.kind = UndoCommand::Kind::TakeSet;
        cmd.take_rec = s.loom.source_id; // A's id, stamped at push time
        cmd.takes_before = {};
        cmd.take_views_before = {};
        s.loom.takes = {n};
        s.loom.take_views = {v};
        cmd.takes_after = s.loom.takes;
        cmd.take_views_after = s.loom.take_views;
        s.undo.push(std::move(cmd));

        // Switch the Loom to B — draw_loom's reset block would clear both
        // vectors outright here (Bug B's fix); simulate that directly since
        // this test links no ImGui draw path.
        s.loom.source_id = s.streams[static_cast<size_t>(b)].id;
        s.loom.takes.clear();
        s.loom.take_views.clear();

        // Ctrl+Z now must NOT resurrect A's take onto B's (correctly empty)
        // Loom state — the exact hazard a naive "just clear on switch" fix
        // would reopen via the undo stack.
        const UndoCommand *un = s.undo.undo();
        check("42/undo pops the take command", un != nullptr, "one on the stack");
        if (un)
            undo_apply(s, *un, /*redo=*/false);
        check("42/undo does not resurrect A's take onto B",
              s.loom.takes.empty() && s.loom.take_views.empty(),
              "a TakeSet command scoped to a closed-over recording (A) must "
              "not restore onto the Loom's current recording (B)");

        // Switch back to A and REDO the same command (un's cursor slot) — now
        // the id matches, so it must actually apply. Using redo (takes_after
        // = [the pushed take]) rather than re-undoing (takes_before = empty,
        // which would be indistinguishable from the mismatched skip above)
        // gives a result that only happens if the id-match path executed.
        s.loom.source_id = s.streams[static_cast<size_t>(a)].id;
        const UndoCommand *re = s.undo.redo();
        check("42/redo re-arms the take command", re != nullptr, "");
        if (re)
            undo_apply(s, *re, /*redo=*/true);
        check("42/redo restores the take with its view in lockstep, onto the "
              "SAME recording it was made on",
              s.loom.takes.size() == 1 && s.loom.take_views.size() == 1 &&
                  s.loom.takes[0].label == "arg0 := 11",
              "take_views must move in lockstep with takes, and the id-match "
              "path must actually apply when the recording matches");
    }

    // --- region gap: inspect_start_params attaches a scoped region ONLY for the
    // scoped modes (trace/dataflow) and picks base+len vs func by the spec shape.
    // A whole-process mode and `auto` send none — so the door never blocks Start on
    // a region they do not need, and never sends one the serve host would reject.
    {
        InspectState is;
        // Isolate from the `steps` ring's own default (true — see doors.h): this
        // block's assertions are about REGION params only, and the dedicated
        // sub-block below already covers `steps` on its own terms.
        is.steps = false;
        std::snprintf(is.region, sizeof is.region, "%s", "0x1000:16");
        is.want = LiveMode::Log;
        check("cap/log no region", inspect_start_params(is).empty(),
              "a whole-process mode must send empty start params");
        is.want = LiveMode::Auto;
        check("cap/auto no region", inspect_start_params(is).empty(),
              "auto samples its own region — no region params");
        is.want = LiveMode::Dataflow;
        nlohmann::json p = inspect_start_params(is);
        check("cap/dataflow base+len",
              p.value("base", 0ull) == 0x1000ull &&
                  p.value("len", 0ull) == 16ull && !p.contains("func"),
              "dataflow + 0x1000:16 -> {base,len}");
        is.want = LiveMode::Trace;
        std::snprintf(is.region, sizeof is.region, "%s", "hotfn");
        nlohmann::json f = inspect_start_params(is);
        check("cap/trace func",
              f.value("func", std::string()) == "hotfn" && !f.contains("base"),
              "trace + a name -> {func:\"hotfn\"}");

        // The register ring (--steps): armed only for the dataflow single-step
        // engines (dataflow / auto); a whole-process mode ignores it.
        is.want = LiveMode::Auto;
        is.steps = true;
        check(
            "cap/auto steps", inspect_start_params(is).value("steps", false),
            "auto + steps -> {steps:true} (the live Scrubber's register ring)");
        is.want = LiveMode::Log;
        check("cap/log ignores steps", inspect_start_params(is).empty(),
              "a whole-process mode carries no register ring, steps or not");

        // 35 T4: `continuous` re-arm, only for the dataflow single-step engines
        // (region "hotfn" is still set from above, so the params also carry func).
        is.want = LiveMode::Dataflow;
        is.continuous = true;
        check("cap/dataflow continuous",
              inspect_start_params(is).value("continuous", false),
              "dataflow + continuous -> {continuous:true} (re-arm until Stop)");
        is.continuous = false;
        check("cap/dataflow no continuous by default",
              !inspect_start_params(is).contains("continuous"),
              "off by default -> the param is omitted (one invocation, then "
              "done)");
        is.want = LiveMode::Log;
        is.continuous = true;
        check("cap/log ignores continuous",
              !inspect_start_params(is).contains("continuous"),
              "a whole-process mode has no re-arm loop, continuous or not");

        // 39 T3: the `auto` sample window (ms). Sent as `ms` only for `auto` and
        // only when the operator set a non-default value; 0 (the default) sends
        // nothing so the host applies its own AUTO_WINDOW_MS. A dataflow/whole-
        // process want samples no region, so it never carries the window.
        is.continuous = false;
        is.want = LiveMode::Auto;
        is.window_ms = 0;
        check("cap/auto default window omitted",
              !inspect_start_params(is).contains("ms"),
              "auto + window 0 -> no `ms` (the host default, byte-identical)");
        is.window_ms = 900;
        check("cap/auto window set",
              inspect_start_params(is).value("ms", 0) == 900,
              "auto + window 900 -> {ms:900} (the settable sample window)");
        is.want = LiveMode::Dataflow;
        check("cap/dataflow ignores window",
              !inspect_start_params(is).contains("ms"),
              "a named-region mode samples nothing, so it carries no window");
        is.window_ms = 0;
        is.want = LiveMode::Auto;

        // The tree filter (depth/focus/module/tid/follow) rides this same Start now
        // the patch bay owns tree config: inspect_start_params merges the engine-
        // side filter (obs_tree_start_params) for a `tree` want, omitting any field
        // left at its default. steps/continuous are still set from above, so this
        // also proves a tree carries neither ring nor re-arm loop.
        is.want = LiveMode::Tree;
        check("cap/tree default filter -> empty params",
              inspect_start_params(is).empty(),
              "a tree with an all-default filter sends nothing — and no register "
              "ring or re-arm loop, even with steps/continuous left set");
        is.observer.filter.depth = 3;
        is.observer.filter.focus = "main";
        is.observer.filter.module = "libc";
        nlohmann::json tp = inspect_start_params(is);
        check("cap/tree filter -> depth+focus+module",
              tp.value("depth", 0) == 3 &&
                  tp.value("focus", std::string()) == "main" &&
                  tp.value("module", std::string()) == "libc" &&
                  !tp.contains("steps") && !tp.contains("continuous"),
              "tree + filter -> {depth,focus,module} on the wire, no ring");
    }

    // --- Swap/Queue must carry the same params as the direct Start. Routing tree
    // through the shared budget flow newly exposed it to the budget-recovery paths,
    // which used to drop ALL start params — so a tree started via Queue silently
    // captured the WHOLE process instead of the configured subtree. inspect_arm_
    // queue snapshots inspect_start_params, and the fire replays that snapshot.
    {
        InspectState is;
        is.want = LiveMode::Tree;
        is.observer.filter.depth = 4;
        is.observer.filter.focus = "worker";
        inspect_arm_queue(is);
        check("queue/arms-want",
              is.has_queued && is.queued_want == LiveMode::Tree,
              "arming the queue stashes the current want");
        check("queue/snapshots-tree-filter",
              is.queued_params.value("depth", 0) == 4 &&
                  is.queued_params.value("focus", std::string()) == "worker",
              "a queued tree FREEZES its depth/focus, not an empty whole-process "
              "start (the regression this refactor had to avoid)");
        InspectState wp;
        wp.want = LiveMode::Log;
        inspect_arm_queue(wp);
        check("queue/whole-process-empty", wp.queued_params.empty(),
              "a queued whole-process mode carries no params, as its Start would");
    }

    // --- 39 T5: the self-end reconcile frees the ptrace jack no user action
    // would (a one-shot `auto` that "starts then stops"). inspect_reconcile_self_
    // end keys on LiveStatus::sessions_ended; an in-flight start (awaiting_started,
    // a Swap's stop+start pair) must NOT be freed before its `started` lands.
    {
        InspectState is;
        is.active.push_back(LiveMode::Auto); // a live auto capture holds the jack
        LiveStatus st;                       // it ended on its own
        st.state = LiveState::Idle;
        st.sessions_ended = 1;
        inspect_reconcile_self_end(is, st);
        check("reconcile/self-end-frees-jack",
              is.active.empty() && is.seen_sessions_ended == 1,
              "a self-ended session frees the ptrace jack the pane held forever "
              "(the 'starts then stops, refused: no session' knot)");
        // No NEW terminal event -> a no-op (idempotent, not a re-clear each frame).
        is.active.push_back(LiveMode::Auto);
        inspect_reconcile_self_end(is, st); // sessions_ended still 1
        check("reconcile/no-delta-no-op", is.active.size() == 1,
              "without a new terminal event the reconcile does nothing");

        // A swap's newly-armed session (awaiting_started) is NOT freed by the OLD
        // session's terminal event, even in the brief Idle before the new start
        // lands (the split-frame case state==Idle alone would misfire on).
        InspectState sw;
        sw.active.push_back(LiveMode::Dataflow); // the newly-armed swap target
        sw.awaiting_started = true;              // its `started` has not landed
        LiveStatus swst;
        swst.state = LiveState::Idle; // brief Idle between old stop and new start
        swst.sessions_ended = 1;      // the OLD session's terminal event
        inspect_reconcile_self_end(sw, swst);
        check("reconcile/in-flight-start-not-freed",
              sw.active.size() == 1 && sw.active[0] == LiveMode::Dataflow,
              "a swap's newly-armed session is not freed before its started lands");
        // Once the host confirms the start (Running), the guard clears and the
        // jack stays held.
        InspectState r;
        r.active.push_back(LiveMode::Dataflow);
        r.awaiting_started = true;
        LiveStatus run;
        run.state = LiveState::Running;
        run.sessions_ended = 1;
        inspect_reconcile_self_end(r, run);
        check("reconcile/running-clears-guard-holds-jack",
              !r.awaiting_started && r.active.size() == 1,
              "a confirmed (Running) start clears the in-flight guard and keeps "
              "the jack");

        // The guard must NOT be cleared by a swapped-out session's `skip`: a
        // dataflow/auto blocker that saw 0 passes ends its swap-stop as NEVER_RAN
        // -> serve `skip`, in the same Idle split-frame the guard protects. So a
        // pending swap with skip_code set (from the OLD session) still holds the
        // NEW session's jack until its `started` lands.
        InspectState sw2;
        sw2.active.push_back(LiveMode::Dataflow); // the newly-armed swap target
        sw2.awaiting_started = true;              // its `started` has not landed
        LiveStatus sw2st;
        sw2st.state = LiveState::Idle;
        sw2st.skip_code = 1;       // the OLD (swapped-out) session skipped
        sw2st.sessions_ended = 1;  // its terminal event
        inspect_reconcile_self_end(sw2, sw2st);
        check("reconcile/swapped-out-skip-does-not-free-new",
              sw2.active.size() == 1 && sw2.active[0] == LiveMode::Dataflow,
              "a swapped-out session's skip must NOT free the swap's newly-armed "
              "session before its started lands");
    }

    // --- 39 T5: a healthy new session must not inherit the previous one's refusal
    // banner. feed_line an `err` (sets last_err), then a `started` — last_err must
    // clear, so a stale red "refused:" does not haunt the next capture.
    {
        LiveSession sess;
        sess.feed_line(
            R"({"k":"err","reason":"no session is running","cmd":"stop"})");
        check("t5/err-sets-last-err", !sess.status().last_err.empty(),
              "an err note surfaces as last_err (the refusal banner)");
        sess.feed_line(
            R"({"k":"session","state":"started","mode":"log","pid":4242})");
        check("t5/started-clears-last-err", sess.status().last_err.empty(),
              "a new session's `started` clears the stale refusal banner");
    }

    // --- inspect_request_start refuses an illegal tree filter — the wire-safety the
    // deleted obs_tree_start_command enforced (it returned "" on a bad filter). The
    // patch bay greys Start on it, but the backstop must hold for every caller.
    {
        InspectState is;
        is.host_started = true; // send_start is a safe no-op with no real host
        is.selected_pid = 4242;
        is.want = LiveMode::Tree;
        is.observer.filter.tid = 7;
        is.observer.filter.follow = true; // tid XOR follow — illegal
        check("start/illegal-tree-filter-refused",
              !inspect_request_start(is) && is.active.empty(),
              "a start with an illegal tree filter must not fire or mark the jack");
        is.observer.filter.follow = false; // now legal (tid alone)
        check("start/legal-tree-filter-starts",
              inspect_request_start(is) && is.active.size() == 1 &&
                  is.active[0] == LiveMode::Tree,
              "a legal tree filter starts and marks the jack active");
    }

    // --- inspect_confirm_swap carries the same backstop, symmetric with
    // inspect_request_start: an illegal tree filter must not stop the blocker to
    // start nothing; a legal one completes the swap (blocker stopped, tree active).
    {
        InspectState is;
        is.host_started = true; // send_start/send_stop are safe no-ops, no host
        is.selected_pid = 4242;
        is.want = LiveMode::Tree;
        is.active.push_back(LiveMode::Log); // a blocker holds the jack
        is.swap_pending = true;
        is.observer.filter.depth = -5; // illegal (depth is 1..1000)
        inspect_confirm_swap(is);
        check("swap/illegal-tree-filter-no-op",
              is.swap_pending && is.active.size() == 1 &&
                  is.active[0] == LiveMode::Log,
              "an illegal tree filter must not complete the swap — the blocker "
              "stays and the confirm stays up");
        is.observer.filter.depth = 3; // now legal
        inspect_confirm_swap(is);
        check("swap/legal-tree-filter-completes",
              !is.swap_pending && is.active.size() == 1 &&
                  is.active[0] == LiveMode::Tree,
              "a legal tree filter completes the swap: blocker stopped, tree "
              "the sole active jack");
    }

    // --- Processes double-click / right-click: the "full detail" attach. It
    // captures at the fullest an un-named target admits (auto + register ring) and
    // reveals the right pane, arming the perturb confirm only once a host is up.
    {
        InspectState at;
        inspect_attach_full_detail(at, 4242);
        check("attach/pid", at.selected_pid == 4242,
              "the attach must select the double-clicked pid");
        check("attach/mode-auto", at.want == LiveMode::Auto,
              "full-detail attach uses auto — the fullest un-named capture");
        check("attach/ring", at.steps,
              "full-detail attach arms the register ring (--steps)");
        // inspect_attach_full_detail auto-connects from the saved settings, so
        // whether a host actually comes up depends on the environment — a built
        // ./build/asmspy on a dev box spawns one; the engine-free desktop lane has
        // none. Assert the invariant for WHICHEVER happened rather than depending
        // on asmspy's absence (39 T5: this used to be RED on any checkout that had
        // built the CLI). The Live-capture pane is revealed either way.
        check("attach/opens-capture", at.want_open_capture,
              "full-detail attach always lands on the Live-capture pane");
        if (at.host_started) {
            // The auto-connect succeeded: the capture is armed on the live host
            // and Connect is not forced (a host is already up).
            check("attach/host-up-no-connect-reveal", !at.want_open_connect,
                  "a successful auto-connect does not force the Connect pane");
        } else {
            // No host: reveal Connect so the user can bring one up, and — with no
            // host — nothing is armed yet.
            check("attach/no-host-reveals-connect", at.want_open_connect,
                  "with no host, attach reveals the Connect pane to bring one up");
            check("attach/no-host-does-not-arm", !at.perturb_pending,
                  "with no host there is nothing to arm yet");
        }

        // The perturb confirm now fires ONLY for the unrecoverable arm64
        // blocking-syscall kill — every other target starts immediately (the "just
        // use Start" simplification; the x86 page-dirty nuisance no longer gates).
        // Drive the gate directly (send_start is a safe no-op with no real host —
        // LiveSession::send() guards on wfd_ < 0), so this exercises the arm
        // without spawning anything.
        InspectState ax; // arm64 target: the kill confirm survives
        ax.host_started = true;
        ax.selected_pid = 7;
        ax.want = LiveMode::Auto;
        ax.target_arch = "aarch64";
        bool arm_started = inspect_request_start(ax);
        check("perturb/arm64 arms confirm",
              !arm_started && ax.perturb_pending && ax.want == LiveMode::Auto,
              "on arm64 a single-step attach must arm the kill confirm, not "
              "silently start");

        InspectState ix; // x86 target: Start just starts, no confirm
        ix.host_started = true;
        ix.selected_pid = 7;
        ix.want = LiveMode::Auto;
        ix.target_arch = "x86_64";
        inspect_request_start(ix);
        check("perturb/x86 no confirm", !ix.perturb_pending,
              "on x86 the perturb nuisance no longer gates Start — it starts");
    }

    // ---- R9: selecting a task opens only that task's relevant panes ---------
    // Capture leads with the live workflow (Processes / Live capture / Log / Save)
    // and CLOSES the replay reading panes; Open is the mirror image. Connect stays
    // closed — Capture auto-connects and only the failure path reveals it.
    {
        ShellState cs;
        shell_apply_mode_panes(cs, Mode::Capture);
        check("panes/capture-opens-workflow",
              cs.pane_open[kPaneProcesses] && cs.pane_open[kPaneCapture] &&
                  cs.pane_open[kPaneLog] && cs.pane_open[kPaneSave],
              "Capture opens Processes / Live capture / Log / Save");
        check("panes/capture-closes-reading",
              !cs.pane_open[kPaneRecording] && !cs.pane_open[kPaneLoom] &&
                  !cs.pane_open[kPaneTimeline] && !cs.pane_open[kPaneInspector],
              "Capture closes the replay reading panes");
        check("panes/capture-no-connect", !cs.pane_open[kPaneConnect],
              "Capture must not force the Connect pane open (it auto-connects)");
        check("panes/home-universal", cs.pane_open[kPaneHome],
              "Home is open in every task");
        check("panes/log-universal-capture", cs.pane_open[kPaneLog],
              "the Log is a persistent console — open in every task");

        ShellState os;
        shell_apply_mode_panes(os, Mode::Open);
        check("panes/open-opens-reading",
              os.pane_open[kPaneRecording] && os.pane_open[kPaneLoom] &&
                  os.pane_open[kPaneTimeline] && os.pane_open[kPaneScrubber] &&
                  os.pane_open[kPaneObserver] && os.pane_open[kPaneInspector],
              "Open opens the replay reading panes");
        check("panes/open-closes-capture",
              !os.pane_open[kPaneCapture] && !os.pane_open[kPaneProcesses],
              "Open closes the capture workflow panes");
        // The Log is the exception: a persistent console open in every task, so it
        // survives the switch OUT of Capture rather than being closed with it.
        check("panes/log-universal-open", os.pane_open[kPaneLog],
              "the Log stays open in Open mode too (persistent console)");

        // Learn leads with the reading panes (a walkthrough opens a recording) but
        // NOT the capture workflow; Home stays open in both.
        ShellState le;
        shell_apply_mode_panes(le, Mode::Learn);
        check("panes/learn",
              le.pane_open[kPaneHome] && le.pane_open[kPaneRecording] &&
                  le.pane_open[kPaneScrubber] && !le.pane_open[kPaneCapture] &&
                  !le.pane_open[kPaneProcesses],
              "Learn opens Home + the reading panes, not the capture workflow");

        // Author leads with Home + the authored run's Recording + Scrubber, and
        // closes the capture workflow and the inspector.
        ShellState au;
        shell_apply_mode_panes(au, Mode::Author);
        check("panes/author",
              au.pane_open[kPaneHome] && au.pane_open[kPaneRecording] &&
                  au.pane_open[kPaneScrubber] && !au.pane_open[kPaneCapture] &&
                  !au.pane_open[kPaneInspector],
              "Author opens Home + Recording + Scrubber, not the capture workflow");

        // Entering Capture clears the live-viz one-shot guard so an ongoing capture
        // re-reveals its panes (shell_apply_live_panes re-applies).
        ShellState cg;
        cg.live_applied_ordinal = 3;
        shell_apply_mode_panes(cg, Mode::Capture);
        check("panes/capture-resets-live-guard", cg.live_applied_ordinal == -1,
              "re-entering Capture re-arms the live-viz pass for an ongoing capture");
    }

    // ---- R10: a live capture opens ONLY the viz panes it fills --------------
    // The running mode falls back to inspect.want when the session has no `started`
    // echo yet. A `log` capture fills Observer + Timeline (no Loom/Scrubber); an
    // `auto` capture fills the whole exact deck. Applied ONCE per capture.
    {
        ShellState ls;
        ls.mode = Mode::Capture;
        ls.live_tab = 0;
        ls.inspect.want = LiveMode::Log;
        shell_apply_live_panes(ls);
        check("live-panes/log-fills-observer-timeline",
              ls.pane_open[kPaneObserver] && ls.pane_open[kPaneTimeline],
              "a log capture opens the Observer + Timeline it fills");
        check("live-panes/log-no-loom-scrubber",
              !ls.pane_open[kPaneLoom] && !ls.pane_open[kPaneScrubber] &&
                  !ls.pane_open[kPaneRecording],
              "a log capture does NOT open the exact Loom / Scrubber / Slice");
        check("live-panes/applied-once", ls.live_applied_ordinal == 0,
              "the pass records the capture ordinal it ran for (no live recording "
              "yet -> 0), so it does not re-fire");
        // Idempotent: a manual close after the one-shot pass holds.
        ls.pane_open[kPaneObserver] = false;
        shell_apply_live_panes(ls);
        check("live-panes/one-shot", !ls.pane_open[kPaneObserver],
              "a second call is a no-op — a manual close is respected");
        // The guard keys on the capture ORDINAL, not the tab index — so shifting
        // live_tab (what an unrelated tab close does) must NOT re-open the closed
        // pane. This is the exact regression the index-keyed guard had.
        ls.live_tab = 2;
        shell_apply_live_panes(ls);
        check("live-panes/stable-across-index-shift", !ls.pane_open[kPaneObserver],
              "shifting live_tab (an unrelated close) must not re-open a pane the "
              "user closed");
        // The capture ends -> the guard resets so the next capture re-applies.
        ls.live_tab = -1;
        shell_apply_live_panes(ls);
        check("live-panes/reset-on-end", ls.live_applied_ordinal == -1,
              "when the capture ends the guard resets for the next one");

        ShellState as;
        as.mode = Mode::Capture;
        as.live_tab = 0;
        as.inspect.want = LiveMode::Auto;
        shell_apply_live_panes(as);
        check("live-panes/auto-fills-exact-deck",
              as.pane_open[kPaneRecording] && as.pane_open[kPaneLoom] &&
                  as.pane_open[kPaneScrubber] && as.pane_open[kPaneTimeline] &&
                  as.pane_open[kPaneObserver],
              "an auto capture opens the whole exact deck");

        // A `trace` capture fills Timeline + Recording (3D/region) + Observer, but
        // NOT the exact Loom/Scrubber (it plants a breakpoint, it does not
        // single-step every step).
        ShellState ts;
        ts.mode = Mode::Capture;
        ts.live_tab = 0;
        ts.inspect.want = LiveMode::Trace;
        shell_apply_live_panes(ts);
        check("live-panes/trace",
              ts.pane_open[kPaneTimeline] && ts.pane_open[kPaneRecording] &&
                  ts.pane_open[kPaneObserver] && !ts.pane_open[kPaneLoom] &&
                  !ts.pane_open[kPaneScrubber],
              "a trace capture opens Timeline/Recording/Observer, not Loom/Scrubber");

        // A statistical `sample` capture never single-steps: Observer (hot edges) +
        // the 3D plane, and DEFINITELY not the exact Loom/Scrubber.
        ShellState ss;
        ss.mode = Mode::Capture;
        ss.live_tab = 0;
        ss.inspect.want = LiveMode::Sample;
        shell_apply_live_panes(ss);
        check("live-panes/sample-no-exact",
              ss.pane_open[kPaneObserver] && !ss.pane_open[kPaneLoom] &&
                  !ss.pane_open[kPaneScrubber],
              "a statistical sample capture never opens the exact Loom / Scrubber");

        // Outside Capture/Inspect mode the pass is inert (it must not fight a
        // replay reader who opened a file).
        ShellState off;
        off.mode = Mode::Open;
        off.live_tab = 0;
        off.pane_open[kPaneLoom] = true;
        shell_apply_live_panes(off);
        check("live-panes/inert-outside-capture",
              off.pane_open[kPaneLoom] && off.live_applied_ordinal == -1,
              "the live-viz pass does nothing in Open mode (never fights a reader)");

        // The PRODUCTION path: the running mode comes from the session's `started`
        // echo FIRST, and `want` is only the pre-echo fallback. Feed a `log`
        // started but leave want = Auto — the LOG panes must open (Observer +
        // Timeline, not the full deck), proving status().mode wins over want.
        ShellState ms;
        ms.mode = Mode::Capture;
        ms.live_tab = 0;
        ms.inspect.want = LiveMode::Auto; // deliberately != the echoed mode
        ms.inspect.session.feed_line(
            R"({"k":"session","state":"started","mode":"log","pid":1,"params":{}})");
        shell_apply_live_panes(ms);
        check("live-panes/uses-session-mode",
              ms.pane_open[kPaneObserver] && ms.pane_open[kPaneTimeline] &&
                  !ms.pane_open[kPaneLoom] && !ms.pane_open[kPaneScrubber],
              "the running mode is the session's started echo (log), not want (auto)");
    }

    // ---- R2: the Log is a bounded, colored scrollback -----------------------
    {
        ShellState gs;
        for (size_t i = 0; i < ShellState::kLogMax + 10; i++)
            shell_log_push(gs, "line " + std::to_string(i), ToastKind::Info);
        check("log/bounded", gs.log.size() == ShellState::kLogMax,
              "the log ring drops the oldest past kLogMax");
        check("log/keeps-tail", gs.log.back().text == "line " +
                                    std::to_string(ShellState::kLogMax + 9),
              "the newest line is kept (a scrollback keeps the tail)");
        check("log/drops-head", gs.log.front().text == "line 10",
              "the oldest ten lines dropped");
        shell_log_push(gs, "", ToastKind::Error);
        check("log/ignores-empty", gs.log.size() == ShellState::kLogMax,
              "an empty line is not logged");
    }

    // ---- the terminal-command hint echoes a gate's fix into the Log ---------
    {
        ShellState gs;
        shell_log_command_hint(
            gs, "sampler skipped: needs perf_event_paranoid<=2 or CAP_PERFMON");
        check("log/cmd-hint",
              gs.log.size() == 1 &&
                  gs.log.back().text == "run this in a terminal: sudo sysctl -w "
                                        "kernel.perf_event_paranoid=2",
              gs.log.empty() ? "(no line)" : gs.log.back().text.c_str());
        shell_log_command_hint(gs, "capturing 42 steps"); // names no gate
        check("log/cmd-hint-none", gs.log.size() == 1,
              "an advice that names no gate logs nothing");
    }

    // ---- 40 T2: the per-pass invocation selector over a continuous capture --
    // A continuous dataflow capture is many invocation passes in one recording;
    // shell_open caches them segmented, the dataflow views follow the latest pass
    // by default (like the Scrubber), and shell_apply_df_pass pins an earlier one.
    // A one-shot recording has a single pass and offers no selector.
    {
        auto step0 = [](const DataflowStream &df) -> long long {
            for (const ValRec &v : df.recs)
                if (v.step == 0)
                    return static_cast<long long>(v.value);
            return -1;
        };
        ShellState ps;
        std::string err;
        int ci = shell_open(ps, gd("low-fidelity/continuous-df.asmtrace"), err);
        check("dfpass/open", ci >= 0, err.c_str());
        if (ci >= 0) {
            size_t i = static_cast<size_t>(ci);
            check("dfpass/three-passes", ps.seg_df[i].passes.size() == 3,
                  "continuous-df carries three df_invocation passes");
            check("dfpass/default-follow-latest", ps.df_pass[i] == -1,
                  "a freshly opened recording follows its latest pass");
            // Default: following the latest -> pass 2 (step-0 value 100) is shown.
            size_t cur = shell_apply_df_pass(ps, i);
            check("dfpass/latest-index", cur == 2, "latest of three passes is 2");
            check("dfpass/latest-value", step0(ps.streams[i].df) == 100,
                  "the latest pass's step-0 op value is 100");
            // Pin pass 0: the active Streams::df becomes pass 0's (value 6), proof
            // the views can reach an earlier pass, not just the conflated tail.
            ps.df_pass[i] = 0;
            cur = shell_apply_df_pass(ps, i);
            check("dfpass/pin-index", cur == 0, "pinned to pass 0");
            check("dfpass/pin-value", step0(ps.streams[i].df) == 6,
                  "pass 0's step-0 op value is 6, distinct from the latest's");
            // Clearing the pin returns to the latest pass.
            ps.df_pass[i] = -1;
            cur = shell_apply_df_pass(ps, i);
            check("dfpass/refollow", cur == 2 && step0(ps.streams[i].df) == 100,
                  "clearing the pin returns to the latest pass");
        }
        // A one-shot recording (no df_invocation marker): exactly one pass, so the
        // pager draws nothing and shell_apply_df_pass follows that single pass.
        int oi = shell_open(ps, gd("mem-df-chain.asmtrace"), err);
        check("dfpass/oneshot-open", oi >= 0, err.c_str());
        if (oi >= 0)
            check("dfpass/oneshot-one-pass",
                  ps.seg_df[static_cast<size_t>(oi)].passes.size() == 1,
                  "a one-shot recording has a single pass and no pager");
    }

    if (failures) {
        std::fprintf(stderr, "test_shell: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_shell: PASS\n");
    return 0;
}
