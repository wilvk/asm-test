// test_shell.cpp — the shell's honesty behaviour + a headless render smoke
// (03-desktop-shell.md T6). shell_banner is D7 as behaviour: non-null for a
// truncated / dropped / torn recording, null for a clean one. Then draw_shell is
// driven for 3 null-backend frames over a Workspace of the fixtures to prove no
// path crashes without a display. No GLFW, no GL, no engines.
#include <cstdio>
#include <string>

#include "imgui.h"

#include "doc/recording.h"
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

    if (failures) {
        std::fprintf(stderr, "test_shell: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_shell: PASS\n");
    return 0;
}
