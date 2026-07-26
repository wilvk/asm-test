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
#include "views/slice_view.h" // 09-T5: assert the blame link's backward cone

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
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

    if (failures) {
        std::fprintf(stderr, "test_shell: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_shell: PASS\n");
    return 0;
}
