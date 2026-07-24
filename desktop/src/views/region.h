// region.h — the live region trace as discrete invocation snapshots
// (08-observer-views.md T6).
//
// **Never a scrub.** A region capture is not a continuous timeline: the engine
// arms a breakpoint at the region's entry, records ONE invocation, and waits
// for the next arrival. Between them the target does whatever it likes,
// unobserved, for an unknown length of time. A slider across that would draw a
// gap as if it were elapsed captured time — so this view pages between numbered
// snapshots and shows the gaps as what they are: separate invocations.
//
// The split is read from the STREAM's order, not invented: a region capture
// writes each invocation as [trace…][coverage], so a `coverage` event closes
// the snapshot before it. That is why Event carries `seq` (doc/recording.h) —
// by_kind alone cannot answer which instructions belonged to which invocation,
// and guessing (e.g. "restart at every offset 0") would silently merge two
// invocations of a routine whose first block runs twice.
//
// Two warnings ride with the view because they are properties of HOW it was
// captured, not opinions about it:
//
//  - **the crawl.** Every ptrace region/dataflow capture single-steps inside
//    the region. The target runs at native speed elsewhere and crawls in here,
//    which is a thing an operator watching a live process needs told before
//    they conclude their service has hung.
//  - **the JIT steer.** When the recording carries `codeimage` events the
//    region is JIT-tracked code, where single-stepping is worst and the
//    out-of-band IBS survey (mode `sample`) answers "what is hot" without
//    stopping anything.
#ifndef ASMDESK_VIEWS_REGION_H
#define ASMDESK_VIEWS_REGION_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/streams.h"
#include "views/observer.h"

namespace asmdesk {

// One invocation: the instructions recorded for it and its coverage footer.
struct RegionInvocation {
    size_t number = 1;           // 1-based, as shown ("invocation #3")
    std::vector<uint64_t> insns; // in execution order
    std::map<uint64_t, std::string> disasm; // D10 text, per offset
    std::vector<uint64_t> blocks;           // the de-duplicated block set
    uint64_t blocks_total = 0; // totals SEEN — they count past the caps
    uint64_t insns_total = 0;
    bool truncated = false; // this invocation's own coverage said so
    // No `coverage` footer closed this one: the capture stopped mid-invocation
    // (the region view's local form of torn). Shown as incomplete, never as a
    // short invocation that simply did less.
    bool closed = false;
    std::string basis; // "rel" | "abs"; never defaulted (schema)
};

struct RegionView {
    std::vector<RegionInvocation> invocations;
    size_t selected = 0; // the page index; paging is discrete, by design

    ObsChrome chrome;
    ObsSkip skip;
    std::string basis;       // the agreed basis across invocations
    std::string basis_error; // set when two invocations disagree

    std::string crawl_warning; // "" when the backend does not single-step
    std::string jit_hint;      // "" unless the region is JIT-tracked
};

RegionView obs_region_build(const Recording &r,
                            const ObsLifecycle *lc = nullptr);

// Page by ±n, clamped. Returns the new index. There is deliberately no
// "seek to fraction" — see the header comment.
size_t obs_region_page(RegionView &v, int delta);

// The selected invocation as 04's Streams, so the canvas renders a snapshot
// with the same code that renders a replayed recording — one canvas, not two.
Streams obs_region_snapshot_streams(const RegionView &v, size_t i);

// The crawl warning for a backend id, or "" when it does not single-step.
std::string obs_region_crawl_warning(const std::string &backend);

std::string obs_region_dump(const RegionView &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_REGION_H
