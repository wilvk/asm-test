// workspace_state.h — the persisted Workspace (20-workspace-and-settings.md T3,
// F10; T4 perspectives/presets). Before this nothing was remembered between
// launches: no reopen-last-workspace, no MRU, no drag-drop. This is the pure
// serialise/deserialise of that memory to a small JSON stored beside the dock
// `.ini` (build/desktop-workspace.json). Every position it stores is an
// `asmtrace-link` (nav.h), so a save is diffable and forward-compatible (unknown
// keys ignored, nav.h:15-17) — a workspace saved by a newer build still restores
// on an older one, and a corrupt store degrades to defaults rather than crashing.
//
// Engine-free and ImGui-free: only the standard library + nlohmann/json, so the
// round-trip is asserted headless (test_workspace_state).
#ifndef ASMDESK_DOC_WORKSPACE_STATE_H
#define ASMDESK_DOC_WORKSPACE_STATE_H

#include <map>
#include <string>
#include <vector>

namespace asmdesk {

// A named saved filter/query preset (T4, F16). A preset names a filter string —
// e.g. the syscall name filter — that a view's filter buffer is set to; the view
// is unchanged otherwise, so a preset is chassis over the existing pure filter
// model and never reads as "the trace only did these".
struct FilterPreset {
    std::string name;
    std::string query;
};

// The whole persisted workspace. `open` is the recording paths; `active` is the
// dt_nav_format of the active position; `pane_links` is one asmtrace-link per
// open recording's per-pane selection. `recents` is the MRU list (most-recent
// first). `perspectives` (T4) maps a name to a serialised dock layout — either
// "preset:<name>" for a built-in arrangement or "ini:<blob>" for a snapshot of
// the user's current DockBuilder layout. `presets` (T4) are the named filters.
struct WorkspaceState {
    std::vector<std::string> open;
    std::string active;
    std::vector<std::string> pane_links;
    std::vector<std::string> recents;
    std::map<std::string, std::string> perspectives;
    std::vector<FilterPreset> presets;
};

// Serialise to a small, stable JSON (sorted keys via nlohmann's ordered dump).
std::string workspace_state_serialize(const WorkspaceState &s);

// Parse defensively: on any malformed input `out` is left at defaults and the
// function returns false — a corrupt/stale store must never crash the app (the
// F2 note that a persisted `.ini` made an upstream crash newly reachable applies
// here too). Unknown keys are ignored (forward-compat).
bool workspace_state_parse(const std::string &json, WorkspaceState &out);

// Push `path` onto an MRU list: move-to-front, dedup, cap (default 12). Pure and
// unit-tested directly (T3 step 4).
void recents_push(std::vector<std::string> &recents, const std::string &path,
                  size_t cap = 12);

} // namespace asmdesk
#endif // ASMDESK_DOC_WORKSPACE_STATE_H
