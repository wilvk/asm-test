// test_layout.cpp — the dock layout manager (13-foundation-moves.md T2), driven
// headless under the ImGui null backend with docking enabled. Asserts the split
// tree the manager builds, so the imgui_internal.h/DockBuilder wiring is pinned
// without a display.
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilderGetNode, to inspect the result

#include "ui/layout.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // DockBuilder needs it
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // Preset names are stable (menu labels + persisted keys).
    check("name/replay",
          std::string(layout_preset_name(LayoutPreset::ReplayInspect)) ==
              "Replay / Inspect",
          "replay preset name drifted");
    check("name/observer",
          std::string(layout_preset_name(LayoutPreset::LiveObserver)) ==
              "Live observer",
          "observer preset name drifted");

    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();

    ImGuiID dock = ImGui::GetID("test_dockspace");
    DockLayout L = layout_build(dock, ImVec2(1280, 720),
                                LayoutPreset::ReplayInspect);

    // Four distinct, non-zero regions were produced.
    check("split/root", L.root == dock, "root id changed");
    check("split/nonzero",
          L.left && L.center && L.right && L.bottom, "a region id is 0");
    check("split/distinct",
          L.left != L.center && L.left != L.right && L.left != L.bottom &&
              L.center != L.right && L.center != L.bottom && L.right != L.bottom,
          "regions are not distinct nodes");

    // The nodes actually exist in the dock tree.
    check("tree/center-exists", ImGui::DockBuilderGetNode(L.center) != nullptr,
          "center node missing after build");
    check("tree/bottom-exists", ImGui::DockBuilderGetNode(L.bottom) != nullptr,
          "bottom node missing after build");

    // (Which windows land in which region is imgui's own DockBuilderDockWindow
    // bookkeeping, exercised live in the app, not re-tested here through internal
    // ids — the manager's contract is the split tree above.)

    // A rebuild with a different preset is idempotent (no crash, still valid).
    DockLayout L2 = layout_build(dock, ImVec2(1280, 720),
                                 LayoutPreset::LiveObserver);
    check("rebuild/valid", ImGui::DockBuilderGetNode(L2.center) != nullptr,
          "a second preset build left an invalid tree");

    ImGui::EndFrame();
    ImGui::DestroyContext();

    if (failures) {
        std::fprintf(stderr, "test_layout: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_layout: all checks passed\n");
    return 0;
}
