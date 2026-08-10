// view_presence.h — the data-driven view set (20-workspace-and-settings.md T1,
// F4). Before this, every recording showed all view tabs at once regardless of
// backing data, so a bare-log recording presented Loom / ABI-x-ray / 3D /
// Scrubber tabs that drew only a "producer absent" placard. This lifts
// observer_draw.cpp's own empties-gating up a level: the view set becomes a PURE
// function of the recording's data × the active mode, and the views a recording
// cannot fill are collapsed into ONE faithful "unavailable views (N)" affordance
// that still NAMES each absent view and its verbatim machine reason (D7 —
// restructured, never removed).
//
// Engine-free and ImGui-free by construction: it decides `present` from the SAME
// model fields the tab body reads, so a headless test asserts the model, not
// pixels.
#ifndef ASMDESK_UI_VIEW_PRESENCE_H
#define ASMDESK_UI_VIEW_PRESENCE_H

#include <optional>
#include <string>
#include <vector>

#include "analysis/stepindex.h"
#include "doc/recording.h"
#include "doc/streams.h"
#include "nav.h" // dt_view — for the want_view keymap routing of the present ones
#include "ui/mode.h"
#include "views/observer_draw.h"

namespace asmdesk {

// The stable identity of a hostable view. NOT dt_view: several of these (Summary,
// Loom, Scrubber, ABI x-ray, 3D) have no dt_view spelling, and the deep-link
// router only knows the replay/observer views. The brief sketched
// `struct ViewPresence { dt_view view; ... }`; the code wins — a private id
// enum covers the whole hostable set, and the entries that DO route by keypress
// carry an optional dt_view (canvas/timeline/slice/diff) for the 1/2/3/4 keymap.
enum class ViewId {
    Summary,
    Canvas,
    Timeline,
    Slice,
    Diff,
    Observer,
    Loom,
    Scrubber,
    AbiXray,
    Scene3D,
    // The session strip (2026-08-10 session-strip spec). Appended LAST, the
    // dt_view convention: enum order is identity, not tab order — the add()
    // call in view_presence.cpp decides where the tab reads.
    SessionStrip,
};

struct ViewPresence {
    ViewId id;
    const char *label;    // the tab label
    bool present = false; // can this recording × mode fill the view?
    std::string reason;   // verbatim machine reason when absent; "" when present
    // The deep-link view a 1/2/3/4 keypress selects, for the four that have one.
    // A keymap request for an ABSENT view lands on the "unavailable views"
    // affordance with this view pre-explained, never a silent no-op (T1 step 3).
    std::optional<dt_view> view;
};

// The whole hostable view set for one recording, in reading order. `present` is
// decided from the same fields the body reads: Loom present iff the recording is
// exact and carries per-step values (loom_fabric_build's own refusal); Scrubber
// iff `si` holds a regstate ring; 3D iff the recording can fill ANY of the pane's
// substrates — a codeimage plane, a coverage block set, a call tree, or wide
// register writes (59 T1; divergence is excluded, see the .cpp for why);
// ABI x-ray iff a second recording is attachable; Observer iff the deck has any
// live events; Slice iff the dataflow stream is present; Diff iff a B is
// attachable. `mode` scopes the set — the Observer deck is live-capture-only and
// reports "not shown in Author mode" there rather than an empty pane.
// `is_live` marks the active tab as the still-growing live capture (25 T1), which
// only sharpens the Scrubber's absent-reason: a live exact-dataflow session that
// did not arm `--steps` recorded no register ring, phrased distinctly from a saved
// emulator recording's (26 T4). It changes no presence verdict.
std::vector<ViewPresence> view_presence(const Streams &a, const ObserverState &obs,
                                        const StepIndex &si, const Recording &r,
                                        Mode mode, bool b_attachable,
                                        bool is_live = false);

// Count of absent entries — the N in "unavailable views (N)".
size_t view_absent_count(const std::vector<ViewPresence> &vp);

} // namespace asmdesk
#endif // ASMDESK_UI_VIEW_PRESENCE_H
