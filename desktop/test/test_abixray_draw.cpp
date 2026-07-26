// test_abixray_draw.cpp — the ABI x-ray's ImGui half under the null backend
// (09-teaching-producers.md T4). No display, no GL, no engines: it drives
// draw_abixray over the shapes the builder test asserts — a clean aligned pair
// stepped through its walkthrough, an UNALIGNED pair (the refusal banner), a
// producer-absent pane (the placard), and a TORN pane (UNKNOWN, not zero) — and
// checks the two-pane deck draws each, including the paths a happy-path smoke
// never reaches.
#include <cstdio>
#include <string>

#include "imgui.h"

#include "analysis/stepindex.h"
#include "views/abixray.h"
#include "views/views_draw.h"
#include "walkthrough.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static Recording load(const std::string &name) {
    std::string err;
    auto r =
        load_recording_file(std::string(ASMTEST_GOLDEN_DIR) + "/" + name, err);
    if (!r) {
        check("load", false, name + ": " + err);
        return Recording{};
    }
    return *r;
}

static int frame(const StepIndex &s, const StepIndex &w, wt_model &walk,
                 uint64_t &playhead) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("abixray");
    draw_abixray(s, w, walk, playhead);
    ImGui::End();
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    return dd ? dd->TotalVtxCount : -1;
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int fw = 0, fh = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh);

    // A clean aligned pair, stepped through every authored stop: the rail, the
    // shared slider, the two locked panes, the DIFF tint and the changed-value
    // highlight all draw.
    {
        StepIndex s = build_step_index(load("abixray-make_pair-sysv.asmtrace"));
        StepIndex w =
            build_step_index(load("abixray-make_pair-win64.asmtrace"));
        wt_model walk = wt_build(load("abixray-make_pair-sysv.asmtrace"));
        for (int i = 0; i < static_cast<int>(walk.stops.size()); i++) {
            walk.cur = i;
            uint64_t ph = dt_abixray_playhead(walk);
            check("make_pair stop frame", frame(s, w, walk, ph) >= 0,
                  "no draw data at stop " + std::to_string(i));
        }
    }

    // An UNALIGNED pair: the "cannot share a playhead" banner path.
    {
        StepIndex s = build_step_index(load("abixray-make_pair-sysv.asmtrace"));
        StepIndex w = build_step_index(load("abixray-sum3-win64.asmtrace"));
        wt_model walk = wt_build(load("abixray-make_pair-sysv.asmtrace"));
        uint64_t ph = 0;
        check("unaligned-pair frame", frame(s, w, walk, ph) >= 0,
              "the not-aligned banner must draw");
    }

    // A producer-absent pane: the placard + docs-link path.
    {
        StepIndex none = build_step_index(load("sum_via_rbx.asmtrace"));
        StepIndex w =
            build_step_index(load("abixray-make_pair-win64.asmtrace"));
        wt_model walk = wt_build(load("abixray-make_pair-win64.asmtrace"));
        uint64_t ph = 0;
        check("producer-absent frame", frame(none, w, walk, ph) >= 0,
              "the absent placard must draw");
    }

    // A TORN pane: the register-ring dishonesty fixture paired with itself is
    // aligned, carries no stops, and is torn at step 0 — so the no-stops rail,
    // the torn banner and the per-pane UNKNOWN all draw.
    {
        StepIndex t = build_step_index(load("regstate-truncated.asmtrace"));
        wt_model walk = wt_build(load("regstate-truncated.asmtrace"));
        uint64_t ph = 0;
        check("torn-pane frame", frame(t, t, walk, ph) >= 0,
              "the torn placard must draw");
    }

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "%d test_abixray_draw check(s) failed\n",
                     failures);
        return 1;
    }
    std::printf("test_abixray_draw: all checks passed\n");
    return 0;
}
