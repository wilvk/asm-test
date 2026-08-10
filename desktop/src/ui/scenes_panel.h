// scenes_panel.h — the Scenes control panel's pure model. One launcher pane
// lists the two scene surfaces (the 2D session strip, the 3D overview) with
// their availability and open affordances; THIS builds its rows from the SAME
// view_presence verdicts the recording tab bar draws from, so the panel and
// the tabs can never disagree about what this recording can fill (D7 — the
// absent reason is the machine's, verbatim, never a re-derivation).
//
// Header-only and ImGui-free: the draw half in shell.cpp walks the rows; the
// rules are asserted in test_view_presence.cpp with no context.
#ifndef ASMDESK_UI_SCENES_PANEL_H
#define ASMDESK_UI_SCENES_PANEL_H

#include <string>
#include <vector>

#include "ui/view_presence.h"

namespace asmdesk {

struct ScenesPanelEntry {
    ViewId id = ViewId::SessionStrip;
    const char *label = "";  // the vp entry's own tab label, re-stated
    const char *blurb = "";  // one sentence: what the scene shows
    bool present = false;    // the vp verdict, verbatim
    std::string reason;      // the vp machine reason when absent ("" present)
    bool has_own_pane = false; // the strip also opens as its own dock pane
};

// The two scene rows, in reading order (2D first). An id missing from `vp`
// yields no row — the panel never invents an entry the tab bar does not have.
inline std::vector<ScenesPanelEntry>
scenes_panel_build(const std::vector<ViewPresence> &vp) {
    std::vector<ScenesPanelEntry> out;
    auto add = [&](ViewId id, const char *blurb, bool has_own_pane) {
        for (const ViewPresence &e : vp)
            if (e.id == id) {
                ScenesPanelEntry r;
                r.id = id;
                r.label = e.label;
                r.blurb = blurb;
                r.present = e.present;
                r.reason = e.reason;
                r.has_own_pane = has_own_pane;
                out.push_back(std::move(r));
                return;
            }
    };
    add(ViewId::SessionStrip,
        "2D — the whole session over stream order: thread lanes, kernel "
        "rail, address bands, run seams; follows the live tail.",
        /*has_own_pane=*/true);
    add(ViewId::Scene3D,
        "3D — the spatial scene family over the address plane: terrain, "
        "worldlines, data layers; its HUD docks on the right rail.",
        /*has_own_pane=*/false);
    return out;
}

} // namespace asmdesk
#endif // ASMDESK_UI_SCENES_PANEL_H
