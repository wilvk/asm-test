// slice_view.h — the def-use slice explorer (04-replay-views.md T5).
//
// The plan's crown jewel: click a step, see everything that produced the value
// there and everything it goes on to affect. Producer-agnostic — it reads
// RECORDED def-use edges, so the emulator L0 producer feeds it today and a PT
// replay feeds it when 07/08 land, with no change here.
//
// LAYERED BY STEP INDEX, NEVER FORCE-DIRECTED. The plan's killed list bans live
// force layouts, and for a reason that outlives the aesthetics: a layout that
// moves is a layout you cannot cite, screenshot, or diff. Here the x-column IS
// the step index and arc lanes are assigned greedily in ascending
// (from_step, to_step) order, so identical input gives an identical picture —
// no randomness, no iteration, no settling.
#ifndef ASMDESK_VIEWS_SLICE_VIEW_H
#define ASMDESK_VIEWS_SLICE_VIEW_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "analysis/slice.h"
#include "doc/streams.h"

namespace asmdesk {

enum class dt_cone { none, back, fwd, both, dimmed };

struct dt_slice_node {
    uint32_t step = 0;
    uint64_t off = 0;
    std::string label;
    dt_cone style = dt_cone::none;
    int column = 0; // the layered x position; equals the node's rank by step
};

struct dt_slice_edge {
    uint32_t from_step = 0, to_step = 0;
    int lane = 0;    // the arc lane, deterministic
    std::string loc; // asmspy_df_loc_str of the carried location
};

struct dt_slice_view {
    std::vector<dt_slice_node> nodes; // ascending by step (column order)
    std::vector<dt_slice_edge> edges;
    std::optional<uint32_t> selected_step;
    bool truncated = false;
    std::string banner;

    // The cones themselves, so `[` / `]` can walk a generation without
    // recomputing, and the timeline can borrow the same slice for its emphasis.
    dt_slice back, fwd;
};

dt_slice_view dt_slice_view_build(const Streams &s,
                                  std::optional<uint32_t> selected);

// One dependence generation along the lit cone from `from`: the nearest in-cone
// step by step distance, ties resolved toward the LOWER step. nullopt at the
// end of the cone.
std::optional<uint32_t> dt_slice_view_walk(const dt_slice_view &v,
                                           uint32_t from, bool forward);

std::string dt_slice_view_dump(const dt_slice_view &v);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_SLICE_VIEW_H
