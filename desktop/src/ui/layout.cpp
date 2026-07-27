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
const char *const kPaneLoom = "Loom";
const char *const kPaneObserver = "Observer";

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

bool layout_needs_default(ImGuiID dockspace_id) {
    ImGuiDockNode *node = ImGui::DockBuilderGetNode(dockspace_id);
    // Absent, or the empty leaf DockSpaceOverViewport just created: no real
    // layout yet, build the default. A split tree (persisted `.ini`) is kept.
    return node == nullptr || node->IsLeafNode();
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

    // The split SEQUENCE is identical across presets so the region node ids are
    // stable build-to-build — only the DockWindow targets differ per preset. That
    // is what lets a preset-switch test read a pane's DockId before and after and
    // assert it *moved* (19 T3): the ids it compares against do not themselves
    // shift. A five-region split: left rail, right inspector, a bottom row split
    // in two (timeline | scrubber, the 19 T3.1 change so both show at once), and
    // the central recording view (with the Loom co-docked) that takes the rest.
    ImGuiID center = dockspace_id;
    L.left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.20f, nullptr,
                                         &center);
    L.right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.28f, nullptr,
                                          &center);
    L.bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr,
                                           &center);
    L.bottom2 = ImGui::DockBuilderSplitNode(L.bottom, ImGuiDir_Right, 0.50f,
                                            nullptr, &L.bottom);
    L.center = center;

    // The deck/nav rail is always left; the active recording (and the Loom,
    // co-docked as a tear-able tab beside it) is always center; the preset decides
    // what fills the bottom row, the right rail and where the Observer deck lands.
    ImGui::DockBuilderDockWindow(kPaneHome, L.left);
    ImGui::DockBuilderDockWindow(kPaneRecording, L.center);
    ImGui::DockBuilderDockWindow(kPaneLoom, L.center);
    ImGui::DockBuilderDockWindow(kPaneInspector, L.right);
    switch (preset) {
    case LayoutPreset::ReplayInspect:
    case LayoutPreset::Author:
        // Reading-first: timeline + scrubber fill the bottom row together, the
        // Observer deck co-docks with the inspector on the right.
        ImGui::DockBuilderDockWindow(kPaneTimeline, L.bottom);
        ImGui::DockBuilderDockWindow(kPaneScrubber, L.bottom2);
        ImGui::DockBuilderDockWindow(kPaneObserver, L.right);
        break;
    case LayoutPreset::LiveObserver:
        // Observer-first: the deck takes the prominent bottom-left, the timeline
        // slides to the bottom-right and the scrubber co-docks on the right — so
        // every pane's node differs from the reading preset above (the moved-pane
        // assertion in 19 T3).
        ImGui::DockBuilderDockWindow(kPaneObserver, L.bottom);
        ImGui::DockBuilderDockWindow(kPaneTimeline, L.bottom2);
        ImGui::DockBuilderDockWindow(kPaneScrubber, L.right);
        break;
    }
    ImGui::DockBuilderFinish(dockspace_id);
    return L;
}

} // namespace asmdesk
