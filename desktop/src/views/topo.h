// topo.h — the process/thread topology map (08-observer-views.md T3).
//
// A `topo` snapshot is a flat task list; a person thinks in processes. So this
// view folds tasks into one CARD per process (its leader, its threads, its
// parent, its invocation total) and orders them by pid, which is stable in a
// way any activity-based ordering is not — a card that moves while you read it
// is a card you cannot point at.
//
// Two facts ride along with every card, and neither is decoration:
//
//  - **The jack is held TREE-WIDE.** `asmspy_engine_procs` SEIZEs the whole
//    descendant tree (`seize_process_tree`), so while a topology session runs
//    nothing else can attach to ANY of these processes — not just the one you
//    started from. The UI says so, because the alternative is the user
//    discovering it as an unexplained refusal somewhere else.
//
//  - **`inv` counts one of two different things.** `mode:"syscalls"` counts
//    syscalls (PTRACE_SYSCALL, near full speed); `mode:"calls"` counts CALL
//    instructions (single-step, and the whole tree crawls). The number is
//    meaningless without the word, so they travel together.
//
// Drill-in is a deep link through 04's router (never a direct reach into
// another view), which is what makes "show me this process" reproducible from
// a shell and pasteable into a bug.
#ifndef ASMDESK_VIEWS_TOPO_H
#define ASMDESK_VIEWS_TOPO_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "nav.h"
#include "views/observer.h"

namespace asmdesk {

// One task of a `topo` snapshot (mirrors `asmspy_task_t`).
struct TopoTask {
    long tid = 0, tgid = 0, ppid = 0;
    bool leader = false;
    std::string comm;
    std::string exe; // leader tasks only, by the engine's contract
    uint64_t inv = 0;
};

// One process: its leader's identity plus every task sharing its tgid.
struct TopoCard {
    long tgid = 0, ppid = 0;
    std::string comm;
    std::string exe;
    std::vector<TopoTask> threads; // ascending by tid; the leader is first
    uint64_t inv_total = 0;
};

struct TopoView {
    std::vector<TopoCard> cards; // ascending by tgid
    std::string count_mode;      // "syscalls" | "calls" — what `inv` counts
    size_t snapshots = 0; // `topo` events seen; the LAST one is what shows
    ObsChrome chrome;
    ObsSkip skip;
};

// Build from the LAST `topo` snapshot in the recording: a snapshot is a
// complete statement of the tree at a moment, so merging several would invent a
// tree that never existed at any one time.
TopoView obs_topo_build(const Recording &r, const ObsLifecycle *lc = nullptr);

// The card's one-line fingerprint: what it is, how many threads, and how much
// it did — with the unit named, never a bare count.
std::string obs_topo_fingerprint(const TopoView &v, const TopoCard &c);

// The tree-wide hold, stated for the budget UI. Non-empty always: this is a
// property of the engine, not of the current session's luck.
const char *obs_topo_jack_note();

// The drill-in link for a card: the syscall stream of THAT process, routed
// through 04's spine.
dt_link obs_topo_drill_link(const std::string &rec_id, const TopoCard &c);

std::string obs_topo_dump(const TopoView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_TOPO_H
