// filter.h — the ONE type-to-narrow filter affordance + free column sort
// (24-one-visual-language.md T4 / F16).
//
// Before this, three unrelated filter idioms lived across the views (ImSearch on
// Learn, an engine-side text box on Tree, a combo on Backends) and no list/table
// offered client-side narrowing where it is free. This is the single idiom:
// type to narrow, with a "showing N of M" count, over any list/table. The
// matcher and the count are PURE (asserted headlessly, model not pixels); the
// draw helper renders the bar and works under the null backend too (a plain
// InputText, not an addon, so no ImSearch context is required).
//
// It does NOT replace Tree's engine-side filter (that bounds what the engine
// EMITS — a different, honest thing) nor Backends' combo (a selector). It sits on
// top of the CLIENT-side lists that had nothing (doc 16's framing).
#ifndef ASMDESK_UI_FILTER_H
#define ASMDESK_UI_FILTER_H

#include <string>
#include <vector>

#include "imgui.h"

namespace asmdesk {

// Case-insensitive substring match. An empty/whitespace query matches every
// item (the neutral "showing all" state).
bool dt_filter_match(const std::string &query, const std::string &item);

// How many of `items` match `query`. `total` (if non-null) receives M (the full
// count); the return value is N (the matching count). Empty query => N == M.
int dt_filter_count(const std::string &query,
                    const std::vector<std::string> &items,
                    int *total = nullptr);

// Free column sort (T4 step 2): a PURE reordering of VIEW indices [0, keys.size)
// by `keys`, stable, without touching the model. Sorting reorders what the user
// sees; the recorded order underneath is unchanged (D4/D7). ImGui gives the
// table sort UI for free once the flag is set; this is the deterministic key
// sort the draw applies to its row indices.
std::vector<int> dt_sorted_order(const std::vector<double> &keys,
                                 bool ascending);

// The filter bar's persisted edit buffer.
struct dt_filter_state {
    char buf[128] = {0};
};

// Draw the filter bar: a labelled InputText plus "showing N of M" computed over
// `items`. Returns the current query (trimmed of nothing — the matcher handles
// blanks). The caller iterates `items` and draws only those where
// dt_filter_match(query, item) is true. Works under the null backend.
std::string dt_filter_bar(dt_filter_state &st,
                          const std::vector<std::string> &items,
                          const char *label = "filter");

} // namespace asmdesk
#endif // ASMDESK_UI_FILTER_H
