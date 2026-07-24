// walkthrough.h — the Learn door's pure view-model
// (docs/internal/gui/06-doors-and-learning.md T4).
//
// A walkthrough is a RECORDING with ordered `stop:true` notes in it, so the
// player is a reader of the same format every other view reads. This header is
// the decode: events in, stops + navigation out. No ImGui, no I/O — the
// cli/asmspy_logview.h / cli/test_view.c pattern, so every rule below is a
// headless assertion.
//
// TWO HONESTY RULES, both structural:
//
//  1. A stop anchored past the recorded window is REFUSED, never clamped. A
//     truncated recording's last stop may point at a step nobody recorded;
//     silently moving the player to the last step it does have would show the
//     reader an instruction and tell them a story about a different one.
//  2. The truncation banner is the model's, not the door's. `truncated` comes
//     off the recording, so a player cannot forget to render it.
#ifndef ASMDESK_WALKTHROUGH_H
#define ASMDESK_WALKTHROUGH_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"

namespace asmdesk {

struct wt_stop {
    int ordinal = 0;       // 1-based, in file order — the schema's contract
    long step_anchor = -1; // -1 = unanchored (about the run, not a step)
    std::string title;
    std::string body;
    std::string expected; // "" when this stop is not a failure framing
    std::string got;
    bool has_framing() const { return !expected.empty() || !got.empty(); }
};

struct wt_model {
    std::string id;    // the recording's basename — the deep-link key
    std::string title; // the recording's leading un-stopped `note`, if any
    std::vector<wt_stop> stops;
    int cur = 0;

    // Off the recording, never re-derived.
    bool truncated = false;
    bool torn = false;
    uint32_t steps_recorded = 0;

    // False when `step` is outside what this recording actually holds. The
    // player renders "stop is beyond the recorded window" and refuses to move.
    bool anchor_in_window(long step) const;

    bool next();
    bool prev();
    const wt_stop *current() const;
};

// Decode one loaded Recording into a walkthrough. A recording with no
// `stop:true` notes yields an EMPTY stop list — it is a recording, just not a
// walkthrough, and the door says so rather than inventing stops from the trace.
wt_model wt_build(const Recording &r);

// Deterministic one-line-per-fact dump — the test surface.
std::string wt_dump(const wt_model &m);

// The player's refusal copy, pinned verbatim by test_walkthrough.cpp.
extern const char *const kWtBeyondWindow;

} // namespace asmdesk
#endif // ASMDESK_WALKTHROUGH_H
