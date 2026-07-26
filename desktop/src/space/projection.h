// projection.h — address space -> unit plane, via a Hilbert curve over a
// compacted address domain (docs/internal/gui/10-spacetime-3d-overview.md T1).
//
// Pure and engine-free: this TU includes no GL, no ImGui and no engine header —
// the standard library only — which is what lets it compile into BOTH desktop
// binaries and the null test harness (D4). A recording's regions are placed on
// the terrain plane with zero engine dependencies, the same closure argument
// analysis/slice.h makes for replay slicing.
#ifndef ASMDESK_SPACE_PROJECTION_H
#define ASMDESK_SPACE_PROJECTION_H

#include <vector>

#include "space/types.h"

namespace asmdesk::space {

// Build a Projection from a raw region set: sort by base, pack each into a
// contiguous slot of a compacted domain [0, sum(len)) (so memory neighbours stay
// plane neighbours), and size the Hilbert plane to hold it — order =
// ceil(log4(sum(len))) clamped to [6, 12], a 64x64 .. 4096x4096 plane. `regions`
// is taken by value and moved into the result after sorting.
//
// Precondition: regions do not overlap (a /proc/maps snapshot and codeimage
// bases satisfy this). If two do overlap, project() resolves an address to the
// region with the greatest base <= addr; no region is dropped or merged.
Projection build_projection(std::vector<Region> regions);

// Region-kind colour + legend name, carried through for the HUD legend (T1 step
// 4). Pure data: the renderer (T4) turns it into GL; this returns only floats
// and a static string, so the legend is decided in an engine-free, testable TU.
struct RegionStyle {
    float r, g, b;
    const char *name;
};
RegionStyle region_style(Region::Kind kind);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_PROJECTION_H
