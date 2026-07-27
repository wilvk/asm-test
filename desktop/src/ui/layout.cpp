// layout.cpp — the shell's dock layout manager (13-foundation-moves.md T2).
// The single place the shell touches imgui_internal.h (DockBuilder). Listed as a
// first-party internal-header dependent in the doc-12 repin gate.
#include "ui/layout.h"

#include "imgui_internal.h" // DockBuilder*

namespace asmdesk {

const char *const kPaneHome = "Home";
const char *const kPaneRecording = "Recording";
const char *const kPaneScrubber = "Scrubber";
const char *const kPaneInspector = "Inspector";
const char *const kPaneTimeline = "Timeline";

const char *layout_preset_name(LayoutPreset p) {
    switch (p) {
    case LayoutPreset::ReplayInspect:
        return "Replay / Inspect";
    case LayoutPreset::Author:
        return "Author";
    case LayoutPreset::LiveObserver:
        return "Live observer";
    }
    return "?";
}

bool layout_exists(ImGuiID dockspace_id) {
    return ImGui::DockBuilderGetNode(dockspace_id) != nullptr;
}

DockLayout layout_build(ImGuiID dockspace_id, ImVec2 size, LayoutPreset preset) {
    DockLayout L;
    L.root = dockspace_id;

    // Destructive rebuild: remove the node and re-split. The caller gates this to
    // first-run / explicit reset — calling it every frame would stomp the user's
    // own docking each frame.
    ImGui::DockBuilderRemoveNode(dockspace_id);
    ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace_id, size);

    // A four-region split: left rail, right inspector, bottom timeline/scrubber,
    // and the central recording view that takes the rest.
    ImGuiID center = dockspace_id;
    L.left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr,
                                         &center);
    L.right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr,
                                          &center);
    L.bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.28f, nullptr,
                                           &center);
    L.center = center;

    // The deck/nav rail is always left, the active recording always center; the
    // preset decides what fills the bottom and right.
    ImGui::DockBuilderDockWindow(kPaneHome, L.left);
    ImGui::DockBuilderDockWindow(kPaneRecording, L.center);
    switch (preset) {
    case LayoutPreset::ReplayInspect:
        ImGui::DockBuilderDockWindow(kPaneScrubber, L.bottom);
        ImGui::DockBuilderDockWindow(kPaneInspector, L.right);
        break;
    case LayoutPreset::Author:
        ImGui::DockBuilderDockWindow(kPaneInspector, L.right);
        break;
    case LayoutPreset::LiveObserver:
        ImGui::DockBuilderDockWindow(kPaneTimeline, L.bottom);
        ImGui::DockBuilderDockWindow(kPaneInspector, L.right);
        break;
    }
    ImGui::DockBuilderFinish(dockspace_id);
    return L;
}

} // namespace asmdesk
