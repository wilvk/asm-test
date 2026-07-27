// legend.h — the shared semantic-palette legend (24-one-visual-language.md T1).
//
// One draw helper for the whole app's colour LEGEND, so a view never hand-rolls
// a swatch row that could drift from what it actually draws. The palette lives
// in ui/theme.h; this renders it, and — crucially — renders the T2 SECOND
// CHANNEL token (shape / pattern / label) beside each swatch, so the legend is
// itself the proof that no distinction rides on colour alone. Draw-half only:
// no engine, no addon, imgui.h for the types (D4), so it links into the app, the
// render-only viewer, and the null test backend identically.
#ifndef ASMDESK_UI_LEGEND_H
#define ASMDESK_UI_LEGEND_H

#include "imgui.h"

namespace asmdesk {

// One legend entry: a colour swatch, a NON-colour channel token (a glyph /
// pattern / short word — "solid", "◄", "dashed" …), and the label with its ONE
// meaning. `channel` may be "" for the rare entry with no second channel, but
// every CATEGORICAL entry should carry one (T2's rule; the legend makes a bare
// entry visible).
void dt_legend_row(ImU32 col, const char *channel, const char *label);

// The whole semantic palette in one block, drawn from the ui/theme.h accessors
// (so legend and encoding cannot drift). A view calls this to explain its own
// colours; callers that only want a subset build rows with dt_legend_row.
void dt_semantic_legend();

// Just the dependency-cone sub-axis (back / fwd / both / off-cone) — the slice
// explorer's legend, split out because that is the one view where all four cone
// hues carry meaning at once.
void dt_cone_legend();

} // namespace asmdesk
#endif // ASMDESK_UI_LEGEND_H
