// mispred.h — the misprediction survey layer's plane-space geometry
// (56-fidelity-and-module-layers.md T5). Pure data (a POD header, no .cpp):
// built by views/hotedges.cpp's build_mispred_layer (which needs
// HotEdgeSceneView, a views/ type, so it cannot live in this space/-only
// island) and consumed by scene3d/scene.h — this split is what lets scene3d
// keep depending on space/ ONLY, never views/, exactly like every other
// scene3d model (D4).
#ifndef ASMDESK_SPACE_MISPRED_H
#define ASMDESK_SPACE_MISPRED_H

#include <cstdint>
#include <vector>

namespace asmdesk::space {

// One bias arc, projected onto the plane. Only an edge whose BOTH endpoints
// project makes an arc — an unplaceable endpoint is counted
// (MispredLayer::off_plane), never drawn as a false line.
struct MispredArc {
    float ua = 0, va = 0, ub = 0, vb = 0; // projected (u, v) endpoints
    uint64_t count = 0, mispred = 0;
    bool is_return = false;
    // A rendering-only observation, never a recorded fact: `to_addr <
    // from_addr` AND both addresses resolve to the SAME region — a
    // latch/loop-back shape. Labelled "derived" wherever shown (T5's own
    // step 6), never implied as a stated fact.
    bool derived_latch = false;
};

// One site column, AGGREGATED over every edge sharing `from_addr`'s cell
// (several call sites a few bytes apart would otherwise draw overlapping
// columns) — count/mispred sum, is_return is true if ANY contributing edge
// is a return.
struct MispredSite {
    float u = 0, v = 0;
    uint64_t count = 0, mispred = 0;
    bool is_return = false;
};

struct MispredLayer {
    std::vector<MispredArc> arcs;
    std::vector<MispredSite> sites;
    // Endpoints (from OR to, either half of any edge) that could not be
    // projected — a HUD chip states this count rather than the edge
    // silently vanishing (T5 step 3's own bar).
    uint32_t off_plane = 0;
};

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_MISPRED_H
