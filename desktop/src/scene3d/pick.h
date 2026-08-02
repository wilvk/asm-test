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
#include "scene3d/scene_kind.h" // T1 (59): SceneKind, the id space's OUTER band
#include "space/converge.h" // T2 (47): ConvergenceSet, resolve_pick_hint's third param
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk {
// T3 (50-two-way-brushing): forward-declared rather than pulling in the whole
// of doc/streams.h — resolve_pick only ever holds a pointer to one.
struct DataflowStream;
} // namespace asmdesk

namespace asmdesk::scene3d {

// --- the OUTER band: which SceneKind an id belongs to (T1, 59) --------------
// T3 (47) bounded the vertex band so the conv/spur bands could exist without
// ambiguity; every one of those bands is INSIDE one scene — the address plane.
// 59 adds substrates that are not the plane, so the id space needs one more,
// OUTERMOST split, and 59 T1's rule is that a band is ALLOCATED, never
// inferred: the top `kPickKindBits` of the 32-bit id name the SceneKind, and
// the remaining `kPickKindShift` bits are that kind's own local id space.
//
// SceneKind::Plane is 0, so `kind << shift` is 0 for it and EVERY plane id —
// pick_id_cell/_vertex/_conv/_spur, every golden, every existing test — is
// byte-identical to what it was before this brief. That is the whole reason
// the enum pins Plane at 0.
//
// The plane's local space is 2^28 = 268,435,456 ids. Its worst case is
// n*n + 1 + npts + nconv + nspur with n = 2^12 (Projection's clamped maximum
// order, projection.h) = 16,777,216 cells, leaving ~251M for the vertex/conv/
// spur bands of any weave a machine could hold. `pick_id_fits` states the
// bound rather than assuming it, and test_scene_kind pins it.
inline constexpr uint32_t kPickKindBits = 4;
inline constexpr uint32_t kPickKindShift = 32u - kPickKindBits;
inline constexpr uint32_t kPickKindLocalMask = (1u << kPickKindShift) - 1u;
inline constexpr uint32_t kPickKindCapacity = kPickKindLocalMask; // max local id

// Tag a kind-local id with its scene band. `local == 0` stays 0 for EVERY kind:
// 0 is the cleared background of the R32UI target ("nothing here"), a fact of
// the framebuffer clear, not of any one scene — so it can never be a kind's
// first element.
inline uint32_t pick_id_in_kind(SceneKind k, uint32_t local) {
    if (local == 0)
        return 0;
    return (scene_kind_index(k) << kPickKindShift) | (local & kPickKindLocalMask);
}
// Which scene band a read-back id lies in (meaningless for id 0 — check that
// first). A decode MUST check this before interpreting the local part: an id
// read back from a stale frame of another kind decodes to a different scene,
// and silently reading it as this one is the aliasing bug the brief names.
inline SceneKind pick_id_kind(uint32_t id) {
    return static_cast<SceneKind>((id >> kPickKindShift) &
                                  ((1u << kPickKindBits) - 1u));
}
inline uint32_t pick_id_local(uint32_t id) { return id & kPickKindLocalMask; }
// Does a kind-local id fit its band? False means the geometry is too large to
// pick unambiguously and the caller must NOT emit the id (a wrapped id would
// alias into the next kind's band — an inferred boundary by another name).
inline bool pick_id_fits(uint64_t local) { return local <= kPickKindCapacity; }

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
    // T1 (59-standalone-scenes): WHICH scene's bands these are. The three
    // counts above describe the address plane's inner bands and mean nothing
    // for another substrate, so a decode reads `kind` first. Defaults to
    // Plane, which is what every pre-59 caller (every existing test, the
    // pre-59 shell.cpp path, Scene::pick_bands()) constructs — so they stay
    // byte-identical without being touched, exactly as T3 (47) kept the
    // pre-T3 callers with `npts == UINT64_MAX`.
    SceneKind kind = SceneKind::Plane;
    // T1 (59): the standalone scene's own uploaded element count — the same
    // "the decoder needs actual uploaded counts, not an inferred boundary"
    // discipline as npts/nconv/nspur, for the one band a non-plane kind has.
    // Local ids run [1, nelem]; 0 is the background.
    uint64_t nelem = 0;
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
// T1 (59): an id whose OUTER band names a kind other than Plane decodes to
// Kind::None — a stale id read back from another substrate's frame is not a
// cell of this one. Plane ids are all < 2^28, so nothing that existed before
// this brief changes.
Pick decode_pick(uint32_t id, uint32_t n, const PickBands &bands = PickBands{});

// T1 (59-standalone-scenes): the standalone kinds' decode. A non-plane scene
// uploads ONE flat band of pickable elements, so its decode is a bounds check
// against the count that was REALLY uploaded (PickBands::nelem) plus the outer
// kind check — the same "actual uploaded counts, never an inferred boundary"
// rule the plane's inner bands follow. Returns the 0-based element index, or
// nullopt for the background, an id from a different scene kind, or an id past
// the live geometry. Inline (like the id encoders above) so the id space stays
// one header both the GL encode and the pure decode read.
inline std::optional<uint64_t> decode_pick_standalone(uint32_t id,
                                                      const PickBands &bands) {
    if (id == 0 || bands.kind == SceneKind::Plane)
        return std::nullopt;
    if (pick_id_kind(id) != bands.kind)
        return std::nullopt;
    const uint32_t local = pick_id_local(id);
    if (local == 0 || local > bands.nelem)
        return std::nullopt;
    return static_cast<uint64_t>(local - 1);
}
// The id a standalone scene writes for its `index`-th pickable element (0
// based). Mirrors decode_pick_standalone exactly — both live here so the GL
// encode and the pure decode cannot drift, the discipline pick_id_cell and
// decode_pick already share.
inline uint32_t pick_id_element(SceneKind k, uint64_t index) {
    if (!pick_id_fits(index + 1))
        return 0; // too large to pick unambiguously: emit nothing, never wrap
    return pick_id_in_kind(k, static_cast<uint32_t>(index + 1));
}

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
    // vertex's region/offset without a second trajectory scan; T3 (50-
    // two-way-brushing) also reads it — resolve_pick's reverse-direction fix
    // searches a DataflowStream for a step at this address rather than
    // trusting the per-tid ordinal `t` to double as a step index.
    uint64_t addr = 0;
    // T3 (50): TrajPoint::placed, mirrored — false means `addr` above is the
    // RAW WIRE OFFSET, not a real address (an anchor could not place it), so
    // resolve_pick must not search a DataflowStream for it (a raw offset
    // could accidentally alias a real low address) and instead falls back to
    // the offset-basis canvas route directly.
    bool placed = true;
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
//   Vertex, exact PC, address found in `df`  -> the Loom / operand TIMELINE at
//                                    the step whose OWN offset resolves to the
//                                    same address (50-two-way-brushing.md T3:
//                                    the address route, never the per-tid
//                                    ordinal `t`, which is a step index only
//                                    by luck — see the fidelity note below);
//                                    several steps sharing that address open
//                                    the FIRST (a loop body — see
//                                    resolve_pick_hint for "one of N" wording);
//   Vertex, exact PC, no `df` supplied, or the address is in NO step of it,
//   or the vertex itself is unplaced (raw wire offset, not a real address)
//                                  -> `traj.trajectories.size() == 1` (the
//                                    ONLY condition under which the per-tid
//                                    ordinal is provably 1:1 with a pass's
//                                    step index) falls back to `t` as the
//                                    step; otherwise the CANVAS at the
//                                    vertex's own offset (never a step index
//                                    the stream cannot honour);
//   Vertex, statistical residency -> the HOT-EDGE view (08-T4), never timeline;
//   Conv (T3, 47)                 -> the TIMELINE at whichever of the arc's two
//                                    tids' t is nearer `follow_step` (a two-
//                                    destination pick collapsed to the CLOSER
//                                    side — never silently, see resolve_pick_hint
//                                    for the readout that says which side);
//   Spur (T3, 47)                 -> the TIMELINE at its owning PC vertex's step
//                                    (the same step it already implied before it
//                                    was independently pickable).
// `conv`/`follow_step` (T3, 47) and `df` (T3, 50) are unused by every kind but
// Conv and Vertex respectively; the defaults keep every pre-existing call site
// (every existing test, every Cell/Conv/Spur-only caller) byte-identical. `df`
// must be the SAME pass the trajectory's df_step-derived vertices came from —
// resolve_pick never re-derives which pass "step" means.
std::optional<dt_link> resolve_pick(const space::TerrainModel &terr,
                                    const space::TrajectorySet &traj,
                                    const std::string &rec, const Pick &p,
                                    const space::ConvergenceSet &conv = {},
                                    uint64_t follow_step = 0,
                                    const DataflowStream *df = nullptr);

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
