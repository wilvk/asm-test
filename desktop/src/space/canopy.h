// canopy.h — the per-module residency skyline
// (56-fidelity-and-module-layers.md T3): "which library is hot as a whole" —
// a question the per-cell terrain structurally cannot answer, because a
// viewer cannot sum a height field by eye.
//
// Pure and engine-free (D4): only space/terrain.h — no GL, no ImGui.
#ifndef ASMDESK_SPACE_CANOPY_H
#define ASMDESK_SPACE_CANOPY_H

#include <cstdint>
#include <vector>

#include "space/terrain.h"

namespace asmdesk::space {

// One region's aggregate, at an inclusive time slice [0, t]. A region with a
// footprint (cells_mapped > 0) always produces an EXACT entry, even at zero
// heat — "mapped but cold" and "never mapped" are different facts, and a
// canopy with cells_mapped > 0, cells_hit == 0 is the wire-outline case, not
// a slab. A region additionally produces a SEPARATE, `statistical`-flagged
// entry when its footprint carries any survey (TF_STAT) residency — never
// merged into the exact entry (the T6 isolation invariant this whole family
// extends).
struct ModuleCanopy {
    size_t region = 0;     // index into Projection::regions
    double raw_heat = 0.0; // SUM of per-cell hit counts over the footprint,
                           // gated at t — summed RAW, before the log below
    double height = 0.0;   // log1p(raw_heat), scaled AFTER the sum (never the
                           // sum of already-log-scaled per-cell heights)
    uint32_t cells_mapped = 0; // the region's total footprint, geometry only
    uint32_t cells_hit = 0;    // of those, how many have hits at-or-before t
    bool torn = false;         // the recording is torn AND this canopy has hits
    bool statistical = false;  // this entry came from the SEPARATE stat layer

    // Rendering placement: the footprint's bounding box in plane-UV space
    // [0,1]^2 — not in the brief's own struct sketch, added because the
    // renderer needs SOME footprint extent and re-deriving it would cost a
    // second whole-plane sweep. Invalid (min > max) when cells_mapped == 0,
    // which cannot happen for an entry this builder actually emits.
    float min_u = 1.0f, min_v = 1.0f, max_u = 0.0f, max_v = 0.0f;
};

// Builds one canopy per region touched by the terrain's footprint, at the
// inclusive slice [0, t]. `t = UINT64_MAX` is the whole recording. A region
// with zero footprint (nothing ever unprojects to it — cannot happen for a
// region actually in `model.proj.regions`, but guarded rather than assumed)
// is omitted entirely, never a zero-mapped entry.
std::vector<ModuleCanopy> build_module_canopies(const TerrainModel &model,
                                                uint64_t t);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_CANOPY_H
