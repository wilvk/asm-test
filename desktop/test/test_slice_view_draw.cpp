// test_slice_view_draw.cpp — null-backend smoke for the canvas-wrapped slice
// explorer (15-plotting-and-graph-nav.md T2). The draw half had no test before;
// imgui_canvas works headless (draw lists only), so this pins that a wide slice
// renders inside the canvas across frames without crashing and produces draw
// data — and that the empty (early-return) path is safe too.
#include <cstdio>

#include "imgui.h"

#include "views/slice_view.h"
#include "views/views_draw.h"

using namespace asmdesk;

static int fails;
static void check(const char *what, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        fails++;
    }
}

static void frame(const dt_slice_view &v, const char *win) {
    ImGui::GetIO().DisplaySize = ImVec2(800, 600);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Begin(win);
    draw_slice_view(v); // wraps ImGuiEx::Canvas
    ImGui::End();
    ImGui::Render();
    check("draw produced data (no crash under the canvas)",
          ImGui::GetDrawData() != nullptr);
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // A small def-use slice: 3 nodes across 3 columns, 2 arcs on distinct lanes.
    dt_slice_view v;
    v.nodes = {{0, 0, "a", dt_cone::back, 0},
               {2, 0, "b", dt_cone::both, 1},
               {4, 0, "c", dt_cone::fwd, 2}};
    v.edges = {{0, 2, 0, ""}, {2, 4, 1, ""}};
    v.selected_step = 2;

    for (int n = 0; n < 3; n++)
        frame(v, "slice");

    // The empty case takes the early-return (no canvas); must be safe too.
    dt_slice_view empty;
    frame(empty, "slice-empty");

    ImGui::DestroyContext();
    if (fails) {
        std::fprintf(stderr, "test_slice_view_draw: %d FAILURE(S)\n", fails);
        return 1;
    }
    std::printf("test_slice_view_draw: all checks passed\n");
    return 0;
}
