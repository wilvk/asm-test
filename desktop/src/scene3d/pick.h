// pick.h — the colour-ID picking layer of the 3D spacetime overview
// (docs/internal/gui/10-spacetime-3d-overview.md T4 step 5). The GL half (the
// R32UI framebuffer, the pick-pass draw and the 1x1 glReadPixels) lives in
// scene.cpp, which owns the GL objects; THIS TU is the engine- AND GL-free half:
// the id encoding both passes agree on, and the resolution of a read-back id to a
// 04 deep-link (nav.h). Keeping resolution here — a pure function of the terrain
// model, the trajectory set and the recording id — is what lets the "a pick
// reaches 04's router" contract be asserted with no GL context at all, and is why
// the FBO smoke's router checks run even where the GL smoke self-skips.
#ifndef ASMDESK_SCENE3D_PICK_H
#define ASMDESK_SCENE3D_PICK_H

#include <cstdint>
#include <optional>
#include <string>

#include "nav.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// The id space written into the R32UI pick target. 0 is the cleared background
// ("nothing here"); terrain cells occupy [1, n*n]; trajectory PC vertices occupy
// [n*n+1, ...). Both passes derive ids from these two functions so the encode and
// the decode below cannot drift.
inline uint32_t pick_id_cell(uint32_t cell) { return cell + 1u; }
inline uint32_t pick_id_vertex(uint32_t n, uint64_t vindex) {
    return static_cast<uint32_t>(static_cast<uint64_t>(n) * n + 1u + vindex);
}

struct Pick {
    enum Kind { None, Cell, Vertex } kind = None;
    uint32_t cell = 0;   // Kind::Cell: linear cell index y*n + x
    uint64_t vertex = 0; // Kind::Vertex: index in the canonical PC-vertex order
};

// Decode a read-back id against a plane of side n (n = 2^order).
Pick decode_pick(uint32_t id, uint32_t n);

// The canonical order the scene uploads pickable trajectory vertices in, and that
// resolve_pick() replays to turn a vertex index back into a (tid, step): every
// PC vertex (is_access == false) of every trajectory, in trajectory-then-point
// order. Access-mark spurs are not independently pickable (they drill to the same
// step as their PC vertex), so they are skipped here. `statistical` carries the
// owning trajectory's TRAJ_STATISTICAL flag so the resolver can route a sampled
// residency vertex to the hot-edge view rather than an exact reader (the T6
// "statistical is never exact" invariant), with no second trajectory scan.
struct PickVertex {
    int32_t tid;
    uint64_t t;
    bool statistical = false;
};
std::vector<PickVertex> pick_vertex_order(const space::TrajectorySet &traj);

// Resolve a decoded pick to a 04 deep-link, or nullopt for None / a padding cell /
// an id past the live geometry. Every pickable kind routes to the flat 2D view
// that actually reads it ("3D to find, 2D to read", T6), with the two honesty
// invariants folded in — statistical never opens an exact reader, and a churned
// region opens the version-aware disasm pane rather than a plain canvas:
//   Cell, exact code, not churned -> the trace CANVAS at the code offset;
//   Cell, exact code, churned     -> the codeimage-versioned DISASM pane (08-T7)
//                                    at the code offset (its bytes differ by trace
//                                    time; only disasm resolves "which version");
//   Cell, data access (rich `mem`)-> the SLICE explorer at the step whose access
//                                    last hit that data cell;
//   Cell, statistical-only (TF_STAT, no exact content) -> the HOT-EDGE view
//                                    (08-T4), NEVER the exact slice explorer;
//   Vertex, exact PC              -> the Loom / operand TIMELINE at that step;
//   Vertex, statistical residency -> the HOT-EDGE view (08-T4), never timeline.
std::optional<dt_link> resolve_pick(const space::TerrainModel &terr,
                                    const space::TrajectorySet &traj,
                                    const std::string &rec, const Pick &p);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_PICK_H
