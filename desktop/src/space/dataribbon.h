// dataribbon.h — the data-access worldline ribbon
// (58-memory-data-cell-family.md T5): the SHAPE of the access order —
// streaming, strided, or pointer-chasing — which a flat address list cannot
// show and a locality-preserving plane can.
//
// This is the same per-vertex, per-tid line construction `trajectory.cpp`
// already performs for the PC path, with the effective address substituted for
// the PC. It is a SUBSTITUTION, not a second ribbon implementation: the
// vertices come from the same `mem` events, keyed on the same `step` axis, and
// are ordered by the same rule. What differs is only which address each vertex
// carries — and that difference is the whole layer.
//
// EXPLICITLY NOT the existing PC->data access-mark spurs
// (scene3d/scene.h's `access_spurs_`). Those draw an instruction's ASSOCIATION
// with the datum it touched; this draws the ORDER in which data was touched.
// The two must never read as one layer, so the legend says so in words.
//
// Pure and engine-free (D4): the standard library, the document model and the
// projection only — no GL, no ImGui, no engine header.
#ifndef ASMDESK_SPACE_DATARIBBON_H
#define ASMDESK_SPACE_DATARIBBON_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "space/types.h"

namespace asmdesk::space {

// Which population fed the ribbon. The two are NOT the same set of accesses
// and must never be presented as interchangeable: the emulator's hardware
// hooks see implicit stack traffic (pushes, call/ret) that a live `mem`
// enumeration does not, so a dataflow-sourced ribbon can be strictly denser
// than a `mem`-sourced one for the same program.
enum class RibbonSource { None, Mem, DataflowAbs };
const char *ribbon_source_label(RibbonSource s);

struct RibbonVertex {
    uint64_t step = 0; // the executing step (the ordering axis)
    uint64_t addr = 0; // the effective address
    uint32_t size = 0; // access width in bytes — the ribbon's width channel
    enum Dir { Unknown, Read, Write } dir = Unknown;
    // Plane placement. `placed` false means no region maps this address: the
    // vertex is KEPT (the access really happened) but contributes no geometry,
    // and it BREAKS the ribbon rather than being joined across.
    bool placed = false;
    float u = 0.0f, v = 0.0f;
    uint32_t cell = 0;
    // Index into Projection::regions, or SIZE_MAX when unplaced. A segment
    // whose two endpoints sit in DIFFERENT regions is a leap across distinct
    // observed-data spans — genuine non-locality, and labelled as such so a
    // reader never attributes it to the projection's compaction.
    size_t region = SIZE_MAX;
};

struct RibbonSegment {
    size_t a = 0, b = 0; // indices into DataRibbon::verts (a immediately
                         // precedes b in recorded step order)
    // The two endpoints are in different Projection regions: a leap across
    // distinct observed-data spans. GENUINE non-locality, not an artefact of
    // the compaction — the compaction preserves neighbourhood WITHIN a region
    // and says nothing across them, so a within-region jump is the only kind
    // this layer could mislead about, and it is not this one.
    bool cross_region = false;
};

struct DataRibbon {
    // Every recorded access, in step order — INCLUDING the unplaced ones, so
    // the count is the truth about the population even where the geometry
    // cannot show it.
    std::vector<RibbonVertex> verts;
    // Only between CONSECUTIVE recorded accesses that may honestly be joined.
    // A missing segment is a GAP: an uncovered step, or an unplaceable
    // endpoint. Interpolating across one would draw an access that was never
    // recorded, which is the one thing this layer must never do.
    std::vector<RibbonSegment> segs;

    RibbonSource source = RibbonSource::None;
    uint32_t off_plane = 0;   // vertices no region maps
    uint32_t gaps = 0;        // consecutive pairs deliberately NOT joined
    uint32_t cross_region = 0;// segments that leap between regions
    // The ribbon's tail is a CAP, not an end: the capture stopped. Nothing is
    // asserted after the last vertex.
    bool capped = false;
    // The recording declared dropped events. Then an access may have been lost
    // BETWEEN any two surviving ones, so every segment's "these were
    // consecutive" claim is weakened — stated on the layer rather than silently
    // ignored, because the drop is not localisable to a particular pair.
    bool drops_present = false;
    // The covered step domain: `mem` steps index into the trace/df step stream,
    // and a step at-or-past this is outside what the capture actually holds.
    // 0 means the recording states no step domain at all (then no step-domain
    // gap can be asserted, and none is).
    uint64_t covered_steps = 0;
};

// Build the ribbon for a recording over an already-built projection. Prefers
// the `mem` stream; falls back to `DataflowStream::recs`' abs-space ValRecs
// when `mem` is absent, and always records WHICH in `source` (they are not the
// same population — see RibbonSource).
//
// EXACT ONLY. Survey/statistical data is never woven into an ordered ribbon:
// an ordered ribbon asserts an access SEQUENCE, and a sampled population has
// no sequence to assert.
DataRibbon build_data_ribbon(const Recording &r, const Projection &proj);

// The layer's legend line — names the source population, says it is not the
// PC->data access spurs, and states the gap and leap rules.
std::string data_ribbon_note(const DataRibbon &ribbon);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_DATARIBBON_H
