// test_shell.cpp — the shell's fidelity behaviour + a headless render smoke
// (03-desktop-shell.md T6). shell_banner is D7 as behaviour: non-null for a
// truncated / dropped / torn recording, null for a clean one. Then draw_shell is
// driven for 3 null-backend frames over a Workspace of the fixtures to prove no
// path crashes without a display. No GLFW, no GL, no engines.
#include <algorithm>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // 19: FindWindowByName + ImGuiWindow::{WasActive,DockId}

#include "doc/df_passes.h" // 37 T1: the pass pager's region words
#include "doc/recording.h"
#include "doc/workspace_state.h" // 20 T3: capture/restore round-trip
#include "scene3d/hud.h"         // 36 T4: placement_chips (fidelity chrome)
#include "scene3d/pick.h" // 52 T3: resolve_pick parity for the flat surface
#include "ui/fidelity.h" // 44 T3: FidelityTier
#include "ui/layout.h" // 19: kPane* names, DockLayout, LayoutPreset, layout_build
#include "ui/shell.h"
#include "ui/theme.h"         // 44 T3: dt_warn_col/dt_refuse_col/dt_dim_col
#include "ui/transport.h"     // 44 T5: transport_tick, direct on Transport
#include "ui/view_presence.h" // 20 T1: the data-driven view set
#include "views/abixray.h"    // 09-T4: the surfaced ABI x-ray tab's model
#include "views/scene2d.h" // 52 T1/T2: Scene2dPlan / build_scene2d_plan
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
            // 56 T3: the per-module canopies are woven alongside the slice —
            // a recording with code regions and hits must produce at least
            // one (the exact rung for whichever region(s) it touched).
            check("scene/canopies woven", !sv.canopies.empty(),
                  "a placed abs recording must produce module canopies");

            // 36 T4: the abs golden is fully placed — no PLACEMENT chip fires
            // for it (all its chip branches are for rel/df/unplaced
            // recordings). 58 T1 added the DATA-rung census to the same
            // vector (the coarse/rich chip and, when `mem` is present, the
            // span + drop counts), which this golden does raise: it carries
            // no `mem`, so it states the coarse rung. Assert the placement
            // half is silent by naming what may legitimately appear, rather
            // than by an emptiness that now means two different things.
            auto abs_chips = scene3d::placement_chips(sv.terr, sv.traj);
            for (const auto &c : abs_chips)
                check("scene/abs draws no placement chip (fully placed abs "
                      "path)",
                      c.text == sv.terr.mem_note,
                      ("an abs recording raised a placement chip: " + c.text)
                          .c_str());
            check("scene/abs still states its data rung (58 T1)",
                  abs_chips.size() == 1 && !sv.terr.mem_note.empty(),
                  "the coarse data rung must be stated, never silent");

            // 54 T1: this golden carries no `mem` and no abs df values, so
            // observed_data_spans() must add nothing — the projection this
            // recording weaves stays byte-identical to before T1.
            check("scene/no mem => no observed-data span note",
                  sv.terr.proj.data_span_note.empty(),
                  "an observed-data note appeared with nothing to observe");

            // 48 T4: the home landmark is woven alongside the other pure
            // models — a codeimage recording places a code region, so
            // scene_home_target must find it and the HUD's synced copy
            // must match (the per-frame sync at the top of draw_scene_overview).
            check("scene/home landmark computed for a codeimage recording",
                  sv.has_home,
                  "a recording with a placed code region must have a home");
            check("scene/HUD synced with the landmark",
                  sv.hud.has_home == sv.has_home &&
                      sv.hud.home_u == sv.home_u && sv.hud.home_v == sv.home_v,
                  "draw_scene_overview must sync home_* into HudState every frame");

            // 50 T2: a Selection.off in this abs-basis recording locates onto
            // the plane and syncs into the HUD, headlessly (no GL needed —
            // scene_locate_off is pure; only the shader ring itself is GL).
            s3.selection.set(a->id, std::nullopt, uint64_t{1048576});
            ImGui::NewFrame();
            ImGui::Begin("t3b");
            draw_scene_overview(s3, s3.ws.recordings[static_cast<size_t>(igs)],
                                *a);
            ImGui::End();
            ImGui::Render();
            check("scene/selection locates onto the plane", sv.highlight.ok,
                  sv.highlight.reason.c_str());
            check("scene/HUD synced with the highlight",
                  sv.hud.has_highlight == sv.highlight.ok,
                  "draw_scene_overview must sync has_highlight into HudState");

            // Clearing the selection must clear the cached highlight too, not
            // leave a stale ring from the previous recording state.
            s3.selection.clear();
            ImGui::NewFrame();
            ImGui::Begin("t3c");
            draw_scene_overview(s3, s3.ws.recordings[static_cast<size_t>(igs)],
                                *a);
            ImGui::End();
            ImGui::Render();
            check("scene/cleared selection clears the highlight", !sv.highlight.ok,
                  "a cleared Selection must not leave a stale highlight");
        }

        // === 52 T2/T3: the GL-free flat surface, built from the SAME woven
        // models draw_scene_overview already produced above. Every
        // draw_scene_overview call in this null-backend file already runs
        // draw_flat_surface() (the three degraded branches call it
        // unconditionally, and scene_host is null throughout this file) —
        // this block proves the plan it built for a real codeimage
        // recording is non-empty and that a pick against it resolves
        // through the SAME scene3d::resolve_pick the 3D path uses (the
        // pure pick-inversion/resolve_pick parity contract itself is
        // proven directly in test_scene2d.cpp). ===
        if (igs >= 0) {
            s3.active_tab = igs;
            const Streams *a = shell_a(s3);
            const SceneView &sv = s3.scenes[static_cast<size_t>(igs)];
            check("scene/flat_view defaults to false", !sv.flat_view,
                  "a freshly-opened recording must default to the GL viewport");

            Scene2dPlan plan = build_scene2d_plan(sv.terr, sv.slice, sv.traj,
                                                  sv.conv, sv.hud.t);
            check("flat surface/has a plane for a codeimage recording",
                  plan.has_plane, "the abs golden must place a flat plane");
            check("flat surface/at least one block", !plan.blocks.empty(),
                  "a sized plane must emit blocks");
            check("flat surface/at least one path with a placed point",
                  std::any_of(plan.paths.begin(), plan.paths.end(),
                              [](const Scene2dPath &p) {
                                  return std::any_of(
                                      p.points.begin(), p.points.end(),
                                      [](const Scene2dPathPoint &pp) {
                                          return pp.placed;
                                      });
                              }),
                  "the abs golden's exact trajectory must place a vertex");

            // T3: pick the first placed path point's own cell and prove it
            // resolves — the same call scene2d_draw.cpp's click handler
            // makes, exercised here with a hand-picked cell instead of a
            // simulated mouse event.
            bool picked = false;
            for (const Scene2dPath &path : plan.paths) {
                for (const Scene2dPathPoint &pp : path.points) {
                    if (!pp.placed)
                        continue;
                    uint32_t cell = 0;
                    if (scene2d_pick_cell(plan, pp.u, pp.v, &cell)) {
                        scene3d::Pick pk;
                        pk.kind = scene3d::Pick::Cell;
                        pk.cell = cell;
                        auto link = scene3d::resolve_pick(
                            sv.terr, sv.traj, a != nullptr ? a->id : "",
                            pk, sv.conv);
                        check("flat surface/a placed vertex's cell resolves",
                              link.has_value(),
                              "a placed exact-path cell must resolve to a link");
                        picked = true;
                        break;
                    }
                }
                if (picked)
                    break;
            }
            check("flat surface/found at least one placed point to pick",
                  picked, "the abs golden's path must have a placed vertex");

            // The reading-mode toggle round-trips and does not crash a
            // render, even under the null backend (where the branch it
            // gates is unreachable, but the field itself must persist).
            s3.scenes[static_cast<size_t>(igs)].flat_view = true;
            ImGui::NewFrame();
            ImGui::Begin("t3flat");
            if (a != nullptr)
                draw_scene_overview(
                    s3, s3.ws.recordings[static_cast<size_t>(igs)], *a);
            ImGui::End();
            ImGui::Render();
            check("flat surface/flat_view persists across a frame",
                  s3.scenes[static_cast<size_t>(igs)].flat_view,
                  "the toggle must not be reset by draw_scene_overview");
            s3.scenes[static_cast<size_t>(igs)].flat_view = false;
        }

        // === 48 T4: camera_here_text — a pure function of (Projection, target) ===
        {
            space::Projection p = space::build_projection(
                {space::Region{0x400000, 0x1000, space::Region::Code,
                               "libfoo .text", 0}});
            float u = 0.0f, v = 0.0f;
            check("here-text/setup: 0x400000 projects",
                  p.project(0x400000, &u, &v), "test address did not project");
            std::string t = scene3d::camera_here_text(p, u, v);
            // 59 T1: the same target, read through each substrate. Only the
            // plane has an address under the camera; the other four have axes
            // that are not addresses, so stating one for them fabricates a
            // location nobody recorded.
            {
                const std::string plane = scene3d::camera_here_line(
                    scene3d::SceneKind::Plane, p, u, v);
                check("here-line/plane: unchanged on the address plane",
                      plane == t,
                      "the plane substrate must read exactly as it always did");
                for (scene3d::SceneKind k : scene3d::all_scene_kinds()) {
                    if (k == scene3d::SceneKind::Plane)
                        continue;
                    const std::string o = scene3d::camera_here_line(k, p, u, v);
                    check("here-line/off-plane: no fabricated address",
                          o.find("0x400000") == std::string::npos &&
                              o.find("libfoo .text") == std::string::npos,
                          "a non-plane substrate must not report an address or "
                          "a region — its axes are not addresses");
                    check("here-line/off-plane: still says something",
                          !o.empty(),
                          "a blank line is not a refusal; the substrate's own "
                          "axis is the honest thing to state");
                    check("here-line/off-plane: plane nav does not apply",
                          !scene3d::hud_plane_nav_applies(k),
                          "the address `go to` row moves the camera by PLANE "
                          "coordinates and must not be offered here");
                }
                check("here-line/plane: plane nav applies",
                      scene3d::hud_plane_nav_applies(scene3d::SceneKind::Plane),
                      "the address plane keeps its go-to row");
            }
            check("here-text: names the region label",
                  t.find("libfoo .text") != std::string::npos,
                  ("got: " + t).c_str());
            check("here-text: names the address",
                  t.find("0x400000") != std::string::npos,
                  ("got: " + t).c_str());
            // Outside [0,1) unprojects to nothing — a real, statable fact.
            std::string outside = scene3d::camera_here_text(p, 5.0f, 5.0f);
            check("here-text: outside the domain says so, never blank",
                  outside.find("outside the compacted domain") !=
                      std::string::npos,
                  ("got: " + outside).c_str());
        }

        // === 48 T5: scene_control_lines — exhaustive over CamKey, never stale ===
        {
            std::vector<std::string> lines = scene3d::scene_control_lines();
            using scene3d::CamKey;
            const CamKey all[] = {
                CamKey::OrbitLeft,  CamKey::OrbitRight,  CamKey::OrbitUp,
                CamKey::OrbitDown,  CamKey::DollyIn,     CamKey::DollyOut,
                CamKey::Reset,      CamKey::TopDown,     CamKey::PanLeft,
                CamKey::PanRight,   CamKey::PanForward,  CamKey::PanBack,
            };
            check("controls: at least one line per CamKey value",
                  lines.size() >= (sizeof(all) / sizeof(all[0])),
                  ("got " + std::to_string(lines.size()) + " lines for " +
                   std::to_string(sizeof(all) / sizeof(all[0])) +
                   " CamKey values")
                      .c_str());
            check("controls: the mouse gestures are advertised too",
                  !lines.empty() &&
                      lines[0].find("orbit") != std::string::npos,
                  "the left-drag orbit line must lead the list");
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

    // T4 (47-scene-inspect-and-pickable-overlays): UNTESTED CLAIM, STATED
    // RATHER THAN IMPLIED (per the brief's own Tests section). The hover
    // tooltip lives entirely inside draw_scene_overview's `if (s.scene_host
    // != nullptr && s.scene_host->ready())` branch — the interactive-viewport
    // path that needs a real GL texture to blit and a real InvisibleButton to
    // hover. Under s.scene_host == nullptr (this file's entire null backend,
    // scene/no-host-by-default asserted above) draw_scene_overview returns at
    // the placard branch BEFORE that code is ever reached — there is no path
    // by which this binary can drive a hover. The `desktop-ui-test`
    // (imgui_test_engine) camera test above this doc's own T4 wiring
    // ("keyboard_orbit_reset_topdown", test_ui.cpp) drives the SAME
    // draw_scene_overview under the SAME null backend for the identical
    // reason (no mouse simulation over a GL texture either) — so no test lane
    // in this tree can exercise the tooltip end-to-end. What IS covered,
    // exhaustively, is the pure data the tooltip does nothing but format:
    // every PickHint field/branch is golden-tested in test_drillin.cpp (T2),
    // and the tooltip's own rendering is a direct, unconditional field-by-
    // field TextUnformatted/TextColored — no branch of its own beyond
    // "fidelity non-empty" and "target empty", both already covered by that
    // golden text. A real end-to-end pixel/interaction proof would need a
    // GL-backed interaction lane this tree does not have.

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

    // 49 T3/T4 (one-time-truth): the encodings legend + axis note + ruler
    // ticks are all PURE, so their content is asserted directly — no ImGui
    // frame, no camera-projected draw.
    {
        // T3: the legend is EXHAUSTIVE BY TEST — exactly the four TerrainFlag
        // bits kTerrainFrag (embedded.h) branches on, no more, no fewer. A
        // shader branch added without a matching swatch here fails this
        // check, which is the whole point (hud.h's own doc comment).
        auto swatches = scene3d::terrain_encoding_swatches();
        auto has_flag = [&](space::TerrainFlag f) {
            for (const auto &sw : swatches)
                if (sw.flag == f)
                    return true;
            return false;
        };
        check("encodings legend: exactly four swatches", swatches.size() == 4,
              ("got " + std::to_string(swatches.size())).c_str());
        check("encodings legend: covers TF_CHURN", has_flag(space::TF_CHURN),
              "no churn/scaffold swatch");
        check("encodings legend: covers TF_TORN", has_flag(space::TF_TORN),
              "no torn/rubble swatch");
        check("encodings legend: covers TF_STAT", has_flag(space::TF_STAT),
              "no statistical swatch");
        check("encodings legend: covers TF_UNKNOWN",
              has_flag(space::TF_UNKNOWN), "no fog-of-war swatch");
        check("encodings legend: never covers TF_READ/TF_WRITE (unbranched)",
              !has_flag(space::TF_READ) && !has_flag(space::TF_WRITE),
              "the terrain shader does not key a colour off read/write");

        // T4 (56): the opcode-class legend is exhaustive over space::OpClass
        // — one swatch per value, no more, no fewer (mirrors embedded.h's
        // opClassHue[8]).
        auto op_swatches = scene3d::opcode_class_swatches();
        check("opcode legend: exactly 8 swatches (one per OpClass)",
              op_swatches.size() == 8,
              ("got " + std::to_string(op_swatches.size())).c_str());
        for (space::OpClass cls :
            {space::OpClass::Unknown, space::OpClass::Move,
             space::OpClass::IntArith, space::OpClass::Logic,
             space::OpClass::CompareBranch, space::OpClass::ScalarFloat,
             space::OpClass::VectorSIMD, space::OpClass::System}) {
            bool found = false;
            for (const auto &sw : op_swatches)
                if (sw.cls == cls)
                    found = true;
            check((std::string("opcode legend covers ") +
                  space::op_class_name(cls))
                      .c_str(),
                  found, "a class added to OpClass without a swatch here");
        }

        // T3: the height-scale note states the raw magnitude, or says there
        // is none.
        check("height scale note: states the raw magnitude",
              scene3d::height_scale_note(3).find("3") != std::string::npos,
              scene3d::height_scale_note(3).c_str());
        check("height scale note: an empty slice says so, not \"0\"",
              scene3d::height_scale_note(0).find("no data") !=
                  std::string::npos,
              scene3d::height_scale_note(0).c_str());

        // T4: the axis note is non-empty (the single source of truth between
        // the draw call and this check that the wording did not vanish).
        check("vertical axes note: non-empty",
              scene3d::vertical_axes_note()[0] != '\0', "note text is empty");

        // 55 T4 step 5: the HUD states WHICH compositing mode the stacked
        // translucent surfaces use. The whole point is that two users on two
        // machines are never silently comparing two different algorithms, so
        // the note must name the mode ("dithered") AND grade it
        // ("approximate") — a note that merely said "translucency: on" would
        // satisfy a non-empty check while defeating the purpose.
        {
            const std::string tm = scene3d::translucency_mode_note();
            check("translucency mode note: names the mode (dithered)",
                  tm.find("dithered") != std::string::npos, tm.c_str());
            check("translucency mode note: grades it (approximate)",
                  tm.find("approximate") != std::string::npos, tm.c_str());
        }

        // T4: ticks span exactly [0, nsteps], and the ruler is skipped
        // entirely (empty ticks) when there is no time axis to ruler.
        {
            auto ticks = scene3d::trajectory_axis_ticks(37);
            check("axis ticks: first tick is 0", !ticks.empty() &&
                                                     ticks.front() == 0,
                  "the ruler must start at the recording's own beginning");
            check("axis ticks: last tick is nsteps",
                  !ticks.empty() && ticks.back() == 37,
                  "the ruler must reach the recording's real extent");
            check("axis ticks: ascending", std::is_sorted(ticks.begin(),
                                                          ticks.end()),
                  "ticks must be monotonic for a ruler to read correctly");
        }
        check("axis ticks: empty when nsteps == 0",
              scene3d::trajectory_axis_ticks(0).empty(),
              "a zero-step recording has no time axis to ruler");
    }

    // T5 (47-scene-inspect-and-pickable-overlays): the overlay-line legend is
    // EXHAUSTIVE BY TEST, mirroring 49 T3's own terrain-swatch pattern just
    // above — every drawn overlay-line class (scene.cpp's conv_arcs_ and
    // access_spurs_) must have exactly one entry here.
    {
        auto swatches = scene3d::overlay_encoding_swatches();
        check("overlay legend: exactly two swatches (convergence + spur)",
              swatches.size() == 2,
              ("got " + std::to_string(swatches.size())).c_str());
        bool has_conv = false, has_spur = false;
        for (const auto &sw : swatches) {
            if (sw.label.find("convergence") != std::string::npos)
                has_conv = true;
            if (sw.label.find("access spur") != std::string::npos)
                has_spur = true;
        }
        check("overlay legend: covers convergence", has_conv,
              "no convergence swatch");
        check("overlay legend: covers access spur", has_spur,
              "no access-spur swatch");
        // T5 step 3: the arc's caption states its own fidelity grade — a
        // co-locality hint, never a proven race or order (converge.h's own
        // wording) — so a pickable arc cannot read as a stronger claim than
        // a drawn one.
        for (const auto &sw : swatches)
            if (sw.label.find("convergence") != std::string::npos)
                check("overlay legend: convergence caption states its own "
                      "fidelity grade",
                      sw.label.find("never a proven race or order") !=
                          std::string::npos,
                      sw.label.c_str());

        // The colours are the keep-in-sync copy of scene.cpp's Line::color
        // literals (hud.h's own doc comment) — pin them so a future colour
        // change on either side fails a named check rather than silently
        // drifting.
        for (const auto &sw : swatches) {
            if (sw.label.find("convergence") != std::string::npos)
                check("overlay legend: convergence colour matches "
                      "scene.cpp's magenta",
                      sw.rgb[0] == 1.0f && sw.rgb[1] == 0.25f &&
                          sw.rgb[2] == 0.85f,
                      "colour drifted from scene.cpp's Line::color");
            if (sw.label.find("access spur") != std::string::npos)
                check("overlay legend: access-spur colour matches "
                      "scene.cpp's lavender",
                      sw.rgb[0] == 0.85f && sw.rgb[1] == 0.85f &&
                          sw.rgb[2] == 0.90f,
                      "colour drifted from scene.cpp's Line::color");
        }

        check("inspect hint: advertises hover-then-click",
              std::string(scene3d::inspect_hint_note()).find("hover") !=
                      std::string::npos &&
                  std::string(scene3d::inspect_hint_note()).find("click") !=
                      std::string::npos,
              scene3d::inspect_hint_note());
    }

    // T5: toggling the convergence checkbox is reflected in the SceneLayers
    // the frame carries — driven through a real ImGui frame (draw_scene_hud
    // itself), not just the HudState default, so the wiring from click to
    // SceneLayers is what's actually asserted (the "plumbing already
    // exists, only the checkbox was missing" claim T5's own brief makes).
    {
        ImGui::CreateContext();
        ImGuiIO &io5 = ImGui::GetIO();
        io5.IniFilename = nullptr;
        unsigned char *p5 = nullptr;
        int w5 = 0, h5 = 0;
        io5.Fonts->GetTexDataAsRGBA32(&p5, &w5, &h5);
        io5.DisplaySize = ImVec2(1280, 720);
        io5.DeltaTime = 1.0f / 60.0f;

        scene3d::HudState hs;
        check("T5: convergence layer defaults on (SceneLayers's own default)",
              hs.layers.convergence, "convergence should default true");

        space::TerrainModel terr;
        space::TrajectorySet traj;
        ImGui::NewFrame();
        scene3d::draw_scene_hud(hs, terr, traj);
        ImGui::Render();
        // No real mouse to click the checkbox under the null backend, but the
        // field itself round-trips through the SAME HudState the checkbox
        // writes — flip it as a click would and re-draw, asserting nothing
        // in draw_scene_hud silently resets it.
        hs.layers.convergence = false;
        ImGui::NewFrame();
        scene3d::draw_scene_hud(hs, terr, traj);
        ImGui::Render();
        check("T5: the convergence toggle survives a redraw (no silent "
              "reset)",
              !hs.layers.convergence,
              "draw_scene_hud must not overwrite the layer toggle it did "
              "not itself flip");
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

        // Process details (gui-process-details Task 7 review, IMPORTANT 3):
        // its context gate (pctx_details) is selected_pid ALONE, deliberately
        // NOT host_started like its sibling kPaneCapture just above — assert
        // both directions the same way that sibling gate is asserted a few
        // lines up. Driven through shell_apply_mode_panes (not a hand-set
        // pane_open, unlike the kPaneCapture/kPanePtSlice checks above) so a
        // regression in ANY of kPaneDetails' three wiring points is visible
        // here, not just the context gate itself:
        //   - the kManagedPanes row (pane_def/pane_is_open/pane_shown all
        //     fall through to TRUE for an unmanaged name — see pane_shown's
        //     own `d == nullptr` clause above — so losing that row does not
        //     hide the pane, it leaves it shown UNCONDITIONALLY; only the
        //     second assertion below (no selection) can catch that);
        //   - the mode_wants_pane Capture-mode arm (shell_apply_mode_panes
        //     would then never set pane_open[kPaneDetails] true at all, so
        //     the FIRST assertion below fails instead);
        //   - the context gate itself (pctx_details gaining a host_started
        //     requirement would fail the first assertion the same way the
        //     sibling kPaneCapture check above does).
        // NOT caught here, same as no other pane in this file: which dock
        // node it lands in (layout.cpp's DockBuilderDockWindow call) — dock-
        // node placement has no assertion for ANY pane in this tree.
        shell_apply_mode_panes(ds, Mode::Capture);
        ds.inspect.host_started = false;
        ds.inspect.selected_pid = 1234;
        frame(ds);
        frame(ds);
        check("pane/details shows with no host, just a selection",
              active(kPaneDetails),
              "Process details must not require host_started (pctx_details "
              "is selected_pid alone)");
        ds.inspect.selected_pid = 0;
        frame(ds);
        frame(ds);
        check("pane/details hides with no selection", !active(kPaneDetails),
              "Process details must hide once nothing is selected");
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
        // docs/internal/archive/gui/45-launch-and-window-target.md T4: Launch is a
        // SECOND way into the same LiveObserver posture, landing on kPaneLaunch
        // instead of kPaneProcesses — no table detour — and with NO
        // auto-connect (connecting before a command is typed is premature;
        // inspect_launch_full_detail connects when "Launch & trace" fires).
        shell_select_mode(es, Mode::Launch);
        check("45t4/launch-preset",
              es.pending_preset.value_or(LayoutPreset::Author) ==
                  LayoutPreset::LiveObserver,
              "Launch mode requests the LiveObserver perspective — the SAME "
              "live workflow as Capture, not a new one");
        check("45t4/launch-opens-launch-pane", es.pane_open[kPaneLaunch],
              "Launch must open the Launch pane");
        check("45t4/launch-closes-processes", !es.pane_open[kPaneProcesses],
              "Launch must NOT open Processes — no table detour");
        check("45t4/launch-no-autoconnect", !es.inspect.want_autoconnect,
              "Launch must not auto-connect: connecting before a command is "
              "typed is premature");
        check("45t4/launch-focuses-launch-pane", es.inspect.want_focus_launch,
              "Launch must ask draw_shell to focus the Launch pane");
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

        // 34 T2's "View in 3D overview" button (and the want_scene handoff it
        // raised) is GONE — the 3D overview is reached like every other view, by
        // its own tab or the `5` keyroute, so no capture pane carries a second
        // door to it. What that keyroute needs still holds: the live capture is a
        // real tab, and Scene3D is a view id the tab strip can select.
        check("34/live tab is a real tab", ls.live_tab >= 0,
              "the `5` keyroute selects the 3D view of the live capture's tab");

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
        // 59 T1: the SUBSTRATE and its per-kind cameras ride the same
        // preserve-list, and for the same reason. Keeping `cam` while dropping
        // `kind` is worse than dropping both: the pane snaps back to the
        // address plane WEARING the abandoned substrate's orbit. `hud.kind` is
        // carried but cannot help — draw_scene_overview overwrites it from
        // sv.kind on the next frame.
        ls.scenes[i].kind = scene3d::SceneKind::ModuleRibbon;
        ls.scenes[i].prism_reg = 101;
        ls.scenes[i].kind_cam.assign(scene3d::all_scene_kinds().size(),
                                     scene3d::Camera{});
        ls.scenes[i].kind_cam_inited.assign(scene3d::all_scene_kinds().size(),
                                            0);
        const size_t rib =
            scene_kind_index(scene3d::SceneKind::ModuleRibbon);
        ls.scenes[i].kind_cam[rib].yaw = 1.25f;
        ls.scenes[i].kind_cam_inited[rib] = 1;
        sess.feed_line(
            R"({"k":"df_step","step":9,"off":0,"disasm":"nop","ops":[]})");
        shell_sync_live_tab(ls);
        check("25t6/substrate preserved",
              ls.scenes[i].kind == scene3d::SceneKind::ModuleRibbon,
              "a live event batch reset the chosen substrate to the address "
              "plane — the user's scene selection is per-VIEW state, exactly "
              "like the camera beside it");
        check("25t6/per-kind cameras preserved",
              rib < ls.scenes[i].kind_cam.size() &&
                  ls.scenes[i].kind_cam[rib].yaw == 1.25f &&
                  rib < ls.scenes[i].kind_cam_inited.size() &&
                  ls.scenes[i].kind_cam_inited[rib] == 1,
              "the per-substrate cameras were destroyed, so switching back to "
              "a substrate would re-seed its default framing");
        check("25t6/prism register preserved", ls.scenes[i].prism_reg == 101,
              "the lane prism's selected register is per-VIEW state too — "
              "resetting it silently re-picks a different register");
        // 61 T9: the layout fingerprint must survive the SAME reset, for the
        // same reason and by the same mechanism. If it does not, every batch
        // compares against an invalid fingerprint, the reflow notice can never
        // fire, and the failure is SILENT — no pure test can see it, because
        // the rule itself stays correct.
        //
        // Asserted AFTER shell_sync_live_tab but BEFORE the lazy 3D weave (that
        // is gated on !sv.built inside the pane draw, which this test never
        // reaches), so the sentinel is still the value that was PRESERVED
        // rather than a freshly computed digest. That ordering is what makes
        // this check specific to the preserve-list.
        const long cap_ord =
            static_cast<long>(sess.recordings().size()) +
            (sess.growing() != nullptr ? 1 : 0);
        ls.scenes[i].layout_fp.valid = true;
        ls.scenes[i].layout_fp.digest = 0xD00Dull;
        ls.scenes[i].layout_fp.regions = 7;
        ls.scenes[i].layout_fp_capture = cap_ord;
        sess.feed_line(
            R"({"k":"df_step","step":1,"off":0,"disasm":"nop","ops":[]})");
        shell_sync_live_tab(ls);
        check("25t6/layout fingerprint preserved",
              ls.scenes[i].layout_fp.valid &&
                  ls.scenes[i].layout_fp.digest == 0xD00Dull &&
                  ls.scenes[i].layout_fp.regions == 7,
              "a live re-weave dropped the previous layout digest, so the "
              "reflow notice can never fire");
        // 61 T10: and the other half of that contract, which an adversarial
        // review found missing. The digest is per-RECORDING state on a
        // per-VIEW preserve-list, so it must NOT cross a capture boundary: a
        // continuous re-arm swaps a new recording into this slot, and carrying
        // the old capture's digest would fire a reflow note on the new
        // recording's FIRST weave, naming a region count ("7 regions became
        // N") that no single recording ever had. Simulated by stamping a stale
        // capture ordinal, which is exactly what the previous capture leaves
        // behind.
        ls.scenes[i].layout_fp.valid = true;
        ls.scenes[i].layout_fp.digest = 0xBEEFull;
        ls.scenes[i].layout_fp.regions = 7;
        ls.scenes[i].layout_fp_capture = cap_ord - 1; // a PREVIOUS capture
        sess.feed_line(
            R"({"k":"df_step","step":2,"off":0,"disasm":"nop","ops":[]})");
        shell_sync_live_tab(ls);
        check("25t6/a previous capture's layout digest is DROPPED",
              !ls.scenes[i].layout_fp.valid &&
                  ls.scenes[i].layout_fp_capture == -1,
              "the digest of a DIFFERENT capture survived into this one, so "
              "the new recording's first weave would report a reflow the "
              "reader never saw");
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

    // --- the AUTO-LED substrate sweep's hand-off -----------------------------
    //
    // From the `auto` front door, three of the five 3D substrates were
    // unreachable: an `auto` capture records `codeimage` + `df_step` and nothing
    // else, so the invocation stack (`coverage`) and the module excursion ribbon
    // (`call`) stayed disabled — and "Capture every substrate", which would have
    // filled them, refused to start without a hand-named region, which an `auto`
    // operator by construction does not have.
    //
    // The auto-led sweep closes that: leg 1 samples for a region, and the scoped
    // leg behind it inherits what the `pick` event named. THIS is the hand-off,
    // and it is the part that can be silently wrong.
    {
        std::vector<LiveNote> notes;
        check("sweep/pick/none-yet", inspect_sweep_pick_region(notes).empty(),
              "with no pick on the wire there is no region to hand on — the "
              "sweep must stop and say so, not send address 0");

        // An idle window rides the SAME channel with zero base/len (39 T3).
        notes.push_back(
            {"session",
             nlohmann::json::parse(
                 R"J({"state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"idle","func":"(idle window)","base":0,"len":0,"attempt":1,"of":3}})J")});
        check(
            "sweep/pick/idle-window-is-not-a-region",
            inspect_sweep_pick_region(notes).empty(),
            "an idle window observed NOTHING; treating its sentinel as a pick "
            "would hand the trace leg a zero-length region at address 0");

        notes.push_back(
            {"session",
             nlohmann::json::parse(
                 R"({"state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"entry","func":"first_ranked","base":4096,"len":64,"attempt":1,"of":2}})")});
        check("sweep/pick/adopts-the-pick",
              inspect_sweep_pick_region(notes) == "0x1000:64",
              "the scoped leg must inherit the region the sampler measured, in "
              "the grammar inspect_start_params reads back");

        // The host WALKS past a ranked candidate that never re-enters and
        // re-emits on each, so the LAST real pick is the region it captured.
        // Taking the first would scope the trace leg to a function the auto leg
        // itself gave up on.
        notes.push_back(
            {"session",
             nlohmann::json::parse(
                 R"({"state":"pick","mode":"auto","pick":{"sampler":"ibs-op","evidence":"entry","func":"second_ranked","base":8192,"len":96,"attempt":2,"of":2}})")});
        check("sweep/pick/last-walked-candidate-wins",
              inspect_sweep_pick_region(notes) == "0x2000:96",
              "the host walks past a candidate that never re-enters; the leg "
              "must inherit the one it ENDED on, not the one it abandoned");

        // A non-`session` note is not a pick, whatever it carries.
        notes.push_back({"err", nlohmann::json::parse(R"({"state":"pick"})")});
        check("sweep/pick/only-session-notes",
              inspect_sweep_pick_region(notes) == "0x2000:96",
              "an `err` note is not the pick channel");
    }
    {
        // The SHAPE the button will run, and the LATCH that keeps it stable. The
        // auto-led sweep writes the picked region into the region field, so a
        // shape re-derived per frame would flip mid-sequence.
        InspectState is;
        check("sweep/shape/no-region-leads-with-auto",
              !inspect_sweep_legs(is).empty() &&
                  inspect_sweep_legs(is)[0] == LiveMode::Auto,
              "with no region named, pressing the button must run the auto-led "
              "sweep — that is the only shape reachable from the `auto` door");
        std::snprintf(is.region, sizeof is.region, "%s", "hotfn");
        check("sweep/shape/named-region-runs-the-three-engines",
              !inspect_sweep_legs(is).empty() &&
                  inspect_sweep_legs(is)[0] == LiveMode::Tree,
              "a named region needs no picker, so the sweep runs the three "
              "scoped engines directly");

        // The latch: start auto-led, then let the region field fill in the way
        // the poll fills it, and the shape must not move.
        InspectState latched;
        latched.host_started = true;
        latched.selected_pid = 1;
        latched.record_session = true;
        std::snprintf(latched.record_path, sizeof latched.record_path, "%s",
                      "/tmp/sweep.asmtrace");
        const std::string why_blocked =
            "an un-named region must not block the sweep any more: got \"" +
            inspect_sweep_blocked(latched) + "\"";
        check("sweep/latch/startable-with-no-region",
              inspect_sweep_blocked(latched).empty(), why_blocked.c_str());
        inspect_sweep_start(latched);
        check("sweep/latch/started", latched.sweep_running, "the sweep armed");
        std::snprintf(latched.region, sizeof latched.region, "%s", "0x1000:64");
        check("sweep/latch/shape-survives-the-adopted-region",
              !inspect_sweep_legs(latched).empty() &&
                  inspect_sweep_legs(latched)[0] == LiveMode::Auto,
              "the auto leg fills the region field in; re-deriving the shape "
              "from it would swap the leg list out from under the running "
              "sequence");
    }

    // --- 2026-08-06 plan, Task 4 fix (coordinator review, Finding 1a/3a): a
    // SIXTH direct-send_start site — inspect_sweep_poll. inspect_sweep_blocked
    // only checks the FIRST leg, at ARM time, and draw_patch_bay polls the
    // sweep BEFORE the button that arms one, so the earliest a leg can fire is
    // the frame AFTER arm — an ordinary sequence in which perf_probed/perf_ok
    // (recomputed every frame, never latched) can flip in the gap.
    {
        InspectState sw;
        sw.host_started = true;
        sw.selected_pid = 1;
        sw.record_session = true;
        std::snprintf(sw.record_path, sizeof sw.record_path, "%s",
                      "/tmp/sweep-host-blocked.asmtrace");
        inspect_sweep_start(sw); // no region named -> auto-led, leg 0 = auto
        check("sweep/poll/armed", sw.sweep_running, "the sweep armed");
        // The measured verdict turns against `auto` in the gap between arm
        // and this poll.
        sw.perf_probed = true;
        sw.perf_ok = false;
        sw.perf_reason = "perf_event_open refused (Permission denied)";
        check("sweep/poll/host-blocked-stops-not-fires",
              !inspect_sweep_poll(sw) && !sw.sweep_running && sw.active.empty(),
              "leg 0 (`auto`) cannot fire on a host where perf is MEASURED "
              "refused — the poll must STOP the sweep, not send a doomed "
              "start and blame the operator for it");
        // "stopped" AND the leg name together — not just the leg name alone,
        // which a FIRED leg's own note ("sweep: leg 1/3 (auto)") would also
        // contain, making the check blind to exactly the mutation it exists
        // to catch (verified: mutating the check above alone left this one
        // green, because both notes name `auto`).
        check("sweep/poll/host-blocked-names-the-leg",
              sw.sweep_note.find("stopped") != std::string::npos &&
                  sw.sweep_note.find("auto") != std::string::npos,
              "the stop note must name which leg it stopped at, the same "
              "convention the no-region-picked stop already follows");
    }

    // --- 2026-08-06 plan, Task 12 (M13): the completion note is scored
    // against what LANDED, not against the leg count. The old note fired on
    // `sweep_at >= legs.size()` alone and named all four substrates
    // unconditionally; the SIMD lane prism is data-dependent on the picked
    // ROUTINE (measured: 8-11 writes over blend_tile, 0 over entered_often,
    // byte-identical capture flags) — reproduce that here through the REAL
    // decode path (space::regions_from_codeimage / obs_tree_build /
    // obs_region_build / decode_streams+lane_prism_any), not the pure
    // scorer's own unit tests (test_budget), which cannot see the door's
    // wiring at all.
    {
        // inspect_sweep_picked_name in isolation first: the name a missing
        // prism gets blamed on has TWO sources, and they must not be
        // confused. An auto pick's OWN func wins over a typed region — the
        // auto-adopted region field holds a base+len spec (pick_region_spec's
        // own comment: a name would re-resolve against a possibly-duplicated
        // symbol), which names no routine at all.
        InspectState pn;
        check("sweep/picked-name/nothing-yet",
              inspect_sweep_picked_name(pn).empty(),
              "no pick, no typed region — there is no routine to name");
        std::snprintf(pn.region, sizeof pn.region, "%s", "0x1000:64");
        check("sweep/picked-name/base-len-names-nothing",
              inspect_sweep_picked_name(pn).empty(),
              "a bare base+len is not a symbol — naming it as \"the routine\" "
              "would be a fabrication parse_region_spec itself would reject "
              "as a func");
        std::snprintf(pn.region, sizeof pn.region, "%s", "entered_often");
        check("sweep/picked-name/typed-symbol-is-the-name",
              inspect_sweep_picked_name(pn) == "entered_often",
              "a scoped sweep never samples — the operator's own typed "
              "symbol IS the routine name");
        pn.session.feed_line(
            R"J({"k":"session","state":"pick","mode":"auto","pick":{"sampler":)J"
            R"J("ibs-op","evidence":"idle","func":"(idle window)","base":0,)J"
            R"J("len":0,"attempt":1,"of":1}})J");
        check("sweep/picked-name/idle-window-does-not-override",
              inspect_sweep_picked_name(pn) == "entered_often",
              "an idle window observed nothing — it must not blot out the "
              "typed region's name");
        // A DIFFERENT name than the typed region, so a decoder that (wrongly)
        // kept reading the region field instead of the pick would be caught
        // here rather than passing by coincidence.
        pn.session.feed_line(
            R"({"k":"session","state":"pick","mode":"auto","pick":{"sampler":)"
            R"("ibs-op","evidence":"entry","func":"second_ranked","base":8192,)"
            R"("len":96,"attempt":1,"of":1}})");
        {
            const std::string got = inspect_sweep_picked_name(pn);
            const std::string why =
                "once the sampler names a routine that is THE name — even "
                "though the region field still reads \"entered_often\" "
                "(typed, never adopted here): got \"" +
                got + "\"";
            check("sweep/picked-name/real-pick-wins-over-typed-region",
                  got == "second_ranked", why.c_str());
        }

        static const char *kHdr =
            R"({"asmtrace":1,"container":"ndjson","producer":{"name":"asmspy",)"
            R"("version":"1.1.0"},"provenance":{"backend":"ptrace-dataflow",)"
            R"("exact":true,"trust":"exact"},"arch":"x86_64"})";
        auto feed_leg = [&](LiveSession &sess, const char *mode,
                            const std::vector<std::string> &body_lines) {
            sess.feed_line(std::string(R"({"k":"cmd","cmd":"start","mode":")") +
                           mode + "\"}");
            sess.feed_line(std::string(R"({"k":"session","state":"started",)") +
                           R"("mode":")" + mode +
                           R"(","pid":4242,"params":{}})");
            sess.feed_line(kHdr);
            sess.feed_line(R"({"k":"codeimage","base":4096,"len":256,)"
                           R"("version":1})");
            for (const std::string &l : body_lines)
                sess.feed_line(l);
            sess.feed_line(R"({"k":"end","events":1})");
            sess.feed_line(
                R"({"k":"session","state":"stopped","reason":"max"})");
        };

        // THE measured defect, reproduced end to end: a scoped (named-region)
        // sweep whose three legs (tree/trace/dataflow) all completed and each
        // wrote real events — the routine is just scalar, so the dataflow
        // leg's op carries no `wide` write.
        {
            InspectState is;
            is.host_started = true;
            is.selected_pid = 4242;
            is.record_session = true;
            std::snprintf(is.record_path, sizeof is.record_path, "%s",
                          "/tmp/sweep-complete-scalar.asmtrace");
            std::snprintf(is.region, sizeof is.region, "%s", "entered_often");
            is.sweep_have_region = true;
            is.sweep_running = true;
            is.sweep_at = sweep_legs(true).size(); // every leg already ran

            feed_leg(is.session, "tree",
                     {R"({"k":"call","tid":1,"depth":0,"addr":4096,)"
                      R"("name":"entered_often","module":"auto_victim"})"});
            feed_leg(is.session, "trace",
                     {R"({"k":"trace","off":0,"basis":"rel"})",
                      R"({"k":"coverage","blocks":[0],"basis":"rel"})"});
            feed_leg(is.session, "dataflow",
                     {R"({"k":"df_step","step":0,"off":0,"disasm":"inc eax",)"
                      R"("ops":[{"space":"reg","reg":35,"size":4,"write":true,)"
                      R"("value_valid":true,"value":41}]})"});
            check("sweep/complete/scalar/three-legs-recorded",
                  is.session.recordings().size() == 3,
                  "the fixture must land exactly the three scoped legs");

            bool fired = inspect_sweep_poll(is);
            check("sweep/complete/scalar/poll-stops-not-fires",
                  !fired && !is.sweep_running,
                  "the completion arm stops the sweep — there is no fourth "
                  "leg to fire");

            size_t marker = is.sweep_note.find("Not this time");
            std::string landed = marker == std::string::npos
                                     ? is.sweep_note
                                     : is.sweep_note.substr(0, marker);
            std::string absent = marker == std::string::npos
                                     ? std::string()
                                     : is.sweep_note.substr(marker);
            const std::string why_plane =
                "codeimage rode along on every leg — got \"" + is.sweep_note +
                "\"";
            check("sweep/complete/scalar/claims-plane",
                  landed.find("address plane") != std::string::npos,
                  why_plane.c_str());
            const std::string why_stack =
                "the trace leg wrote a real coverage block set — got \"" +
                is.sweep_note + "\"";
            check("sweep/complete/scalar/claims-stack",
                  landed.find("invocation stack") != std::string::npos,
                  why_stack.c_str());
            const std::string why_ribbon =
                "the tree leg wrote a real call event — got \"" +
                is.sweep_note + "\"";
            check("sweep/complete/scalar/claims-ribbon",
                  landed.find("module excursion ribbon") != std::string::npos,
                  why_ribbon.c_str());
            const std::string why_prism =
                "M13: the dataflow leg's only op carried no `wide` write — "
                "the note must not claim the prism landed: got \"" +
                is.sweep_note + "\"";
            check("sweep/complete/scalar/does-not-claim-the-prism",
                  landed.find("SIMD lane prism") == std::string::npos,
                  why_prism.c_str());
            const std::string why_reason =
                "the reason must name the ROUTINE the sweep captured, not "
                "the capture itself — got \"" +
                is.sweep_note + "\"";
            check("sweep/complete/scalar/names-the-routine-not-the-capture",
                  absent.find("entered_often") != std::string::npos,
                  why_reason.c_str());
        }

        // The AUTO-LED shape: the picked routine's name comes from the pick
        // event, and the region field (once the auto leg's hand-off adopts
        // it) holds base+len, NOT the name — this is the one path where the
        // two could be confused, so it is exercised here rather than assumed
        // from the isolated inspect_sweep_picked_name checks above.
        {
            InspectState is;
            is.host_started = true;
            is.selected_pid = 4242;
            is.record_session = true;
            std::snprintf(is.record_path, sizeof is.record_path, "%s",
                          "/tmp/sweep-complete-auto.asmtrace");
            is.sweep_have_region = false;
            is.sweep_running = true;
            is.sweep_at =
                sweep_legs(false).size(); // auto -> tree -> trace, done

            // The auto leg: samples, picks `entered_often`, then captures
            // dataflow over it — codeimage + df_step, per doors.h's own
            // description of what an `auto` capture records.
            is.session.feed_line(
                R"({"k":"session","state":"pick","mode":"auto","pick":{)"
                R"("sampler":"ibs-op","evidence":"entry","func":)"
                R"("entered_often","base":4096,"len":64,"attempt":1,"of":1}})");
            feed_leg(is.session, "auto",
                     {R"({"k":"df_step","step":0,"off":0,"disasm":"inc eax",)"
                      R"("ops":[{"space":"reg","reg":35,"size":4,"write":true,)"
                      R"("value_valid":true,"value":41}]})"});
            // The hand-off: region now holds the pick's base+len, same as
            // inspect_sweep_adopt_pick would leave it — NOT the func name.
            std::snprintf(is.region, sizeof is.region, "%s", "0x1000:40");
            feed_leg(is.session, "tree",
                     {R"({"k":"call","tid":1,"depth":0,"addr":4096,)"
                      R"("name":"entered_often","module":"auto_victim"})"});
            feed_leg(is.session, "trace",
                     {R"({"k":"trace","off":0,"basis":"rel"})",
                      R"({"k":"coverage","blocks":[0],"basis":"rel"})"});

            bool fired = inspect_sweep_poll(is);
            check("sweep/complete/auto/poll-stops-not-fires",
                  !fired && !is.sweep_running, "no fourth leg to fire");

            size_t marker = is.sweep_note.find("Not this time");
            std::string absent = marker == std::string::npos
                                     ? std::string()
                                     : is.sweep_note.substr(marker);
            const std::string why =
                "the region field holds base+len once adopted — the routine "
                "name must come from the PICK, not that spec: got \"" +
                is.sweep_note + "\"";
            check("sweep/complete/auto/names-the-pick-not-the-region-spec",
                  absent.find("entered_often") != std::string::npos &&
                      absent.find("0x1000:40") == std::string::npos,
                  why.c_str());
        }
    }

    // --- region gap: inspect_start_params attaches a scoped region ONLY for the
    // scoped modes (trace/dataflow) and picks base+len vs func by the spec shape.
    // A whole-process mode and `auto` send none — so the door never blocks Start on
    // a region they do not need, and never sends one the serve host would reject.
    {
        InspectState is;
        // Isolate from the `steps` ring's own default (true) and the `auto` sample
        // window's (2000 ms) — see doors.h: this block's assertions are about
        // REGION params only. The window IS independently re-covered below on its
        // own terms (`cap/auto default window omitted`, `cap/auto window set`).
        // `steps` is NOT re-covered for `Auto` anywhere else in this file: the
        // only other steps-related check (`cap/auto steps`) sets is.steps=true
        // and never re-tests the false case — so THIS assertion, just below, is
        // the one and only place a false `is.steps` reaching `Auto` is proven to
        // stay off the wire. Coordinator review (2026-08-06 plan, Task 8):
        // narrowing this off `.empty()` for `insns` silently dropped that
        // coverage once; do not narrow it again without re-adding the same checks.
        is.steps = false;
        is.window_ms = 0;
        std::snprintf(is.region, sizeof is.region, "%s", "0x1000:16");
        is.want = LiveMode::Log;
        check("cap/log no region", inspect_start_params(is).empty(),
              "a whole-process mode must send empty start params");
        is.want = LiveMode::Auto;
        // 2026-08-06 plan, Task 8: no longer `.empty()` — auto now ALSO carries
        // `insns` unconditionally (it is itself a dataflow engine, doors.h's own
        // "the auto leg ... is itself the dataflow engine"). Narrowed to what the
        // comment above actually claims: no REGION key AND (since is.steps=false
        // and is.window_ms=0 are still in effect here) no `steps`/`ms` either —
        // insns present, nothing else that this specific state should produce.
        {
            nlohmann::json ap = inspect_start_params(is);
            check(
                "cap/auto no region",
                !ap.contains("base") && !ap.contains("func") &&
                    !ap.contains("len") && !ap.contains("steps") &&
                    !ap.contains("ms"),
                "auto samples its own region — no region params, and with "
                "is.steps=false/is.window_ms=0 still set, no steps/ms either");
        }
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

        // `continuous` DEFAULTS per mode: on for `auto` (which finds its own
        // region from a sample window, so one invocation is over before you look
        // at it), off for a named dataflow region. Applied on each entry into a
        // mode, and never once the operator has moved the checkbox themselves.
        {
            InspectState d;
            d.want = LiveMode::Auto;
            inspect_apply_continuous_default(d);
            check("cap/auto defaults to continuous", d.continuous,
                  "picking `auto` arms the re-arm loop by default");
            d.want = LiveMode::Dataflow;
            inspect_apply_continuous_default(d);
            check("cap/dataflow defaults to one invocation", !d.continuous,
                  "a named region keeps the one-invocation default");
            d.want = LiveMode::Auto;
            inspect_apply_continuous_default(d);
            check("cap/re-entering auto re-arms the default", d.continuous,
                  "the default follows the mode, not just the first frame");
            // The operator's own choice pins: unticking continuous under `auto`
            // must survive a trip through another mode and back.
            d.continuous = false;
            d.continuous_touched = true;
            d.want = LiveMode::Log;
            inspect_apply_continuous_default(d);
            d.want = LiveMode::Auto;
            inspect_apply_continuous_default(d);
            check("cap/a touched continuous is never re-defaulted",
                  !d.continuous,
                  "a default must not fight a choice the operator made");
            // A defaulted `auto` reaches the wire as {continuous:true} — the
            // checkbox and the start params are the same value, never two.
            InspectState w;
            w.want = LiveMode::Auto;
            inspect_apply_continuous_default(w);
            check("cap/auto default reaches the wire",
                  inspect_start_params(w).value("continuous", false),
                  "the default must be what the start actually sends");
            // And the untouched `auto` sample window is 2000 ms, sent explicitly
            // (0 would mean "the host's own 400 ms default, send no `ms`").
            check("cap/auto default window is 2000ms",
                  inspect_start_params(w).value("ms", 0) == 2000,
                  "a fresh auto capture samples for 2000 ms, not the host's 400");
            check("procs/only-attachable is on by default", w.hide_unattachable,
                  "the picker opens on the rows it can actually act on");
        }

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

    // 2026-08-06 plan, Task 8: three flags the serve host honours and the GUI
    // never sent. `insns` is unconditional for the dataflow engines -- without
    // it a dataflow capture emits no `trace` stream, and dt_diff_build cannot
    // align a pair, so two perfect captures are undiffable.
    {
        InspectState is;
        is.want = LiveMode::Dataflow;
        std::snprintf(is.region, sizeof is.region, "%s", "hotfn");
        check("cap/dataflow sends insns",
              inspect_start_params(is).value("insns", false),
              "a dataflow capture must carry its instruction stream, or the "
              "Diff and the 3D canvas have nothing to align");
        is.want = LiveMode::Log;
        check("cap/log ignores insns",
              !inspect_start_params(is).contains("insns"),
              "a whole-process mode single-steps nothing");
        is.want = LiveMode::Dataflow;
        is.want_mem = true;
        check("cap/mem is opt-in", inspect_start_params(is).value("mem", false),
              "`mem` feeds the address plane's data layers");
        is.want_statediff = true;
        nlohmann::json p = inspect_start_params(is);
        check("cap/statediff is opt-in", p.value("statediff", false),
              "`statediff` is what gives the divergence scene its ribs");

        // `auto` is the SAME dataflow engine under another name (doors.h:
        // "the auto leg ... is itself the dataflow engine") -- insns must
        // reach it too, or an auto-led sweep leg is still undiffable.
        InspectState au;
        au.want = LiveMode::Auto;
        check("cap/auto sends insns too",
              inspect_start_params(au).value("insns", false),
              "auto is a dataflow engine with a picked-not-typed region; it "
              "needs the same trace stream to be diffable");

        // Mutation guard: `mem`/`statediff` must NOT be a hidden default. If
        // either leaked out unconditionally (the same bug this task fixes for
        // `insns`, but in the wrong direction), a bare dataflow capture would
        // silently pay the statediff cost (forces the register ring on server
        // side, cli/asmspy.c) on every single Start.
        InspectState fresh;
        fresh.want = LiveMode::Dataflow;
        std::snprintf(fresh.region, sizeof fresh.region, "%s", "hotfn");
        nlohmann::json fp = inspect_start_params(fresh);
        check("cap/mem off by default", !fp.contains("mem"),
              "an un-ticked capture must not pay mem's per-access cost");
        check("cap/statediff off by default", !fp.contains("statediff"),
              "an un-ticked capture must not force the register ring on");
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

    // --- 2026-08-06 plan, Task 4 fix (coordinator review, Finding 1/3a): the
    // Queue-fire backstop. inspect_queue_poll (extracted out of draw_patch_bay
    // so it is headlessly testable) re-checks the host verdict at FIRE time —
    // Queue can only be ARMED while the host permits it, but the jack frees
    // LATER, on its own schedule, and perf_probed/perf_ok can flip in the gap.
    {
        InspectState is;
        is.host_started = true;
        is.selected_pid = 4242;
        is.want = LiveMode::Auto;
        inspect_arm_queue(is); // armed while the host verdict was unprobed
        check("queue/armed", is.has_queued, "the queue armed");
        // The jack is free (active empty, so budget_queue_ready holds), but
        // perf now measures refused — the arm-time snapshot must not be
        // trusted at fire time.
        is.perf_probed = true;
        is.perf_ok = false;
        is.perf_reason = "perf_event_open refused (Permission denied)";
        check("queue/host-blocked-does-not-fire",
              !inspect_queue_poll(is) && is.has_queued && is.active.empty(),
              "`auto` cannot fire from Queue on a host where perf is "
              "MEASURED refused — the queued want must stay queued rather "
              "than fire a doomed capture");
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

    // --- 2026-08-06 plan, Task 4 fix (coordinator review, Finding 1/3a): the
    // SAME symmetry test as the tree-filter block above, for the host-
    // capability backstop inspect_confirm_swap carries independently (it
    // sends `start` directly, never through inspect_request_start).
    {
        InspectState is;
        is.host_started = true;
        is.selected_pid = 4242;
        is.want = LiveMode::Auto;
        is.active.push_back(LiveMode::Log); // a blocker holds the jack
        is.swap_pending = true;
        is.perf_probed = true;
        is.perf_ok = false;
        is.perf_reason = "perf_event_open refused (Permission denied)";
        inspect_confirm_swap(is);
        check("swap/host-blocked-no-op",
              is.swap_pending && is.active.size() == 1 &&
                  is.active[0] == LiveMode::Log,
              "`auto` cannot fire via Swap on a host where perf is MEASURED "
              "refused — the blocker must stay and the confirm must stay up, "
              "exactly like the illegal-tree-filter case above");
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

    // --- 2026-08-06 plan, Task 4 fix (coordinator review, Finding 1/3): the
    // host-capability gate must refuse to FIRE, not merely grey a radio — a
    // greyed radio only stops a CLICK from picking a blocked mode; it has no
    // bearing on `s.want` already holding one. This is the backstop's own
    // test, exactly the "the guarantee never rests on a UI gate alone" idiom
    // the tree-filter block above already pins for inspect_request_start.
    {
        InspectState hb; // perf MEASURED refused on this host; want = auto
        hb.host_started = true;
        hb.selected_pid = 7;
        hb.want = LiveMode::Auto;
        hb.perf_probed = true;
        hb.perf_ok = false;
        hb.perf_reason = "perf_event_open refused (Permission denied)";
        check("start/host-blocked-refused",
              !inspect_request_start(hb) && hb.active.empty(),
              "`auto` cannot fire on a host where perf is MEASURED refused — "
              "it would record ZERO events, and this backstop must hold even "
              "though the radio is greyed for the SAME reason");

        // An unprobed host (not yet connected, or a remote/ssh session) must
        // block NOTHING — a stale or wrong-machine verdict must never refuse
        // a Start on a guess (mode_host_blocked's own header comment).
        InspectState hu;
        hu.host_started = true;
        hu.selected_pid = 7;
        hu.want = LiveMode::Auto;
        hu.perf_probed = false; // not asked (or asked about the wrong host)
        hu.perf_ok = false;
        hu.perf_reason = "perf_event_open refused (Permission denied)";
        check("start/unprobed-host-not-blocked",
              inspect_request_start(hu) && hu.active.size() == 1,
              "an unprobed host must not refuse a Start — it has not measured "
              "anything about the machine that will actually run asmspy");

        // The double-click path: inspect_attach_full_detail force-sets `auto`
        // unconditionally, so a stale/measured-blocked verdict must survive
        // the force-set rather than being bypassed by it. Pre-set
        // host_started so the attach does not need to dial a real connect
        // (mirrors the ax/ix pattern above).
        InspectState ha;
        ha.host_started = true;
        ha.perf_probed = true;
        ha.perf_ok = false;
        ha.perf_reason = "perf_event_open refused (Permission denied)";
        inspect_attach_full_detail(ha, 4242);
        check("attach/host-blocked-does-not-fire",
              ha.want == LiveMode::Auto && ha.selected_pid == 4242 &&
                  ha.active.empty(),
              "a double-click still selects the pid and `auto` (so the "
              "operator sees WHY the radio and Start are greyed), but must "
              "not have fired a start that records nothing");

        // The Launch path carries the identical backstop — `sample` is the
        // one kLaunchModes entry the sampler needs.
        InspectState hl;
        hl.host_started = true;
        std::snprintf(hl.launch_cmd, sizeof hl.launch_cmd, "/bin/true");
        hl.want = LiveMode::Sample;
        hl.perf_probed = true;
        hl.perf_ok = false;
        hl.perf_reason = "perf_event_open refused (Permission denied)";
        check("launch/host-blocked-refused",
              !inspect_request_launch(hl) && hl.active.empty(),
              "`sample` cannot fire from Launch on a host where perf is "
              "MEASURED refused — the launch backstop must hold too");
    }

    // --- docs/internal/archive/gui/45-launch-and-window-target.md T3/T4: the Launch
    // pane's "Launch & trace" — mirrors the full-detail attach block above,
    // but there is no pid until the host names one. ---------------------------
    {
        InspectState lt;
        std::snprintf(lt.launch_cmd, sizeof lt.launch_cmd, "/bin/true");
        inspect_launch_full_detail(lt);
        check("launch/opens-capture", lt.want_open_capture,
              "launch always lands on the Live-capture pane");
        if (lt.host_started) {
            check("launch/host-up-no-connect-reveal", !lt.want_open_connect,
                  "a successful auto-connect does not force the Connect pane");
        } else {
            check("launch/no-host-reveals-connect", lt.want_open_connect,
                  "with no host, launch reveals the Connect pane to bring one "
                  "up");
        }

        // Drive the gate directly (send_launch is a safe no-op with no real
        // host — LiveSession::send() guards on wfd_ < 0), so this exercises
        // inspect_request_launch's own logic without spawning anything —
        // exactly the pattern inspect_request_start's tests above use.
        InspectState lg;
        lg.host_started = true;
        std::snprintf(lg.launch_cmd, sizeof lg.launch_cmd, "/bin/true");
        lg.want = LiveMode::Log;
        bool fired = inspect_request_launch(lg);
        check("launch/fires",
              fired && lg.active.size() == 1 && lg.active[0] == LiveMode::Log,
              "a filled-in command with a host up must fire the launch and "
              "occupy the jack");
        check("launch/awaiting-pid", lg.launch_awaiting_pid,
              "a launch has no pid yet — launch_awaiting_pid must be armed");
        check("launch/selected-pid-unset", lg.selected_pid == 0,
              "no pid is known until the started event names one "
              "(inspect_reconcile_self_end adopts it, tested below)");

        // A blank command refuses cleanly — the button is gated on this too,
        // but the function must not depend on the UI gate alone (F22-style).
        InspectState lb;
        lb.host_started = true;
        bool blank_fired = inspect_request_launch(lb);
        check("launch/blank-refuses", !blank_fired && lb.active.empty(),
              "a blank command must refuse cleanly, not fire an empty argv");

        // A busy jack refuses cleanly too — but with NO swap offer (doors.h:
        // there is no already-selected target for a launch's swap to name).
        InspectState lbusy;
        lbusy.host_started = true;
        std::snprintf(lbusy.launch_cmd, sizeof lbusy.launch_cmd, "/bin/true");
        lbusy.want = LiveMode::Log;
        lbusy.active.push_back(LiveMode::Stream); // occupies the one ptrace jack
        bool busy_fired = inspect_request_launch(lbusy);
        check("launch/busy-refuses", !busy_fired && !lbusy.swap_pending,
              "a busy jack must refuse the launch cleanly, with no swap "
              "offer");

        // The pid-adoption path (inspect_reconcile_self_end): a launch's pid
        // is adopted from LiveStatus::pid the moment the host confirms
        // Running — and ONLY for a launch (launch_awaiting_pid), never
        // overwriting an attach's already-known pid.
        InspectState lp;
        lp.launch_awaiting_pid = true;
        LiveStatus lst;
        lst.state = LiveState::Running;
        lst.pid = 424242;
        inspect_reconcile_self_end(lp, lst);
        check("launch/pid-adopted",
              lp.selected_pid == 424242 && !lp.launch_awaiting_pid,
              "a launch's pid must be adopted from the started event's "
              "LiveStatus::pid, and the awaiting flag cleared");

        InspectState la; // an attach's pid must NEVER be touched by this path
        la.selected_pid = 111;
        la.launch_awaiting_pid = false;
        LiveStatus ast;
        ast.state = LiveState::Running;
        ast.pid = 999999;
        inspect_reconcile_self_end(la, ast);
        check("launch/attach-pid-untouched", la.selected_pid == 111,
              "an attach's already-known pid (launch_awaiting_pid false) "
              "must never be overwritten by this path");
    }

    // --- docs/internal/archive/gui/45-launch-and-window-target.md T7/T8: the
    // crosshair drag's STATE MACHINE — driven directly (synthetic
    // PickedWindow/ShellState), independent of a real X11 drag or display,
    // per T7's own "Tests." note. ---------------------------------------------
    {
        ShellState ps;
        check("pick/starts-clear", !ps.picking_window,
              "a fresh ShellState must not start mid-pick");
        shell_start_window_pick(ps);
        check("pick/drag-start-arms", ps.picking_window,
              "shell_start_window_pick must set picking_window");

        // A failed pick (T8 step 1): logs the stated reason, clears the flag,
        // touches NO session state (never a silent no-op).
        PickedWindow bad;
        bad.ok = false;
        bad.why_not = "no window at that point";
        size_t log_before = ps.log.size();
        long pid_before = ps.inspect.selected_pid;
        Mode mode_before = ps.mode;
        shell_finish_window_pick(ps, bad);
        check("pick/release-clears-on-failure", !ps.picking_window,
              "a failed pick must still clear picking_window");
        check("pick/failure-logs-why-not",
              ps.log.size() > log_before &&
                  ps.log.back().text == bad.why_not,
              "a failed pick must log the stated reason, verbatim, never "
              "silently");
        check("pick/failure-touches-no-session-state",
              ps.inspect.selected_pid == pid_before && ps.mode == mode_before,
              "a failed pick must not attach or change mode");

        // A successful pick (T8 step 2): attaches exactly as a Processes-row
        // double-click would (inspect_attach_full_detail — auto mode, the
        // register ring armed) and lands in Mode::Capture.
        ShellState ok_s;
        shell_start_window_pick(ok_s);
        PickedWindow good;
        good.ok = true;
        good.pid = 31415;
        good.title = "a real window";
        shell_finish_window_pick(ok_s, good);
        check("pick/release-clears-on-success", !ok_s.picking_window,
              "a successful pick must clear picking_window");
        check("pick/success-selects-pid", ok_s.inspect.selected_pid == 31415,
              "a successful pick must select the resolved pid");
        check("pick/success-mode-auto", ok_s.inspect.want == LiveMode::Auto,
              "a successful pick attaches at full detail (auto), same as a "
              "table double-click");
        check("pick/success-lands-capture", ok_s.mode == Mode::Capture,
              "a successful pick must land the shell in Mode::Capture");
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
        check("live-panes/log-fills-observer-only",
              ls.pane_open[kPaneObserver] && !ls.pane_open[kPaneTimeline],
              "a log capture opens the Observer it fills — and NOT the "
              "Timeline, which is the OPERAND timeline (one row per df_step) "
              "and would render zero rows for a syscall-only capture");
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
              ts.pane_open[kPaneRecording] && ts.pane_open[kPaneObserver] &&
                  !ts.pane_open[kPaneTimeline] && !ts.pane_open[kPaneLoom] &&
                  !ts.pane_open[kPaneScrubber],
              "a trace capture opens Recording (its Canvas + 3D overview) and "
              "Observer (codeimage) — not the operand Timeline, which needs "
              "df_step, and not the exact Loom/Scrubber");

        // A `tree` capture fills the 3D overview's module-excursion ribbon: the
        // serve host arms a code image over the executable's text precisely so
        // it can (cli/asmspy.c:4031-4040). Omitting kPaneRecording here is what
        // CLOSED that pane on every tree capture.
        ShellState tr;
        tr.mode = Mode::Capture;
        tr.live_tab = 0;
        tr.inspect.want = LiveMode::Tree;
        shell_apply_live_panes(tr);
        check("live-panes/tree-opens-3d-host",
              tr.pane_open[kPaneObserver] && tr.pane_open[kPaneRecording],
              "a tree capture opens the Observer (call tree) AND the Recording "
              "pane that hosts the 3D overview its ribbon lives in");

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
              ms.pane_open[kPaneObserver] && !ms.pane_open[kPaneTimeline] &&
                  !ms.pane_open[kPaneLoom] && !ms.pane_open[kPaneScrubber],
              "the running mode is the session's started echo (log -> Observer "
              "alone), not want (auto -> the whole exact deck)");
    }

    // ---- 59 T1: a kind that becomes unavailable must be LEFT ----------------
    //
    // Entering an unavailable kind is guarded (the selector wraps those entries
    // in BeginDisabled). Staying on one was guarded by NOTHING, so detaching B
    // while the divergence substrate was showing kept drawing it as an empty
    // scene — the exact fabrication build_divergence_scene refuses to perform
    // by returning a refusal card instead.
    {
        std::vector<std::string> why(scene3d::all_scene_kinds().size());
        const size_t div = scene_kind_index(scene3d::SceneKind::Divergence);

        why[div] = "";
        check("evict/available-stays",
              shell_evict_unavailable_kind(scene3d::SceneKind::Divergence,
                                           why) ==
                  scene3d::SceneKind::Divergence,
              "an available kind must not be evicted");

        why[div] = "needs a second recording (press d to attach one)";
        check("evict/unavailable-falls-back",
              shell_evict_unavailable_kind(scene3d::SceneKind::Divergence,
                                           why) == scene3d::SceneKind::Plane,
              "a kind that became unavailable must fall back to Plane rather "
              "than keep drawing an empty substrate");

        check("evict/plane-is-terminal",
              shell_evict_unavailable_kind(scene3d::SceneKind::Plane, why) ==
                  scene3d::SceneKind::Plane,
              "Plane is the terminal fallback; it must never be evicted from");

        // 59 T1: since the pane now opens for a recording that has a substrate
        // but NO codeimage, Plane itself can be unavailable. It must STILL be
        // terminal — there is nowhere further to fall back to, and the pane's
        // own placard is what explains the empty plane. Evicting here would
        // loop.
        why[scene_kind_index(scene3d::SceneKind::Plane)] =
            "no address-space regions";
        check("evict/unavailable-plane-still-terminal",
              shell_evict_unavailable_kind(scene3d::SceneKind::Plane, why) ==
                  scene3d::SceneKind::Plane,
              "an unavailable Plane must not be evicted — it is the fallback, "
              "so leaving it has nowhere to go");
        why[scene_kind_index(scene3d::SceneKind::Plane)].clear();

        // A short/absent availability vector must not index out of range, and
        // must be read as "nothing is known to be unavailable".
        std::vector<std::string> empty;
        check("evict/short-vector-safe",
              shell_evict_unavailable_kind(scene3d::SceneKind::LanePrism,
                                           empty) ==
                  scene3d::SceneKind::LanePrism,
              "an availability vector that has not been sized yet must not "
              "evict, and must not read past its end");
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

    // ---- 37 T1 + 40 T2: the pager must name the REGION a pass covers -------
    // The producer stamps ONE region per invocation (asmspy.c dataflow_record),
    // so an `auto` candidate walk reaches a reader as several passes whose
    // regions differ. Paging then silently changes WHICH CODE is on screen, and
    // an ordinal alone ("pass 2 of 3") does not say so. The walk in this fixture
    // re-arms on span A twice before moving to B, so pass->region is NOT a
    // rotation of the region set: a label keyed on the pass ordinal
    // (regions[p % n]) yields A, B, A and is wrong for two of the three passes.
    {
        ShellState ps;
        std::string err;
        int ci = shell_open(ps, gd("auto-multi-region.asmtrace"), err);
        check("dfregion/open", ci >= 0, err.c_str());
        if (ci >= 0) {
            size_t i = static_cast<size_t>(ci);
            const SegmentedDataflow &seg = ps.seg_df[i];
            check("dfregion/three-passes", seg.passes.size() == 3,
                  "the candidate walk recorded three invocation passes");
            check("dfregion/two-regions", df_pass_regions(seg).size() == 2,
                  "three passes visiting two distinct spans");
            if (seg.passes.size() == 3) {
                std::string p0 = df_pass_desc(seg, 0);
                std::string p1 = df_pass_desc(seg, 1);
                std::string p2 = df_pass_desc(seg, 2);
                check("dfregion/pass0-names-A",
                      p0.find("0x100000") != std::string::npos, p0.c_str());
                // The walk re-arms on A: pass 1 is region A again, NOT the
                // second region an ordinal-keyed label would name here.
                check("dfregion/pass1-is-A-again",
                      p1.find("0x100000") != std::string::npos, p1.c_str());
                check("dfregion/pass1-is-not-B",
                      p1.find("0x110000") == std::string::npos,
                      "pass 1 re-armed on span A, not B");
                check("dfregion/pass2-names-B",
                      p2.find("0x110000") != std::string::npos, p2.c_str());
                check("dfregion/pass2-is-not-A",
                      p2.find("0x100000") == std::string::npos,
                      "pass 2 is span B alone");
            }
            // Reaching a region WITHOUT knowing its pass ordinal: "show me span
            // B" must not require the user to guess that B happens to live at
            // pass 3. A region maps to the passes that cover it.
            {
                std::vector<size_t> a_ = df_passes_for_region(seg, 0x100000);
                std::vector<size_t> b_ = df_passes_for_region(seg, 0x110000);
                check("dfregion/region-A-passes",
                      a_.size() == 2 && a_[0] == 0 && a_[1] == 1,
                      "span A was armed for passes 0 and 1");
                check("dfregion/region-B-passes", b_.size() == 1 && b_[0] == 2,
                      "span B was armed for pass 2 alone");
                // A region the recording never carried has no passes — an empty
                // answer, never a fallback to pass 0.
                check("dfregion/unknown-region-has-no-passes",
                      df_passes_for_region(seg, 0xdead0000).empty(),
                      "an unrecorded region must not resolve to a pass");
                // The live default within a region is its LATEST pass, matching
                // the pager's own follow-the-latest rule.
                check("dfregion/latest-pass-of-A",
                      df_latest_pass_for_region(seg, 0x100000) == 1,
                      "A's latest pass is 1, not its first");
                check("dfregion/latest-pass-of-B",
                      df_latest_pass_for_region(seg, 0x110000) == 2,
                      "B's only pass is 2");
                // Selecting a region resolves to a df_pass value directly, so
                // the pager's jump is testable rather than buried in a widget
                // callback. Region B's latest pass IS the recording's latest,
                // so choosing B is follow-the-latest (-1), not a pin that would
                // then freeze a live capture on a pass that stops being newest.
                int pin = 0;
                check("dfregion/select-B-follows-latest",
                      df_pass_pin_for_region(seg, 0x110000, &pin) && pin == -1,
                      "B's latest pass is the recording's latest");
                // Region A's latest pass is 1, behind the recording's latest,
                // so choosing A must PIN it.
                check("dfregion/select-A-pins-its-latest",
                      df_pass_pin_for_region(seg, 0x100000, &pin) && pin == 1,
                      "A's latest pass is 1 and must be pinned");
                // A region no pass covers resolves to nothing: the caller must
                // leave the current pass alone rather than jump somewhere.
                int untouched = 7;
                check("dfregion/select-unknown-is-a-no-op",
                      !df_pass_pin_for_region(seg, 0xdead0000, &untouched) &&
                          untouched == 7,
                      "an unrecorded region must not move the pager");
            }
        }
        // A single-region recording names no region: with nothing to disambiguate
        // the label would be chrome that never changes.
        int oi = shell_open(ps, gd("low-fidelity/continuous-df.asmtrace"), err);
        check("dfregion/singleregion-open", oi >= 0, err.c_str());
        if (oi >= 0) {
            const SegmentedDataflow &seg = ps.seg_df[static_cast<size_t>(oi)];
            check("dfregion/singleregion-no-region-chrome",
                  df_pass_desc(seg, 0).find("region") ==
                      std::string::npos,
                  "a recording with no stated region must claim none");
        }
    }

    // === 44-faithful-city-phase-a T3: scene_atmosphere_for_tier ==============
    // The load-bearing fidelity invariant (44's §6 point 9 / T3's own Tests
    // section): the sky's colour SOURCE must be byte-identical to whatever
    // the 2D fidelity banner reads (dt_warn_col/dt_refuse_col), never an
    // independently-chosen RGB literal. Asserted with no ImGui frame, no GL.
    {
        auto eq3 = [](const float a[3], const ImVec4 &b) {
            return a[0] == b.x && a[1] == b.y && a[2] == b.z;
        };
        auto eqf3 = [](const float a[3], const float b[3]) {
            return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
        };
        scene3d::Atmosphere neutral =
            scene_atmosphere_for_tier(FidelityTier::Neutral);
        check("atmo/neutral-front-is-dim-col", eq3(neutral.front, dt_dim_col()),
              "neutral tier's front colour must be the shared dim colour");
        check("atmo/neutral-no-fog", neutral.fog_density == 0.0f,
              "a clear/neutral sky must carry no fog");

        scene3d::Atmosphere caution =
            scene_atmosphere_for_tier(FidelityTier::Caution);
        check("atmo/caution-front-is-warn-col", eq3(caution.front, dt_warn_col()),
              "caution tier's front colour must be the SAME dt_warn_col() the "
              "2D banner reads — not an independently-chosen amber");

        scene3d::Atmosphere integrity =
            scene_atmosphere_for_tier(FidelityTier::Integrity);
        check("atmo/integrity-front-is-refuse-col",
              eq3(integrity.front, dt_refuse_col()),
              "integrity tier's front colour must be the SAME dt_refuse_col() "
              "the 2D banner reads — not an independently-chosen red");
        // Every tier's ambient sits on the SAME shared dark baseline.
        check("atmo/ambient-shared-baseline",
              eq3(neutral.ambient, dt_panel_bg_col()) &&
                  eq3(caution.ambient, dt_panel_bg_col()) &&
                  eq3(integrity.ambient, dt_panel_bg_col()),
              "every tier's ambient must read the same shared panel colour");
        // The three tiers are visibly distinct (T3's Done-when).
        check("atmo/three-tiers-distinct",
              !eqf3(neutral.front, caution.front) &&
                  !eqf3(caution.front, integrity.front) &&
                  !eqf3(neutral.front, integrity.front),
              "the three tiers must map to three visibly distinct front colours");
    }

    // === 44 T5: SceneFrame.sun is a pure function of hud.t / hud.nsteps ======
    {
        check("sun/t0", scene_sun_from_hud(0, 100) == 0.0f, "t=0 must be 0.0");
        check("sun/mid", scene_sun_from_hud(50, 100) == 0.5f,
              "t=nsteps/2 must be 0.5");
        check("sun/end", scene_sun_from_hud(100, 100) == 1.0f,
              "t=nsteps must be 1.0");
        check("sun/nsteps-zero-guard", scene_sun_from_hud(0, 0) == 0.5f,
              "nsteps==0 must return the fixed noon constant, never divide by "
              "zero");
    }

    // === 44 T5: the two Transports advance INDEPENDENTLY under Play ==========
    // Mirrors ui/transport.h's own header-only testability: advancing
    // SceneView::play must not move SceneView::follow_step, and vice versa —
    // the D7/doc34 anti-fusion rule the brief's Constraints & gates section
    // names explicitly.
    {
        SceneView sv;
        sv.play.playing = true;
        sv.play.steps_per_sec = 10.0f;
        sv.hud.t = 0;
        const uint64_t max_terrain = 1000;
        const uint64_t max_follow = 1000;
        sv.hud.t = transport_tick(sv.play, sv.hud.t, max_terrain, 1.0f);
        check("two-clocks/play-advanced", sv.hud.t > 0,
              "sv.play ticking a full second at 10/s must advance hud.t");
        check("two-clocks/follow-untouched-by-play", sv.follow_step == 0,
              "advancing sv.play must NOT move follow_step — the two clocks "
              "must never fuse");

        sv.follow_play.playing = true;
        sv.follow_play.steps_per_sec = 5.0f;
        const uint64_t hud_t_before = sv.hud.t;
        sv.follow_step =
            transport_tick(sv.follow_play, sv.follow_step, max_follow, 1.0f);
        check("two-clocks/follow-advanced", sv.follow_step > 0,
              "sv.follow_play ticking a full second at 5/s must advance "
              "follow_step");
        check("two-clocks/play-untouched-by-follow", sv.hud.t == hud_t_before,
              "advancing sv.follow_play must NOT move hud.t");
        // Different rates (10/s vs 5/s over the same 1s) prove they are truly
        // two SEPARATE Transport instances, not a shared one.
        check("two-clocks/independent-rates", sv.hud.t != sv.follow_step,
              "two Transports at different rates must diverge");
    }

    if (failures) {
        std::fprintf(stderr, "test_shell: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_shell: PASS\n");
    return 0;
}
