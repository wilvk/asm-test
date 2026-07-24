// tree.h — the live/replay call tree and its filter panel
// (08-observer-views.md T5).
//
// This is the day-one place the GUI exceeds the TUI, and it costs no engine
// work: `asmspy_tree_filter_t` (depth / focus / module) has existed all along
// and the TUI passes NULL for it. The filter is ENGINE-side by design and that
// is the whole point — it bounds what the engine EMITS while it keeps tracking
// every call and return, so the depths on the surviving lines stay true. A
// client-side filter over an unfiltered stream would show the same rows with
// depths that no longer mean anything (and would still pay for every line on
// the wire).
//
// Which is why the panel here does not filter anything: it builds the `start`
// parameters for the next session, and refuses the combinations the protocol
// refuses — with the same words, so the client and the serve loop cannot
// disagree about what is legal. A UI that let you pull a lever the server was
// always going to reject has taught you nothing.
#ifndef ASMDESK_VIEWS_TREE_H
#define ASMDESK_VIEWS_TREE_H

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "doc/recording.h"
#include "views/observer.h"

namespace asmdesk {

// One `call` event (schema `call`, mirroring `asmspy_tree_call_t`).
struct TreeRow {
    long tid = 0;
    int depth = 0; // EFFECTIVE depth: re-based by a focus filter, engine-side
    uint64_t addr = 0;
    std::string name;   // resolved symbol, or "0x…"
    std::string module; // module basename, "jit", or "?"
};

// The filter panel's state == the engine's filter, one field per parameter.
struct TreeFilter {
    // 1..1000; 0 means "omit the parameter", which is the engine's unlimited.
    // `depth:0` on the wire asks for a tree with no levels and is refused.
    int depth = 0;
    std::string focus;   // re-bases the tree so this function sits at depth 0
    std::string module;  // only calls into this module basename
    long tid = 0;        // pin ONE task; 0 = every thread
    bool follow = false; // also follow child PROCESSES
};

inline constexpr int kTreeDepthMax = 1000;

struct TreeView {
    std::vector<TreeRow> rows;
    ObsChrome chrome;
    ObsSkip skip;

    // What the session was actually started with, off the `started` params
    // echo. A client must never assume its own request round-tripped: the echo
    // is the effective parameters after defaulting, and it is the only honest
    // source for what the engine ran.
    TreeFilter effective;
    bool have_effective = false;
};

TreeView obs_tree_build(const Recording &r, const ObsLifecycle *lc = nullptr);

// Distinct tids present, ascending — the tree is per-thread and a reader needs
// to know how many threads' calls are interleaved in front of them.
std::vector<long> obs_tree_tids(const TreeView &v);

// Validate the panel. Returns "" when the filter is legal, else the refusal —
// VERBATIM the protocol's own wording (asmtrace-schema.md, "Refusals").
std::string obs_tree_filter_error(const TreeFilter &f);

// The `start` command's params, omitting anything left at its default (the
// protocol takes an omitted parameter as the subcommand default, so sending
// `depth:0` and omitting `depth` are different requests).
nlohmann::json obs_tree_start_params(const TreeFilter &f);

// The whole `{"cmd":"start","mode":"tree",...}` line for LiveSession::send.
// Empty when the filter is illegal — the client blocks first, and the serve
// loop's identical refusal stays a backstop rather than the user's error
// message.
std::string obs_tree_start_command(const TreeFilter &f, long pid);

std::string obs_tree_dump(const TreeView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_TREE_H
