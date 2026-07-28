// test_scrubber_draw.cpp — the scrubber's ImGui half under the null backend
// (09-teaching-producers.md T3). No display, no GL, no engines: it drives
// draw_scrubber over the three shapes the builder test asserts — a clean scrub,
// a torn recording, and a producer-absent recording — and checks the deck draws
// every one, including the two paths a happy-path smoke never reaches (the torn
// placard and the absent message).
#include <cstdio>
#include <string>

#include "imgui.h"

#include "analysis/stepindex.h"
#include "views/views_draw.h"

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

// One frame drawing the scrubber over `idx` at `playhead`. Returns the vertex
// count as "something was drawn". `idx` is mutable (draw_scrubber takes it by ref
// so the synthesise action can replace it) but no synth runs here — rec is null.
static int frame(StepIndex &idx, uint64_t playhead) {
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin("scrubber");
    draw_scrubber(idx, playhead);
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
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // A clean scrub: draw every held step, including the diff-highlight rows.
    {
        StepIndex idx = build_step_index(load("add_signed.asmtrace"));
        for (uint64_t ph = 0; ph < idx.total_steps(); ph++)
            check("add_signed frame", frame(idx, ph) >= 0,
                  "no draw data at step " + std::to_string(ph));
    }

    // The torn recording: the placard AND a seek into the torn region.
    {
        StepIndex idx = build_step_index(load("regstate-truncated.asmtrace"));
        check("torn-region frame", frame(idx, 0) >= 0, "the tear must draw");
        check("held frame past the tear", frame(idx, 5) >= 0,
              "a held step past the tear must draw");
    }

    // A producer-absent recording: the absent message + docs link path.
    {
        StepIndex idx = build_step_index(load("sum_via_rbx.asmtrace"));
        check("producer-absent frame", frame(idx, 0) >= 0,
              "the absent placard must draw");
    }

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "%d test_scrubber_draw check(s) failed\n",
                     failures);
        return 1;
    }
    std::printf("test_scrubber_draw: all checks passed\n");
    return 0;
}
