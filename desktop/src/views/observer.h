// observer.h — the chrome every live Observer view puts on screen before it
// renders a single row (docs/internal/gui/08-observer-views.md).
//
// The six views in 08 render different things, but they all answer the same two
// questions first: how was this measured, and what is missing from it. That is
// two facts a view must not be free to re-word — a skip reason comes from the
// subsystem that measured it, and a truncation is a property of the recording,
// not of the renderer's mood — so both live here as pure functions of the
// Recording, shared by every view and asserted once.
//
// Engine-free and ImGui-free by construction (D4/D9): a live view and a replayed
// one are the same code over the same document model, which is how every view in
// this doc is tested on a host with nothing to attach to.
#ifndef ASMDESK_VIEWS_OBSERVER_H
#define ASMDESK_VIEWS_OBSERVER_H

#include <cstdint>
#include <string>

#include "doc/recording.h"

namespace asmdesk {

// The provenance + honesty facts an observer view shows above its rows.
struct ObsChrome {
    std::string backend; // the MEASURED producer id ("ibs-op", "hwdebug-watch")
    std::string trust;   // "exact" | "statistical" | "weak" | "strong"
    bool exact = true;   // false = a sample; it never renders as exact
    bool redacted = false; // payload withheld AT RECORD TIME (not in the file)

    bool truncated = false; // the footer said so, or the recording is torn
    bool torn = false;      // no `end` footer: the producer stopped mid-record
    uint64_t lost = 0;      // the drop record
    bool throttled = false;

    // "" when the recording is clean. Non-empty means it MUST be shown: a
    // banner that can be dismissed and forgotten while the numbers below it are
    // still wrong is worse than no banner at all.
    std::string banner;

    bool dropped() const { return lost > 0 || throttled; }
};

ObsChrome obs_chrome(const Recording &r);

// The one-line provenance chip: "ptrace-syscalls / exact" or
// "ibs-op / statistical". Statistical is never spelled the same way as exact —
// the schema's rule, applied at the only place a user reads it.
std::string obs_chrome_chip(const ObsChrome &c);

// The `session` lifecycle lines of a capture — WHEREVER they live, which is the
// point of this type.
//
// In a FILE they are ordinary lines the loader groups like any other kind: a
// reader of a sliced serve stream cannot tell them apart, and should not have
// to. In a LIVE session they are deliberately kept OUT of the growing recording
// (07-T3): they are not recording events, the `end` footer does not count them,
// and a client that folded them in would report a healthy session as one whose
// footer disagrees with its contents.
//
// Both are right, and a view must not care which it is looking at — a live
// capture and the file you slice out of it are the same session.
struct ObsLifecycle {
    std::vector<nlohmann::json> sessions; // `session` bodies, in arrival order
};

// The lifecycle a FILE carries in its own event groups.
ObsLifecycle obs_lifecycle_of(const Recording &r);

// The `params` echo of the last `started` event for `mode`, or nullptr. This is
// the EFFECTIVE parameters after defaulting — the only honest answer to "what
// is this session actually running with", which is never the same question as
// "what did the client ask for".
const nlohmann::json *obs_started_params(const ObsLifecycle &lc,
                                         const std::string &mode);

// A `session state:"skip"` lifecycle event (asmtrace-schema.md, Serve protocol).
//
// A skip is a SUCCESSFUL session that had nothing to report — never an error —
// and its `reason` is the text of whichever subsystem actually probed. Three
// different host facts hide behind one "watchpoint unavailable" (no regset, no
// slots, slots that refuse to reserve) and they send an operator to three
// different places, so the reason is carried and rendered VERBATIM.
struct ObsSkip {
    bool present = false;
    int code = 0;
    std::string reason;
    std::string mode;
};

// The skip for `mode`, or `present == false`. An empty `mode` matches any.
ObsSkip obs_skip(const ObsLifecycle &lc, const std::string &mode);
ObsSkip obs_skip(const Recording &r, const std::string &mode);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_OBSERVER_H
