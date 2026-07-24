// loom_draw.h — the thin ImGui half of the Loom (05-loom-day-one.md T2).
//
// Same split as views_draw.h: this takes an ALREADY-BUILT plan and only paints
// it. Nothing here decides anything — if a rule is interesting enough to test,
// it lives in loom_plan() / loom_select() / loom_annex_join(), which the loom
// tests link on their own with no ImGui at all.
#ifndef ASMDESK_LOOM_DRAW_H
#define ASMDESK_LOOM_DRAW_H

#include <string>
#include <vector>

#include "loom/annex.h"
#include "loom/fabric_plan.h"
#include "loom/feed.h"
#include "loom/lineage.h"

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
};

// Draw the Loom for recording `self` of `ws` (its decoded streams in `s`).
// Weaves on first sight and on a tab change; a refusal renders as a placard and
// NOTHING else, because there is no fabric to draw.
void draw_loom(LoomState &L, const Streams &s, const Workspace &ws, int self);

} // namespace asmdesk
#endif // ASMDESK_LOOM_DRAW_H
