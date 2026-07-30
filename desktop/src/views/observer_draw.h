// observer_draw.h — the thin ImGui half of the live Observer views
// (08-observer-views.md T1-T7). Same split as views_draw.h and loom_draw.h:
// every function here takes an ALREADY-BUILT model and only draws it, and the
// build enforces it — the model TUs link into their own tests with no ImGui, so
// a builder that reached for a widget would fail to link.
//
// The one place a draw here does more than draw is interaction state (which
// payload rows are revealed, which invocation is paged to). That state lives in
// the model structs, is set through the model's own functions, and survives
// across frames in ObserverState below.
#ifndef ASMDESK_VIEWS_OBSERVER_DRAW_H
#define ASMDESK_VIEWS_OBSERVER_DRAW_H

#include <functional>
#include <string>

#include "doc/recording.h"
#include "nav.h"
#include "views/disasm.h"
#include "views/hotedges.h"
#include "views/region.h"
#include "views/syscalls.h"
#include "views/topo.h"
#include "views/tree.h"
#include "views/watch.h"

namespace asmdesk {

// Every Observer view over one recording, built once (at open, or when a live
// session's recording grows) rather than per frame — and holding the
// interaction state that must survive between frames.
struct ObserverState {
    SyscallView syscalls;
    WatchView watch;
    TopoView topo;
    HotEdgeView hotedges;
    TreeView tree;
    RegionView region;
    DisasmView disasm;

    // The tree filter panel's edit buffers: what the NEXT session would be
    // started with (the current one's effective filter is in `tree.effective`).
    TreeFilter filter;
    char focus_buf[128] = {0};
    char module_buf[128] = {0};

    // The disassembly pane's logical time. 0 = the latest version, mirroring
    // asmtest_codeimage_bytes_at's own convention.
    uint64_t disasm_when = 0;

    // The syscall pane's client-side name filter (16 T2). UI state, not model:
    // it selects which rows draw_obs_syscalls DRAWS (via obs_syscall_filter_
    // indices), never mutating SyscallView. Per-deck, so each recording's pane
    // keeps its own filter.
    char syscall_filter[96] = {0};

    // Client-side type-to-narrow filters for the hot-edges + disasm lists
    // (22-selection-and-search.md T3 step 4): they narrow the DISPLAY only (each
    // draw keeps every model row and shows "showing N of M"), so a filtered list
    // never implies the hidden rows do not exist (D7) — the same fidelity-safe
    // idiom as the syscall filter. Deliberately NOT added to the call TREE, which
    // stays engine-filtered so surviving depths never lie (tree.h, D7).
    char hotedges_filter[96] = {0};
    char disasm_filter[96] = {0};

    // Which graph the shared "Graph" tab (15 T3) is showing: 0 = topology,
    // 1 = hot-edges, 2 = call tree. The tab offers only the KINDS this recording
    // actually has and re-homes the selection onto an available one, so a stored
    // index whose data vanished (a live recording changing shape between frames)
    // never leaves an empty canvas.
    int graph_sel = 0;

    bool built = false;
};

// Rebuild every view from `r`. Cheap at session scale and pure, so the caller
// decides when — never once per frame.
//
// `lc` is where the session lifecycle lives when it is NOT in the recording: a
// live host keeps `session`/`cmd`/`err` out of the growing recording (07-T3),
// because they are not recording events and the footer does not count them. A
// replayed file has them inline and passes nullptr. Same views either way —
// that equivalence is the doc's whole premise, so it is one parameter rather
// than a second code path.
void observer_build(ObserverState &s, const Recording &r,
                    const ObsLifecycle *lc = nullptr);

// Which views have anything to show for this recording — so the UI offers the
// tabs the capture actually produced rather than a row of empty panes.
bool observer_has_any(const ObserverState &s);

// The graph views (topo / tree / hotedges) draw on a node-editor pan/zoom canvas
// with selection + fit-graph (15-plotting-and-graph-nav.md T3). Node-editor
// reaches into ImGui internals and is APP-ONLY, exactly as the ImPlot chassis is
// (main.cpp creates the context; the headless view tests create none). This flag
// is the same guard: main.cpp enables it, the null-backend tests leave it off,
// so those draws fall back to the plain list/table and never enter node-editor.
void obs_graph_enable(bool on);
bool obs_graph_enabled();

void draw_obs_syscalls(SyscallView &v, ObserverState &s);
void draw_obs_watch(const WatchView &v);
// `go` routes a node click / drill-in through 04's router; `rec_id` is the
// link's recording. topo / hotedges / tree all take it so a clicked graph node
// navigates rather than reaching into another view (D4).
void draw_obs_topo(const TopoView &v, const std::string &rec_id,
                   const std::function<void(const dt_link &)> &go);
void draw_obs_hotedges(const HotEdgeView &v, ObserverState &s,
                       const std::string &rec_id,
                       const std::function<void(const dt_link &)> &go);
// The tree-capture filter editor (depth/focus/module/tid/follow): the patch bay
// draws it beside the mode picker, so a `tree` capture's engine-side filter is
// configured where every other mode's config lives. Returns the filter validation
// error ("" = legal) so the caller can gate its single Start on it; the illegal
// case is also shown inline in red. Pure input state — no built recording needed.
std::string draw_tree_filter(ObserverState &s);
// The call-tree preview (the effective filter the session ran with, then the
// indented call list). A pure view over the built TreeView — the filter EDITOR is
// draw_tree_filter above, so the Observer deck's Tree tab is this preview alone.
void draw_obs_tree(const TreeView &v);
void draw_obs_region(RegionView &v);
void draw_obs_disasm(const DisasmView &v, ObserverState &s);

// The whole Observer deck over one recording, as a tab bar.
void draw_observer(ObserverState &s, const Recording &r,
                   const std::string &rec_id,
                   const std::function<void(const dt_link &)> &go);

// The lifecycle a LIVE session keeps beside its recording, in the shape the
// views read. Defined here rather than in live/session.h so the view layer
// never depends on the capture host.
ObsLifecycle observer_lifecycle_from(const std::vector<nlohmann::json> &notes);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_OBSERVER_DRAW_H
