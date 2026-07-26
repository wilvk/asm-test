// scrubber.h — the register time-travel scrubber (09-teaching-producers.md T3).
//
// A playhead over a recording's steps showing the FULL register file at that
// step, seeked O(1) through the shared `regstate` index (analysis/stepindex.h),
// with per-step change highlighting (each register diffed vs the previous held
// step). The builder is pure — no ImGui, no I/O, no engine (D4) — so its every
// decision is asserted headlessly; scrubber_draw.cpp renders the built model.
//
// Honesty (D7), two cases the view must never paper over:
//   - The ring DROPPED a prefix of steps: [0, dropped) is a TORN region, and
//     seeking into it renders UNKNOWN (never a register file of zeros).
//   - The recording has NO `regstate` events: the producer is ABSENT, and the
//     view says so and deep-links the docs. The "re-run with a larger
//     max_insns" fallback is documented as NOT day-one (the plan's honest-limits
//     stance) and is deliberately not offered here.
#ifndef ASMDESK_VIEWS_SCRUBBER_H
#define ASMDESK_VIEWS_SCRUBBER_H

#include <cstdint>
#include <string>
#include <vector>

#include "analysis/stepindex.h"

namespace asmdesk {

// One register row of the deck at the playhead.
struct dt_scrubber_reg {
    std::string name;
    uint64_t value = 0;
    bool changed = false; // differs from the previous held step (the highlight)
};

// The scrubber's view-model at one playhead position.
struct dt_scrubber {
    // present == false: the producer is absent. `absent_message` is the placard
    // and `docs` the deep-link; the whole deck is empty.
    bool present = false;
    std::string absent_message;
    std::string docs;

    std::string desc; // the state-descriptor id the deck renders from

    // The full step space is [0, total). [0, dropped) is torn; [dropped, total)
    // is held ([first_held, last_held]).
    uint64_t total = 0;
    uint64_t dropped = 0;
    uint64_t first_held = 0;
    uint64_t last_held = 0;
    size_t held = 0;

    uint64_t playhead = 0; // the step shown, clamped into [0, total)

    bool at_held = false;   // the playhead lands on a held step
    bool torn_here = false; // the playhead is inside the torn region
    bool has_prev = false;  // a previous held step exists to diff against
    std::vector<dt_scrubber_reg> regs; // the register deck at the playhead

    std::string banner; // the torn-edge placard (empty when the ring kept all)
};

// Build the model at `playhead` (clamped into the full step space). An absent
// producer yields a present==false model carrying the placard and docs link.
dt_scrubber dt_scrubber_build(const StepIndex &idx, uint64_t playhead);

// `[` / `]` (04's step-key bindings): the previous / next step, clamped to the
// full step space [0, total). Torn steps are walkable too — the tear is part of
// the timeline, and stepping across it is how a reader SEES it.
uint64_t dt_scrubber_prev(const StepIndex &idx, uint64_t playhead);
uint64_t dt_scrubber_next(const StepIndex &idx, uint64_t playhead);

std::string dt_scrubber_dump(const dt_scrubber &s);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_SCRUBBER_H
