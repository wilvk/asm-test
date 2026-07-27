// perspectives.h — named dock perspectives + named filter presets
// (20-workspace-and-settings.md T4, F2/F16). Once the panes are real (the
// keystone, doc 19), the analyst wants to name and recall dock ARRANGEMENTS (the
// VS Code / Blender / JetBrains model) and repeated QUERIES. Both are workspace
// state, persisted in the T3 store (WorkspaceState::perspectives / ::presets), so
// they reuse the round-trip already tested there; this file is only the thin
// apply/snapshot seam over ImGui's own dock persistence + the layout manager.
//
// A perspective value is one of two forms, self-describing so an older build can
// still read a newer store:
//   "preset:<name>"  a built-in arrangement — re-split via layout_build; the
//                    three shipped presets seed the map.
//   "ini:<blob>"     a snapshot of the user's current DockBuilder layout, taken
//                    with ImGui::SaveIniSettingsToMemory and restored with
//                    LoadIniSettingsFromMemory.
#ifndef ASMDESK_UI_PERSPECTIVES_H
#define ASMDESK_UI_PERSPECTIVES_H

#include <cstddef>
#include <string>

#include "imgui.h" // ImGuiID / ImVec2

namespace asmdesk {

// Encode a built-in preset name as a perspective value ("preset:Replay / Inspect").
std::string perspective_preset_value(const char *preset_name);

// Snapshot the CURRENT dock layout as a perspective value ("ini:<blob>"). Wraps
// ImGui::SaveIniSettingsToMemory, so it captures the live DockBuilder tree.
std::string perspective_snapshot();

// Apply a perspective value to `dockspace_id`, sized to `size`. A "preset:" value
// re-splits via layout_build; an "ini:" value is loaded through ImGui's own dock
// deserialiser. Returns false (a no-op) on an unrecognised value — a corrupt
// store must never crash. Must be called inside a frame (DockBuilder needs the
// current window), exactly as the View menu's preset calls are.
bool perspective_apply(const std::string &value, ImGuiID dockspace_id,
                       ImVec2 size);

// Apply a filter preset's query into a fixed view filter buffer (the syscall
// name filter, the tree filter): a pure copy, capped to the buffer. The view is
// unchanged otherwise, so the "showing N of M" honesty stays exactly as the
// filter model renders it (D7). Returns the number of bytes written.
size_t filter_preset_apply(const std::string &query, char *buf, size_t buf_size);

} // namespace asmdesk
#endif // ASMDESK_UI_PERSPECTIVES_H
