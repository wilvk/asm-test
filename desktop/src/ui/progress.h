// progress.h — determinate-vs-indeterminate progress feedback, decided honestly
// (docs/internal/gui/14-quick-wins.md T3).
//
// A determinate bar (a real fraction) is shown ONLY when an HONEST total exists:
// an .asmtrace `end` footer, or a bounded live budget. A torn recording (no
// footer) or an unbounded live session has no honest total, so a fabricated
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
// has_total — an honest total EXISTS (end footer / bounded budget). Never an
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

} // namespace asmdesk
#endif // ASMDESK_UI_PROGRESS_H
