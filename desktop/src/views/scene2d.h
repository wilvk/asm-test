// scene2d.h — the GL-free 2D terrain surface's pure model
// (docs/internal/gui/52-flat-terrain-surface.md).
//
// Pure and engine-free (D4): only the space/ models the 3D pane already weaves
// before it ever reaches GL (ui/shell.cpp builds sv.terr/sv.traj/sv.conv once
// per recording) — no GL, no ImGui. That is what lets the flat surface draw
// under the null test backend, in the render-only viewer, over plain SSH, and
// on a driver whose shader will not build: every one of the cases the 3D pane
// today only shows a placard for.
#ifndef ASMDESK_VIEWS_SCENE2D_H
#define ASMDESK_VIEWS_SCENE2D_H

#include <cstdint>
#include <string>
#include <vector>

#include "space/converge.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk {

// T1: the cell -> colour model, the SINGLE source of truth for what a cell
// looks like — shared by the flat surface (T2) and, by comment (a GLSL string
// cannot include a header), kTerrainFrag (scene3d/shaders/embedded.h). It
// reproduces the shader's branch chain EXACTLY, in the SAME order: kind hue ->
// height mix -> churn/scaffold -> statistical dim -> fog-of-war pit ->
// torn/rubble last. Getting the order wrong would make the flat surface
// disagree with the 3D view about a cell's fidelity state — worse than no
// surface at all.
struct CellPaint {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
    bool hatched = false; // torn/unknown patterning; the draw half applies it
    const char *why = ""; // the fidelity state this paint encodes, verbatim
};
CellPaint cell_paint(float height, uint32_t flags, uint8_t kind);

// One down-sampled block of the flat surface (T2 step 5). Down-sampling ORs
// `flags` and takes the MAX `height` over the block's source cells — a torn or
// statistical cell can never be binned away — then calls cell_paint on that
// folded (height, flags) pair with the REPRESENTATIVE cell's kind (the block's
// top-left source cell; region boundaries rarely split a small block, and
// this is the same tie-break scene2d.cpp documents for `region` below).
// `total_cells`/`off_domain_cells` let the draw half mark the compacted-
// domain edge (T4) without a second scan: a block is wholly padding when the
// two are equal and nonzero, and straddles the edge when 0 < off_domain <
// total.
struct Scene2dBlock {
    uint32_t bx = 0, by = 0; // block-grid coordinates (row-major with grid_w)
    CellPaint paint;
    int32_t region = -1; // index into TerrainModel::proj.regions; -1 = none
    uint32_t total_cells = 0, off_domain_cells = 0;
};

// One trajectory path point in plane space [0,1)^2 (Projection::project's own
// range). `placed` mirrors TrajPoint::placed: false means this vertex could
// not be projected (an unreachable rel offset) and the draw half must break
// the strip here, never interpolate across it (T2 step 3) — `u`/`v` are then
// meaningless and must not be drawn. `past_playhead` mirrors 49's dim-never-
// discard rule for the SAME terrain-time playhead the slice was cut at (T2
// step 4): a point past it is real, recorded data, not yet reached in this
// view, so the draw half dims it rather than omitting it.
struct Scene2dPathPoint {
    float u = 0.0f, v = 0.0f;
    bool placed = true;
    bool past_playhead = false;
};
struct Scene2dPath {
    int32_t tid = -1;
    bool statistical = false; // solid vs. dashed — the SAME channel the 3D
                              // view uses, never a new encoding (T2 step 2)
    std::vector<Scene2dPathPoint> points;
};

// One access mark or convergence mark, drawn as a small glyph at its cell —
// the same established colours as hud.cpp's overlay_encoding_swatches()
// (scene2d_draw.cpp cites them verbatim, the same keep-in-sync convention
// terrain_encoding_swatches() already follows against kTerrainFrag).
struct Scene2dMark {
    float u = 0.0f, v = 0.0f;
    bool convergence = false; // false = access spur, true = convergence hint
};

// T4 step 1: one region's label anchor. Placed at the region's compacted-
// domain MIDPOINT cell, not traced as a boundary polygon — a region's true
// footprint on a Hilbert-mapped plane is not a rectangle, and tracing its
// exact outline is not what this brief's "S" effort buys (see scene2d.cpp).
// Block-level region-transition edges (Scene2dBlock::region) are what the
// draw half actually outlines.
struct Scene2dRegionLabel {
    float u = 0.0f, v = 0.0f;
    std::string text; // verbatim region label, or its kind's legend name
};

struct Scene2dPlan {
    bool has_plane = false; // false => terr.w == 0; nothing else below is valid
    uint32_t w = 0, h = 0;  // the SOURCE terrain grid (== proj plane side)
    uint32_t downsample = 1; // source cells per block edge; 1 = no down-sample
    uint32_t grid_w = 0, grid_h = 0;  // block grid = ceil(w/downsample) each
    std::vector<Scene2dBlock> blocks; // row-major, grid_w*grid_h entries
    std::vector<Scene2dPath> paths;
    std::vector<Scene2dMark> marks;
    std::vector<Scene2dRegionLabel> region_labels;
    // T4 step 3's scale strip is NOT built here: it is
    // scene3d::height_scale_note(terr.max_full_heat(playhead_t)), and that
    // function lives in scene3d/hud.cpp alongside ImGui-calling code — pulling
    // it in here would drag ImGui into this TU's engine-free link closure for
    // one string. The draw half already links hud.o (every consumer of this
    // plan does), so it computes the note directly at draw time instead.
};

// Build the flat-surface plan for one slice. `slice` is the ALREADY-CUT
// terrain (the caller's re-slice at the current playhead — this never
// re-slices, exactly like the 3D pane's own sv.slice); `playhead_t` is the
// SAME t the slice was cut at, so path points grade past/before it exactly as
// kTrajFrag's time-cut does (T2 step 4). `max_blocks_per_edge` caps the block
// grid so a busy order-12 plane (4096x4096) down-samples instead of emitting
// 16M+ draw rects (T2 step 5); the factor actually used is `w /
// plan.grid_w` (integer), which the draw half states on screen.
Scene2dPlan build_scene2d_plan(const space::TerrainModel &terr,
                               const space::Terrain &slice,
                               const space::TrajectorySet &traj,
                               const space::ConvergenceSet &conv,
                               uint64_t playhead_t,
                               uint32_t max_blocks_per_edge = 256);

// T3: invert the flat surface's block layout back to a SOURCE terrain cell
// index (the same `cell` classify_cell/resolve_pick expect: y*w + x in the
// FULL, non-down-sampled grid) — the block's own top-left source cell, the
// same representative cell build_scene2d_plan used for its paint/region. Takes
// a click position as a fraction of the drawn surface's width/height (each in
// [0,1)); this is the exact arithmetic the draw half's layout uses, run
// backwards, so a flat pick and a 3D pick of the same cell can never disagree
// (T3's own done-when). False when the plan has no plane or the fraction is
// outside [0,1).
bool scene2d_pick_cell(const Scene2dPlan &plan, float frac_x, float frac_y,
                       uint32_t *cell);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_SCENE2D_H
