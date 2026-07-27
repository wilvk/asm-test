// loom_draw.h — the thin ImGui half of the Loom (05-loom-day-one.md T2).
//
// Same split as views_draw.h: this takes an ALREADY-BUILT plan and only paints
// it. Nothing here decides anything — if a rule is interesting enough to test,
// it lives in loom_plan() / loom_select() / loom_annex_join(), which the loom
// tests link on their own with no ImGui at all.
#ifndef ASMDESK_LOOM_DRAW_H
#define ASMDESK_LOOM_DRAW_H

#include <cstdint>
#include <string>
#include <vector>

#include "loom/annex.h"
#include "loom/fabric_plan.h"
#include "loom/feed.h"
#include "loom/lineage.h"
#include "loom/take_view.h" // loom_take_node_t + the takes-gutter accumulator (22 T4)
#include "ui/primer.h"      // first-open primer state (24 T5)
#include "ui/selection.h"   // the shared brushing-and-linking selection (22 T1)
#include "ui/undo.h"        // app-level undo stack, for the takes gutter (22 T4)

namespace asmdesk {

// Paint the plan into the current window's draw list, offset by its cursor
// origin. `hover` receives the hover text of the prim under the mouse (empty
// when there is none), so the caller owns tooltip policy.
void draw_loom_plan(const std::vector<loom_prim_t> &prims,
                    std::string *hover = nullptr);

// The panel's per-tab state. The fabric is woven ONCE per recording (weaving is
// not a per-frame operation) and `source_id` is how the panel notices the tab
// changed under it.
struct LoomState {
    loom_feed_t feed;
    loom_fabric_t fabric;
    bool built = false;
    std::string err; // the weave refusal, rendered verbatim
    std::string source_id;

    loom_view_t cam;
    loom_selection_t sel;
    bool has_selection = false;
    int lane = -1;         // the inspected lane (its header was clicked)
    uint32_t playhead = 0; // the audit scrubs this
    bool audit = false;
    dt_primer_state primer; // the first-open primer (24 T5), per recording

    // The persistent takes gutter's accumulator (22 T4, F12). LoomState held no
    // take set before this — F12's "the gutter accumulates forks with no
    // remove/clear" was describing doc 05:393-408's design that was never wired.
    // A full build appends each loom_take_run result here; the render-only viewer
    // shows recorded takes but assembles none. Per-take remove + clear-forks are
    // reversible undo Commands (each keeps its `err`/`disclosure` verbatim, so a
    // clear never quietly drops a take's loud refusal — D7).
    std::vector<loom_take_node_t> takes;
};

// The Loom camera's selection dim, DERIVED from the ONE shared selection (22 T1).
// A Loom click lights the worldline (L.sel.steps); a brush from another pane
// (shared->step) dims to that step; nothing selected dims to nothing. Pure, so the
// selection test asserts it as model state (D4) with no ImGui context.
inline std::vector<uint32_t> loom_shared_dim(const LoomState &L,
                                             const Selection *shared) {
    if (L.has_selection)
        return L.sel.steps; // the Loom's own picked worldline
    if (shared != nullptr && shared->step)
        return {*shared->step}; // brushed from another pane — highlight in place
    return {};                  // nothing selected -> no dim
}

// Draw the Loom for recording `self` of `ws` (its decoded streams in `s`).
// Weaves on first sight and on a tab change; a refusal renders as a placard and
// NOTHING else, because there is no fabric to draw. `shared` is the ONE shared
// selection (22 T1): a Loom click writes it (cross-highlighting timeline/slice/3D)
// and the fabric dim reads it (loom_shared_dim). `undo` records the takes gutter's
// remove/clear as reversible Commands (22 T4). Both may be null (a standalone
// draw with no shell), leaving the Loom's own behaviour unchanged.
void draw_loom(LoomState &L, const Streams &s, const Workspace &ws, int self,
               Selection *shared = nullptr, UndoStack *undo = nullptr);

// The persistent takes gutter (22 T4): each accumulated take with a per-node
// [remove] and a gutter-level [clear forks], both reversible via `undo`. Public
// so the interaction lane drives its buttons directly (test_ui), exactly as it
// drives draw_obs_syscalls. Call inside an ImGui window.
void draw_loom_takes_gutter(LoomState &L, UndoStack *undo);

} // namespace asmdesk
#endif // ASMDESK_LOOM_DRAW_H
