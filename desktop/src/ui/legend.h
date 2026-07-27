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

// The Loom's structural take axis (05/24 T2): hot = solid fill, dim = hollow
// outline, unaligned tail = dashes. Its palette is loom-local by design, but the
// SECOND CHANNEL (fill pattern + label) is the point — the fabric never rides on
// colour alone, so the take legend names each pattern.
void dt_loom_take_legend();

// ---------------------------------------------------------------------------
// The ONE encoding table (24-one-visual-language.md T2 / F15). Each row pairs a
// semantic colour with the NON-colour channel it is ALSO drawn with, so no
// distinction rides on colour alone. The shared legend renders from this table
// and the T2 test asserts over it — legend, encoding and test cannot drift.
struct dt_encoding {
    const char *key;     // the semantic name (stable id)
    ImU32 col;           // its colour, from ui/theme.h (never a literal here)
    const char *channel; // the second channel token: a glyph / pattern / word.
                         // MUST be non-empty for every `categorical` row (T2).
    bool categorical;    // true = one of several peer categories a reader must
                         // tell apart (so the second-channel rule binds); false
                         // = a lone signal (a banner amber) with no peer to
                         // confuse it, though it still carries a token.
    bool text;   // drawn as small BODY text (>=4.5:1 gate) vs a fill/large-text
                 // swatch (>=3:1 gate).
    bool large_text_only; // amber: allowed only as large text / headers (T2).
};

// The whole categorical + signal palette, one row each. `n` receives the count.
const dt_encoding *dt_encoding_table(int *n);

} // namespace asmdesk
#endif // ASMDESK_UI_LEGEND_H
