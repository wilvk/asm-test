// test_perspectives.cpp — named dock perspectives + named filter presets
// (20-workspace-and-settings.md T4, F2/F16). Null backend with DockingEnable: a
// preset-backed perspective applied through perspective_apply reproduces a real
// dock split tree (assert with DockBuilderGetNode, as test_layout does); a filter
// preset's query is copied into a fixed buffer (model state); an unknown value
// degrades to a no-op rather than crashing.
#include <cstdio>
#include <cstring>
#include <string>

#include "imgui.h"
#include "imgui_internal.h" // DockBuilderGetNode

#include "ui/layout.h"
#include "ui/perspectives.h"

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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();

    // A built-in preset perspective encodes + applies to a REAL split tree.
    {
        std::string value = perspective_preset_value(
            layout_preset_name(LayoutPreset::ReplayInspect));
        check("preset-encodes", value == "preset:Replay / Inspect",
              "the preset perspective value must be self-describing");
        ImGuiID dock = ImGui::GetID("persp_dock");
        bool ok = perspective_apply(value, dock, ImVec2(1280, 720));
        check("preset-applies", ok, "a preset perspective must apply");
        check("preset-builds-tree",
              ImGui::DockBuilderGetNode(dock) != nullptr &&
                  !layout_needs_default(dock),
              "an applied preset perspective must reproduce a real split tree");
    }

    // An unknown value is a no-op, never a crash.
    {
        ImGuiID dock = ImGui::GetID("persp_dock2");
        check("unknown-noop",
              !perspective_apply("preset:Does Not Exist", dock,
                                 ImVec2(1280, 720)),
              "an unknown preset name must degrade to a no-op");
        check("garbage-noop",
              !perspective_apply("garbage-value", dock, ImVec2(1280, 720)),
              "a malformed perspective value must degrade to a no-op");
    }

    // A snapshot round-trips as an "ini:" value that re-applies without crash.
    {
        std::string snap = perspective_snapshot();
        check("snapshot-tagged", snap.rfind("ini:", 0) == 0,
              "a snapshot must be tagged ini:");
        ImGuiID dock = ImGui::GetID("persp_dock3");
        check("snapshot-applies",
              perspective_apply(snap, dock, ImVec2(1280, 720)),
              "an ini snapshot must apply");
    }

    // A filter preset copies its query into a fixed buffer (model state); a
    // too-long query truncates rather than overrunning.
    {
        char buf[96] = {0};
        size_t n = filter_preset_apply("read", buf, sizeof buf);
        check("filter-copies", std::strcmp(buf, "read") == 0 && n == 4,
              "a preset writes its query into the filter buffer");
        char small[4] = {0};
        filter_preset_apply("readwrite", small, sizeof small);
        check("filter-truncates", std::strcmp(small, "rea") == 0,
              "a too-long query truncates to the buffer, NUL-terminated");
    }

    ImGui::EndFrame();
    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "test_perspectives: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_perspectives: all checks passed\n");
    return 0;
}
