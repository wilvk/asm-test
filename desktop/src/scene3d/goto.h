// goto.h — the "go there" resolvers of 48-scene-navigation-and-goto.md: turn a
// pick, a bare address, or a region index into a plane coordinate the camera
// can be pointed at. A SEPARATE TU from pick.cpp (which owns the pick ID
// space and the drill-out-to-2D resolver) because these answer a different
// question — "where do I point the camera", never "which 2D view opens" — but
// the same discipline: pure math over the space/ models, no GL, no ImGui, no
// engine (D4), so every path here is exercised headlessly.
#ifndef ASMDESK_SCENE3D_GOTO_H
#define ASMDESK_SCENE3D_GOTO_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "scene3d/camera.h" // Camera, scene_on_screen's param (50 T4)
#include "scene3d/pick.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// 48 T2: recentre the camera on what a pick landed on. A Cell pick recentres
// on that cell's centre (the same (x+0.5)/n rounding resolve_pick already
// uses); a Vertex pick recentres on that VERTEX'S CELL — its placed address
// projected onto the plane, never the vertex's world-Y (the target rides the
// plane, per the brief). False for Pick::None, a padding cell (unproject
// finds no owning region), or an unplaced vertex (36 T5) — a double-click on
// background/padding must be a documented no-op, never a silent jump to
// (0,0). `traj` resolves a Vertex pick's (tid, t) back to its TrajPoint,
// which pick.h's Pick alone does not carry — a deliberate widening of the
// brief's own Tests-section signature (which omitted TrajectorySet), because
// T2 step 2's requirement ("project its addr") is unsatisfiable without it.
bool scene_recentre_target(const space::TerrainModel &terr,
                           const space::TrajectorySet &traj, const Pick &p,
                           float *u, float *v);

// 48 T3: resolve a bare address to its plane cell — a thin, testable wrapper
// over Projection::project. False when no region maps it; the caller must
// REFUSE, never clamp to the nearest cell (D7: a goto answers the address
// given, or states it cannot — it never silently answers a different one).
bool scene_goto_addr(const space::Projection &proj, uint64_t addr, float *u,
                     float *v);

// 48 T3: the cap on how many addresses scene_goto_region samples across a
// region's byte range. Exhaustive (every address visited) for any region at
// or under this size — every fixture and every realistic code/data span in
// the tree qualifies; a bounded, evenly-spaced downsample above it, so a
// pathologically large observed-data span (54 T1) cannot make one goto an
// unbounded-cost walk. Named so the tradeoff is visible, not buried in a
// magic number.
inline constexpr size_t kGotoRegionSamples = 4096;

// 48 T3: frame a region's REAL cell footprint, never a bounding box derived
// from base/len — a Hilbert layout's footprint for one contiguous byte range
// is a real (if compact) SET of cells, and a rectangle assumed from address
// range would necessarily include cells a neighbouring region owns (the city
// doc's own rule: a plinth is the region's real cell set, never a bbox).
// Walks up to kGotoRegionSamples addresses spanning the region through
// Projection::project and frames the (u,v) BOUNDING BOX of the cells that
// actually came back — still not the exact footprint shape, but never wider
// than the true cells the sampled addresses touched, and exact whenever the
// region is small enough to sample exhaustively. `radius` is a camera-
// ergonomics heuristic scaled to the footprint's bounding extent (NOT a
// fidelity claim — nothing about it is asserted as measured). False when
// `region_index` is out of range or the region is empty (len == 0).
bool scene_goto_region(const space::Projection &proj, size_t region_index,
                       float *u, float *v, float *radius);

// 48 T4: a stable landmark to return to — the CODE-DISTRICT CENTROID derived
// from the region footprint, never the plane centre. Averages
// scene_goto_region's centroid over every Region::Code region (equal weight
// per region, not per byte — this is a camera landmark, not a measurement).
// Because a Hilbert cell is a real address, this is stable across live growth
// by construction: new events add cells, but a founding region's placement
// never moves, which is what makes "reset view" mean something on a growing
// capture. False when the recording places no code region at all (a data-only
// or empty projection) — the caller keeps whatever camera it already has.
bool scene_home_target(const space::TerrainModel &terr, float *u, float *v);

// 50 T4 (two-way-brushing): does plane cell (u,v) project inside the camera's
// current viewport, at plane height 0 — a disclosure check, not a fidelity
// measurement (the terrain's real height at that cell varies per slice; this
// answers "roughly where", which is all "off-screen, follow to see it" needs).
// Pure math over Camera::mvp (no GL), so it is exercised headlessly. `edge`,
// when non-null and the point is off-screen, names the side that clipped —
// "left"/"right"/"top"/"bottom" (whichever axis clipped furthest), or
// "behind" for a point behind the camera entirely — for a caller to point a
// directional cue at.
bool scene_on_screen(const Camera &cam, float u, float v, float aspect,
                     std::string *edge = nullptr);

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_GOTO_H
