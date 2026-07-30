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
    ImGuiID root = 0;    // the dockspace itself
    ImGuiID left = 0;    // the deck / nav rail
    ImGuiID center = 0;  // the active recording / main view (+ Loom co-docked)
    ImGuiID right = 0;   // the inspector (ABI x-ray / backends / capability)
    ImGuiID bottom = 0;  // the timeline (bottom-left half)
    ImGuiID bottom2 = 0; // the scrubber (bottom-right half) — the T3.1 split, so
                         // the timeline AND the scrubber are visible together
};

// Build (or rebuild) the default split for `preset` into `dockspace_id`, sized
// to `size`. Destructive: it removes and rebuilds the node, so call it only on
// first run or an explicit "reset" — never every frame (that would fight the
// user's manual docking). Returns the region node ids.
DockLayout layout_build(ImGuiID dockspace_id, ImVec2 size, LayoutPreset preset);

// The preset's human name (menu label + the persisted-layout key).
const char *layout_preset_name(LayoutPreset p);

// Reset to the shipped default (18-breach-stops.md T2, F2). Distinct from
// layout_build as an INTENT — it is the always-available recovery the keybinding
// and the auto-fallback call — but it rebuilds the same ReplayInspect default,
// removing a corrupt/collapsed node and re-splitting. Thin + headless-testable,
// so test_layout drives it without a display.
void layout_reset(ImGuiID dockspace_id, ImVec2 size);

// Is ANY of the layout's panes still visible (18-breach-stops.md T2)? A pure
// predicate over the dock node tree: true when at least one leaf node under
// `dockspace_id` holds a docked window, false when the tree is absent or every
// node is empty — the "zero visible panes" strand a corrupt/collapsed persisted
// `.ini` makes reachable, which the shell auto-falls-back from. Testable on
// synthetic node state (build a layout with panes vs. remove the node).
bool layout_any_pane_visible(ImGuiID dockspace_id);

// Whether a dock node already exists for `dockspace_id` (i.e. a layout was
// persisted / already built). Wraps DockBuilderGetNode so the shell need not
// include imgui_internal.h itself — the containment the doc-12 gate relies on.
bool layout_exists(ImGuiID dockspace_id);

// Whether `dockspace_id` needs the default split built into it: true when the
// node is absent or a bare leaf (a fresh dockspace — DockSpaceOverViewport
// creates exactly such an empty leaf, so `layout_exists` alone is NOT enough to
// decide "already laid out"), false once it carries a real split (a persisted
// `.ini` layout, which must be kept). The one accurate first-run signal — again
// wrapping the imgui_internal.h `ImGuiDockNode` so the shell stays free of it.
bool layout_needs_default(ImGuiID dockspace_id);

// The window names the layout docks — the SAME strings the shell passes to
// ImGui::Begin, so the two cannot drift. Exposed for the tests.
extern const char *const kPaneHome;
extern const char *const kPaneRecording;
extern const char *const kPaneScrubber;
extern const char *const kPaneInspector;
extern const char *const kPaneTimeline;
// Two views carry their own inner tab bar (the Loom's `loom-detail`, the
// Observer deck's `observer`), so they must be their OWN docked pane — nesting
// them inside another pane's tab strip would rebuild the 3-deep exclusive stack
// this brief (19) tears down. They dock alongside the five regions above.
extern const char *const kPaneLoom;
extern const char *const kPaneObserver;
// The Inspect / live-capture workflow, split into dockable panes (replacing the
// single Inspect door window): the host Connect pane, the searchable Processes
// target picker, and the Live-capture controls (patch bay + region + Start).
// Each opens on demand, closes, undocks, and appears in View ▸ Panels.
extern const char *const kPaneConnect;
extern const char *const kPaneProcesses;
extern const char *const kPaneCapture;
// The capture surface's split-out panes: the colored session Log (docked in the
// bottom region beside the timeline), Save capture (docked beside the Live-capture
// controls), and the PT slice — the hardware-recorded def-use slice, its OWN pane
// and CONTEXT-GATED to an Intel PT host (it needs PT silicon to have a path to
// replay). Each opens on demand and appears in View ▸ Panels.
extern const char *const kPaneLog;
extern const char *const kPaneSave;
extern const char *const kPanePtSlice;

} // namespace asmdesk
#endif // ASMDESK_UI_LAYOUT_H
