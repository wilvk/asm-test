// overview.h — the always-visible overview/minimap model (21-spine-navigation.md
// T3, completing 14-quick-wins.md T5's overview-strip stub).
//
// A PURE, engine-free projection of a recording's OWN rows: the whole-trace
// density the timeline / Loom strips draw, plus the current viewport window and
// the fractional-click -> step mapping the click-to-jump routes through
// dt_nav_go. Nothing here draws (that is timeline_draw.cpp / fabric_imgui.cpp);
// nothing here fabricates structure (docs 04/08 layout ban): a sparse trace
// yields a sparse strip — one bucket per step the recording actually recorded,
// never a padded [0, nsteps) range invented to look complete. That honesty rule
// is the load-bearing constraint of the minimap and is pinned by test_overview.
#ifndef ASMDESK_VIEWS_OVERVIEW_H
#define ASMDESK_VIEWS_OVERVIEW_H

#include <cstdint>
#include <vector>

#include "loom/fabric.h"
#include "views/timeline.h"

namespace asmdesk {

// One drawn bucket: a REAL step the recording covered, and the count the strip
// draws for it (def-use activity for a timeline row, live-span count for a Loom
// step). `step` is the recording's own step index — never a synthetic position.
struct OverviewBucket {
    uint32_t step = 0;
    uint32_t weight = 0;
};

// The whole-trace density series + the current viewport, in step space. `nsteps`
// is the step EXTENT (last real step + 1) the fractional click maps against;
// `buckets` are the real steps only. `lo`/`hi` is the viewport the main view is
// windowed to (the strip draws it as the current-position rectangle).
struct OverviewStrip {
    std::vector<OverviewBucket> buckets; // real steps only, in order
    uint32_t nsteps = 0;                 // step extent, for the click mapping
    double lo = 0, hi = 0;               // current viewport [lo, hi], step space
    bool from_timeline = false;          // which builder produced it (provenance)
};

// The operand-timeline projection: one bucket per row the timeline actually
// holds (a dropped/unknown step is kept with weight 0 — a real step index whose
// activity is unknown, never invented away and never a fabricated in-between
// bucket). `lo`/`hi` is the viewport the caller windowed the table to.
OverviewStrip overview_from_timeline(const dt_timeline &t, double lo, double hi);

// The Loom-fabric projection: one bucket per recorded step (the fabric's steps
// ARE the recorded steps), weight = the number of worldlines live at that step
// (a live-span count, from the fabric's own spans — no new structure). `lo`/`hi`
// is the step window the ImZoomSlider set.
OverviewStrip overview_from_fabric(const loom_fabric_t &f, double lo, double hi);

// Map a fractional x position (0..1 across the whole trace) to a step index,
// clamped to the real step space. A minimap click and a typed go-to therefore
// land on the same step link (D4): round(frac * nsteps), clamped to the last
// real step.
uint32_t overview_click_step(const OverviewStrip &o, double frac);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_OVERVIEW_H
