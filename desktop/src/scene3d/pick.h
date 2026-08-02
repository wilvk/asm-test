// pick.h — the colour-ID picking layer of the 3D spacetime overview
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T4 step 5). The GL half (the
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
#include "space/converge.h" // T2 (47): ConvergenceSet, resolve_pick_hint's third param
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// The id space written into the R32UI pick target. 0 is the cleared background
// ("nothing here"); terrain cells occupy [1, n*n]; trajectory PC vertices occupy
// [n*n+1, n*n+1+npts) (T3, 47-scene-inspect-and-pickable-overlays: the vertex
// band used to be open-ended — [n*n+1, ...) — but a bounded band is what makes
// the NEXT two bands possible without ambiguity). Both passes derive ids from
// these functions so the encode and the decode below cannot drift.
inline uint32_t pick_id_cell(uint32_t cell) { return cell + 1u; }
inline uint32_t pick_id_vertex(uint32_t n, uint64_t vindex) {
    return static_cast<uint32_t>(static_cast<uint64_t>(n) * n + 1u + vindex);
}
// T3: convergence arcs occupy [n*n+1+npts, n*n+1+npts+nconv) — `npts` is the
// SAME uploaded PC-vertex count pick_id_vertex's band uses, so the two bands
// can never overlap regardless of how many vertices a given weave uploads
// (the doc's own "the decoder needs actual uploaded counts, not an inferred
// boundary" instruction, made structural).
inline uint32_t pick_id_conv(uint32_t n, uint64_t npts, uint64_t i) {
    return static_cast<uint32_t>(static_cast<uint64_t>(n) * n + 1u + npts + i);
}
// T3: access spurs occupy [n*n+1+npts+nconv, n*n+1+npts+nconv+nspur) — past
// BOTH prior bands.
inline uint32_t pick_id_spur(uint32_t n, uint64_t npts, uint64_t nconv,
                             uint64_t i) {
    return static_cast<uint32_t>(static_cast<uint64_t>(n) * n + 1u + npts +
                                 nconv + i);
}

// T3: the actual uploaded counts a decode needs to place the vertex/conv/spur
// band boundaries — carried explicitly rather than inferred (an inferred
// boundary is exactly the drift risk the brief calls out). `npts ==
// UINT64_MAX` (the default) means "unbounded" — the PRE-T3 behaviour, where
// every id past the cell band decodes as a Vertex and no conv/spur band
// exists — so every caller that constructed a Pick before T3 (every existing
// test, every pre-T3 shell.cpp call) stays byte-identical without being
// touched. A real caller (T3's shell.cpp wiring, T3's own tests) passes the
// true counts from a Scene::pick_bands() upload.
struct PickBands {
    uint64_t npts = UINT64_MAX;
    uint64_t nconv = 0;
    uint64_t nspur = 0;
};

struct Pick {
    enum Kind { None, Cell, Vertex, Conv, Spur } kind = None;
    uint32_t cell = 0;   // Kind::Cell: linear cell index y*n + x
    uint64_t vertex = 0; // Kind::Vertex: index in the canonical PC-vertex order
    uint64_t conv = 0;   // Kind::Conv: index into ConvergenceSet::marks
    uint64_t spur = 0;   // Kind::Spur: index into pick_spur_order()'s result
};

// T2 (47-scene-inspect-and-pickable-overlays): everything a hover readout needs
// to answer "what is this, and where would a click send me?" before any
// navigation happens — computed by resolve_pick_hint (pick.cpp), which shares
// ONE classification helper with resolve_pick so `target` can never disagree
// with what a click actually does (the anti-drift bar T2 exists for).
struct PickHint {
    bool empty = true;    // nothing pickable under the cursor (background, or a
                          // decoded id past all known geometry)
    std::string what;     // "code cell" | "data cell" | "survey cell" |
                          // "PC vertex" | "padding" | "convergence" |
                          // "access spur" (the last two added by T3)
    std::string where;    // region label + address/offset, verbatim
    std::string quantity; // the cell's own number, with its unit named
    std::string fidelity; // "" when exact; else the graded reason
    std::string target;   // dt_view_name of where a click would go, or
                          // "" when a click would do nothing
};

// Decode a read-back id against a plane of side n (n = 2^order). `bands`
// defaults to "unbounded vertex band, no conv/spur band" (see PickBands), so
// every pre-T3 call site decodes exactly as it always did.
Pick decode_pick(uint32_t id, uint32_t n, const PickBands &bands = PickBands{});

// The canonical order the scene uploads pickable trajectory vertices in, and that
// resolve_pick() replays to turn a vertex index back into a (tid, step): every
// PC vertex (is_access == false) of every trajectory, in trajectory-then-point
// order. `statistical` carries the owning trajectory's TRAJ_STATISTICAL flag so
// the resolver can route a sampled residency vertex to the hot-edge view rather
// than an exact reader (the T6 "statistical is never exact" invariant), with no
// second trajectory scan.
// T3 (47) UPDATE: access-mark spurs are now INDEPENDENTLY pickable too (this
// comment used to say otherwise — "they are skipped here" is still true of
// THIS function, which enumerates only PC vertices; see pick_spur_order below
// for the spur band's own canonical order).
struct PickVertex {
    int32_t tid;
    uint64_t t;
    bool statistical = false;
    // T2 (47-scene-inspect-and-pickable-overlays): the vertex's own address —
    // the DERIVED absolute address for a placed rel/df offset, the raw wire
    // offset for one the anchor could not place (TrajPoint::addr's own
    // contract, space/types.h). Added so resolve_pick_hint can name a
    // vertex's region/offset without a second trajectory scan; resolve_pick
    // itself does not need it (it routes on tid/t alone).
    uint64_t addr = 0;
};
std::vector<PickVertex> pick_vertex_order(const space::TrajectorySet &traj);

// T3 (47): the canonical order the scene uploads pickable access-mark spurs
// in — one entry per is_access TrajPoint that scene.cpp's set_trajectories
// draws a spur for (a projected access point following an already-projected
// PC vertex on the SAME trajectory), in the SAME trajectory-then-point order
// set_trajectories iterates. `t` is the OWNING PC vertex's own t (a spur
// drills to that step, per pick.h's existing note — now it can be hit as well
// as implied). KEEP-IN-SYNC with scene.cpp's spur-building loop: this is the
// pure-math mirror of GL vertex data scene.cpp uploads, so the two cannot be
// unified into one implementation across the GL/engine-free boundary (pick.cpp's
// own banner); the end-to-end GL smoke (test_scene_fbo.cpp) is what proves the
// two agree on a real upload.
struct PickSpur {
    int32_t tid;
    uint64_t t;
};
std::vector<PickSpur> pick_spur_order(const space::TrajectorySet &traj,
                                      const space::Projection &proj);

// Resolve a decoded pick to a 04 deep-link, or nullopt for None / a padding cell /
// an id past the live geometry. Every pickable kind routes to the flat 2D view
// that actually reads it ("3D to find, 2D to read", T6), with the two fidelity
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
//   Vertex, statistical residency -> the HOT-EDGE view (08-T4), never timeline;
//   Conv (T3, 47)                 -> the TIMELINE at whichever of the arc's two
//                                    tids' t is nearer `follow_step` (a two-
//                                    destination pick collapsed to the CLOSER
//                                    side — never silently, see resolve_pick_hint
//                                    for the readout that says which side);
//   Spur (T3, 47)                 -> the TIMELINE at its owning PC vertex's step
//                                    (the same step it already implied before it
//                                    was independently pickable).
// `conv`/`follow_step` (T3) are unused by every kind but Conv; the defaults keep
// every pre-T3 call site (every existing test, every Cell/Vertex-only caller)
// byte-identical.
std::optional<dt_link> resolve_pick(const space::TerrainModel &terr,
                                    const space::TrajectorySet &traj,
                                    const std::string &rec, const Pick &p,
                                    const space::ConvergenceSet &conv = {},
                                    uint64_t follow_step = 0);

// T2 (47-scene-inspect-and-pickable-overlays): the readout resolve_pick throws
// away — a pure, golden-testable function answering "what is this, and where
// would a click send me?" for the SAME pick resolve_pick would resolve, so a
// hover preview can never disagree with what a click actually does. Shares one
// private classification helper with resolve_pick (pick.cpp) rather than
// re-deriving the branch logic — the anti-drift bar this task exists for.
// T3 adds `follow_step` (a trailing default, so T2-only callers are unaffected)
// — needed to grade a Conv pick's "which side was chosen" the same way
// resolve_pick's own Conv branch does.
PickHint resolve_pick_hint(const space::TerrainModel &terr,
                           const space::TrajectorySet &traj,
                           const space::ConvergenceSet &conv, const Pick &p,
                           uint64_t follow_step = 0);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_PICK_H
