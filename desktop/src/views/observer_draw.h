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

void draw_obs_syscalls(SyscallView &v);
void draw_obs_watch(const WatchView &v);
// `go` routes a drill-in through 04's router; `rec_id` is the link's recording.
void draw_obs_topo(const TopoView &v, const std::string &rec_id,
                   const std::function<void(const dt_link &)> &go);
void draw_obs_hotedges(const HotEdgeView &v);
// Returns a `start` command line when the user asks for one, else "". The
// caller sends it — this file never touches a session.
std::string draw_obs_tree(const TreeView &v, ObserverState &s, long pid);
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
