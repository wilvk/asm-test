// layout.h — the shell's dock layout manager (docs/internal/gui/
// 13-foundation-moves.md T2).
//
// The ONLY imgui_internal.h consumer in the shell: DockBuilder is deliberately
// internal and will churn in the announced docking rewrite, so ALL of that
// exposure is contained in layout.cpp (and layout.cpp is listed as a first-party
// internal-header dependent in the doc-12 repin gate). Nothing here decides a
// product rule — it only arranges named windows into a default split, so the
// arrangement is unit-testable headless (D4) without any rendering.
#ifndef ASMDESK_UI_LAYOUT_H
#define ASMDESK_UI_LAYOUT_H

#include "imgui.h" // for ImGuiID / ImVec2

namespace asmdesk {

// Per-mode presets — the plan's Learn/Author/Inspect vs replay vs live-observer
// split (F1). Each places a different set of panes; the regions are the same.
enum class LayoutPreset { ReplayInspect, Author, LiveObserver };

// The dock node ids of the default split, so the shell docks its panes into them
// and the tests assert the tree.
struct DockLayout {
    ImGuiID root = 0;   // the dockspace itself
    ImGuiID left = 0;   // the deck / nav rail
    ImGuiID center = 0; // the active recording / main view
    ImGuiID right = 0;  // the inspector (diff / capability)
    ImGuiID bottom = 0; // the timeline / scrubber
};

// Build (or rebuild) the default split for `preset` into `dockspace_id`, sized
// to `size`. Destructive: it removes and rebuilds the node, so call it only on
// first run or an explicit "reset" — never every frame (that would fight the
// user's manual docking). Returns the region node ids.
DockLayout layout_build(ImGuiID dockspace_id, ImVec2 size, LayoutPreset preset);

// The preset's human name (menu label + the persisted-layout key).
const char *layout_preset_name(LayoutPreset p);

// Whether a dock node already exists for `dockspace_id` (i.e. a layout was
// persisted / already built). Wraps DockBuilderGetNode so the shell need not
// include imgui_internal.h itself — the containment the doc-12 gate relies on.
bool layout_exists(ImGuiID dockspace_id);

// The window names the layout docks — the SAME strings the shell passes to
// ImGui::Begin, so the two cannot drift. Exposed for the tests.
extern const char *const kPaneHome;
extern const char *const kPaneRecording;
extern const char *const kPaneScrubber;
extern const char *const kPaneInspector;
extern const char *const kPaneTimeline;

} // namespace asmdesk
#endif // ASMDESK_UI_LAYOUT_H
