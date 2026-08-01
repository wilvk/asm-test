// transport.h — pure play/pause + playhead-projection seams
// (docs/internal/archive/gui/34-playhead-and-scene-reach.md T1/T3).
//
// Two tiny pure functions, header-only so no TU/makefile change and so the null
// backend drives them directly (D4):
//
//   transport_tick   — advance a playhead over ITS OWN step axis while playing,
//                      at a steady rate, stopping cleanly at the end.
//   playhead_project — the shared execution-step brush projected onto a local
//                      playhead, gated on the recording so a brush in ANOTHER
//                      recording never moves this pane (22 T1, the shell.cpp
//                      selection-scoping gate).
//
// There is deliberately NO single global clock here: the register/flat views
// share the execution-step axis, while the 3D overview walks a different
// terrain-time axis, and each transport advances its own (see the brief's
// fidelity note). Both axes reuse the SAME tick function — that is the only thing
// they share.
#ifndef ASMDESK_UI_TRANSPORT_H
#define ASMDESK_UI_TRANSPORT_H

#include <cstdint>
#include <optional>
#include <string>

#include "ui/selection.h"

namespace asmdesk {

// A play/pause transport over one step axis. `accum` carries the fractional step
// remainder between frames so the rate stays steady under a variable frame time;
// `steps_per_sec` is the playback speed. Pure state — no ImGui, no time source.
struct Transport {
    bool playing = false;
    float accum = 0.0f;
    float steps_per_sec = 8.0f;
};

// Advance `cur` toward `max` by whole steps for `dt` seconds at t.steps_per_sec,
// carrying the fractional remainder in t.accum. Returns `cur` unchanged when
// paused, when there is nothing to play (max == 0), or when dt <= 0 (the null
// backend's zero DeltaTime, so a headless frame never advances). On reaching
// `max` it STOPS (clears playing, drops the remainder) and returns `max`, so
// playback lands exactly on the last step and the button flips back to "Play"
// rather than looping or overshooting.
inline uint64_t transport_tick(Transport &t, uint64_t cur, uint64_t max,
                               float dt) {
    if (!t.playing || max == 0 || dt <= 0.0f)
        return cur;
    t.accum += dt * t.steps_per_sec;
    if (t.accum < 1.0f)
        return cur;
    const uint64_t whole = static_cast<uint64_t>(t.accum);
    t.accum -= static_cast<float>(whole);
    uint64_t next = cur + whole;
    if (next >= max) {
        next = max;
        t.playing = false;
        t.accum = 0.0f;
    }
    return next;
}

// The shared execution-step brush projected onto a local playhead: the clamped
// `selection.step` when the selection belongs to `rec_id` and carries a step,
// else nullopt (the caller keeps its own playhead). This is the ONE gate that
// keeps a step brushed in another recording from moving this pane after a tab
// switch (22 T1 — the same `selection.rec == a->id` test shell_timeline_model
// applies). The clamp keeps a projected step inside the local axis; the pane's
// own fidelity handling (e.g. the Scrubber's torn-edge at_step) covers a step the
// axis cannot show.
inline std::optional<uint64_t> playhead_project(const Selection &sel,
                                                const std::string &rec_id,
                                                uint64_t max) {
    if (sel.rec != rec_id || !sel.step.has_value())
        return std::nullopt;
    const uint64_t s = *sel.step;
    return s > max ? max : s;
}

} // namespace asmdesk
#endif // ASMDESK_UI_TRANSPORT_H
