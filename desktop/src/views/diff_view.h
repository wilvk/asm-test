// diff_view.h — the two-recording summary panel (04-replay-views.md T7).
//
// Plan D3's document-model law says every view takes one OR two recordings;
// this is the view whose whole subject is the pair. It renders the refusal
// reason when there is one, and otherwise: coverage counts, the top heat
// deltas, the hot-edge deltas under a STATISTICAL provenance chip, and the
// divergence card ("patient zero" — 05-loom-day-one.md's fork mechanic reads
// the same dt_divergence). Every row carries a deep link, so the panel is a
// launcher into the canvas and the timeline rather than a dead end.
#ifndef ASMDESK_VIEWS_DIFF_VIEW_H
#define ASMDESK_VIEWS_DIFF_VIEW_H

#include <string>
#include <vector>

#include "analysis/diff.h"
#include "doc/streams.h"
#include "nav.h"

namespace asmdesk {

enum class dt_diff_row_kind { header, coverage, heat, edge, divergence, note };

struct dt_diff_row {
    dt_diff_row_kind kind = dt_diff_row_kind::note;
    std::string text;
    // Statistical rows are chipped as such and NEVER shown as exact heat.
    bool statistical = false;
    // Empty when the row is not navigable; otherwise a formatted deep link.
    std::string link;
};

struct dt_diff_view {
    std::string a_id, b_id;
    std::string refusal; // non-empty => `rows` holds only the reason
    std::vector<dt_diff_row> rows;
    dt_diff diff;
};

// `top_heat` caps the heat-delta rows; the cap is ANNOUNCED in a note row when
// it bites, never applied silently.
dt_diff_view dt_diff_view_build(const Streams &a, const Streams &b,
                                size_t top_heat = 16);

std::string dt_diff_view_dump(const dt_diff_view &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_DIFF_VIEW_H
