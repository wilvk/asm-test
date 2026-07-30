// end_state.h — fanning the collapsed `Ended` state into its CAUSE
// (docs/internal/gui/23-graded-truth-layer.md T2, F20).
//
// When streaming stops the user could not tell WHY: a clean detach, a torn drop,
// an EOF, a mid-capture crash, or the overwhelmingly common cause — a stale
// `build/asmspy` that prints its usage banner and exits 0. All folded into one
// "ended", and the differentiating diagnosis was offloaded to an external README
// essay. The data to distinguish them is already in `LiveStatus` + the malformed
// count + the torn-recording facts; nothing fanned `Ended` into them. This does.
//
// Pure + header-only (no subprocess): end_cause() decides from a fact struct the
// state machine already fills, so every cause is asserted with no live target
// (D4). end_facts_of() gathers those facts off a LiveSession for the draw code.
// This does not collapse `Ended`; it CLASSIFIES it, and makes the torn/mismatch
// causes LOUDER and more specific, never quieter (D7).
#ifndef ASMDESK_LIVE_END_STATE_H
#define ASMDESK_LIVE_END_STATE_H

#include <cstdint>
#include <string>

#include "live/session.h"

namespace asmdesk {

// The four terminations, each with a DISJOINT recovery and a different trust in
// whatever data is already on screen.
enum class EndCause {
    StoppedClean,     // a clean detach / quit / max / exit — complete for what it got
    TornHostGone,     // the host crashed mid-capture (non-clean exit) — tail untrusted
    TornEof,          // the stream ended (EOF) mid-capture — tail untrusted
    ProtocolMismatch, // a non-serve asmspy printed a usage banner and exited 0
    HostFailedNoData, // the host exited with an ERROR before any recording began —
                      // a bad remote (ssh refused/auth), a wrong flag, a missing
                      // target. Nothing was captured; NOT a clean stop.
};

// The facts end_cause() decides from — all derivable today. Kept as a struct so
// the classifier is pure and header-testable (the state machine can produce most
// of these directly; a non-clean host exit needs a subprocess, so it is tested by
// constructing the facts).
struct EndFacts {
    bool host_exited = false;  // the subprocess was reaped
    int host_status = 0;       // its wait status (0 = a clean exit(0))
    uint64_t malformed_lines = 0; // unparseable / usage-banner lines from the host
    bool any_recording = false;   // a header ever arrived (a real session began)
    bool torn_recording = false;  // the last recording is torn (no `end` footer)
};

// Classify a terminated session. Order matters: the usage-banner case (malformed
// output, no header ever) is checked FIRST because it is the common one and its
// symptom (no recording at all) is unambiguous; a torn recording then splits on
// whether the host died badly (crash) or the pipe simply closed (EOF).
inline EndCause end_cause(const EndFacts &f) {
    // PROTOCOL-MISMATCH: the host emitted unparseable lines (a usage banner) and
    // never opened a recording. Nothing was captured, and the fix is mechanical.
    if (f.malformed_lines > 0 && !f.any_recording)
        return EndCause::ProtocolMismatch;
    // HOST-FAILED: the host exited with an ERROR and no recording ever began — a
    // refused/failed ssh, a bad flag, a missing target. It printed nothing we
    // could parse (or that path is above), so the collapsed "ended" would read as
    // a clean stop; it is a failure and must be LOUD (a non-zero wait status is a
    // non-clean exit or a signal death — see reap()).
    if (!f.any_recording && f.host_exited && f.host_status != 0)
        return EndCause::HostFailedNoData;
    if (f.torn_recording) {
        // A non-clean host exit means it crashed mid-capture; a clean/absent exit
        // status with a torn tail means the stream just ended (EOF).
        if (f.host_exited && f.host_status != 0)
            return EndCause::TornHostGone;
        return EndCause::TornEof;
    }
    return EndCause::StoppedClean;
}

// Gather the facts off a live session (the draw code's entry point). `growing()`
// is normally null at `Ended`, but a still-open recording is counted as "any
// recording" and as torn — an open recording at end is torn by definition.
inline EndFacts end_facts_of(const LiveSession &s) {
    EndFacts f;
    const LiveStatus &st = s.status();
    f.host_exited = st.host_exited;
    f.host_status = st.host_status;
    f.malformed_lines = s.malformed_lines();
    f.any_recording = !s.recordings().empty() || s.growing() != nullptr;
    if (s.growing() != nullptr)
        f.torn_recording = true;
    else if (!s.recordings().empty())
        f.torn_recording = s.recordings().back().torn;
    return f;
}

// A short label for the placard heading.
inline const char *end_cause_title(EndCause c) {
    switch (c) {
    case EndCause::StoppedClean:
        return "Session ended cleanly";
    case EndCause::TornHostGone:
        return "Host crashed mid-capture";
    case EndCause::TornEof:
        return "Stream ended mid-capture";
    case EndCause::ProtocolMismatch:
        return "Not a serve-capable asmspy";
    case EndCause::HostFailedNoData:
        return "Capture host failed to start";
    }
    return "Session ended";
}

// The full placard sentence: the cause AND the trust of the data already on
// screen, so the user knows how much of what they are looking at to believe.
inline std::string end_cause_message(EndCause c) {
    switch (c) {
    case EndCause::StoppedClean:
        return "Session ended cleanly. The capture above is complete for what it "
               "recorded.";
    case EndCause::TornHostGone:
        return "The host crashed mid-capture — this recording is TORN "
               "(integrity): its tail is untrusted and must not be read as a "
               "complete capture.";
    case EndCause::TornEof:
        return "The stream ended (EOF) mid-capture — this recording is torn: its "
               "tail is untrusted.";
    case EndCause::ProtocolMismatch:
        return "The host is not a serve-capable `asmspy` — it printed a usage "
               "banner and exited. Nothing was captured.";
    case EndCause::HostFailedNoData:
        return "The capture host exited with an ERROR before any data arrived — "
               "e.g. a refused/failed ssh, a wrong flag, or a missing target. "
               "Nothing was captured; this is a failure, not a clean stop.";
    }
    return "Session ended.";
}

// The ONE-LINE FIX, rendered in-pane (T2 step 2 / step 4). Non-empty for the two
// causes with a mechanical remedy: it is a machine-checkable instruction, kept
// VERBATIM so it mirrors the README essay's remedy voice rather than paraphrasing.
inline std::string end_cause_fix(EndCause c) {
    if (c == EndCause::ProtocolMismatch)
        return "Fix: rebuild `build/asmspy` (`make cli`); Disconnect + reconnect.";
    if (c == EndCause::HostFailedNoData)
        return "Fix: check the capture command / connection (run it in a shell to "
               "see its error), then Disconnect + reconnect.";
    return std::string();
}

// The graded fidelity tier the placard renders through (T1's components): the
// torn causes and the protocol mismatch are INTEGRITY (loud, non-collapsible); a
// clean stop is NEUTRAL.
inline bool end_cause_is_integrity(EndCause c) {
    return c != EndCause::StoppedClean;
}

} // namespace asmdesk
#endif // ASMDESK_LIVE_END_STATE_H
