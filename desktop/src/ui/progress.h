// progress.h — determinate-vs-indeterminate progress feedback, decided faithfully
// (docs/internal/archive/gui/14-quick-wins.md T3).
//
// A determinate bar (a real fraction) is shown ONLY when a REAL total exists:
// an .asmtrace `end` footer, or a bounded live budget. A torn recording (no
// footer) or an unbounded live session has no real total, so a fabricated
// percentage would be a lie — it gets the indeterminate bar instead. The
// decision is pure + header-only so it is unit-tested without an ImGui context
// (D4) and the draw half only picks a bar to render.
#ifndef ASMDESK_UI_PROGRESS_H
#define ASMDESK_UI_PROGRESS_H

#include <cstdint>

namespace asmdesk {

enum class ProgressMode { Hidden, Determinate, Indeterminate };

// active   — is anything in flight worth a bar at all (a growing recording, a
//            load underway). When false: Hidden.
// has_total — a real total EXISTS (end footer / bounded budget). Never an
//            inferred or guessed total; that is the whole point.
inline ProgressMode progress_mode(bool active, bool has_total, uint64_t total) {
    if (!active)
        return ProgressMode::Hidden;
    if (has_total && total > 0)
        return ProgressMode::Determinate;
    return ProgressMode::Indeterminate;
}

// The clamped fraction for a Determinate bar. Only meaningful when
// progress_mode() returned Determinate (total > 0).
inline float progress_fraction(uint64_t done, uint64_t total) {
    if (total == 0)
        return 0.0f;
    double f = static_cast<double>(done) / static_cast<double>(total);
    return f < 0.0 ? 0.0f : (f > 1.0 ? 1.0f : static_cast<float>(f));
}

// --- the uniform busy signal for EVERY long op (23-graded-truth-layer.md T4) --
// Generalises this helper (14 T3) to any operation that can exceed a frame — a
// PT decode, a symbol / codeimage load, a terrain / trajectory rebuild — so an
// expert never has to guess whether the tool is working or hung. The DECISION
// half only (still pure, header-only, ImGui-free): the elapsed clock, the faithful
// determinate-vs-indeterminate mode, and a cancel flag. The draw half
// (spinner/bar/Cancel button) is draw_progress() in views_draw.h. The fidelity
// rule is UNCHANGED: no real total -> the indeterminate bar, never a fake %.
struct LongOp {
    bool active = false;    // is anything in flight worth a bar
    bool has_total = false; // a REAL total exists (never inferred)
    uint64_t total = 0, done = 0;
    // Time is INJECTED (the caller sets `now`/`started_at` from ImGui::GetTime),
    // so elapsed() is pure and testable with no clock — the same reason the rest
    // of this header takes its inputs rather than reading a global.
    double started_at = 0.0;
    double now = 0.0;
    bool cancel_requested = false;
    const char *label = "working";

    ProgressMode mode() const { return progress_mode(active, has_total, total); }
    float fraction() const { return progress_fraction(done, total); }
    // Seconds since the op began. Clamped at 0 so a stale/unset clock never
    // reports negative elapsed.
    double elapsed() const { return now > started_at ? now - started_at : 0.0; }
    // Begin the op at `t`, clearing any prior cancel.
    void begin(double t) {
        active = true;
        started_at = t;
        now = t;
        cancel_requested = false;
    }
    void request_cancel() { cancel_requested = true; }
};

// The 3D scrub's degrade-to-coarse decision (T4 step 3): a terrain re-slice whose
// touched-cell count would exceed the per-frame budget renders the labelled
// COARSE plane for the in-flight frame instead of stalling the UI thread, then
// swaps to the full slice when it lands. Pure so it is testable with no GL. A
// zero budget means "never degrade" (a caller that has not measured a budget).
inline bool should_degrade(uint64_t cell_count, uint64_t budget_cells) {
    return budget_cells != 0 && cell_count > budget_cells;
}

// Must an OVER-BUDGET slice stay coarse this frame? While PLAYING, always:
// transport_tick already keeps the playback RATE wall-clock steady, and this
// keeps the per-frame COST bounded too — a running animation never pays the
// full re-slice every frame, so the frame rate holds instead of the animation
// appearing to slow down. Not playing, the historical two-step is unchanged:
// coarse for the in-flight frame (`pending` false), the full slice lands one
// frame later (`pending` true). The full slice therefore lands on the first
// frame after pause/stop. Pure, beside should_degrade, for the same D4 reason.
inline bool scrub_hold_coarse(bool playing, bool pending) {
    return playing || !pending;
}

// The provenance note the degraded (coarse) scrub frame carries, so the flat
// plane is NEVER presented as measured emptiness (terrain.h:22 / D7) — it is the
// same labelled coarse rung the terrain shows normally, shown while the full
// detail loads.
inline const char *scrub_degrade_note() {
    return "coarse (scrubbing): a flat plane while the full slice loads — the "
           "labelled coarse rung, not measured emptiness";
}

} // namespace asmdesk
#endif // ASMDESK_UI_PROGRESS_H
