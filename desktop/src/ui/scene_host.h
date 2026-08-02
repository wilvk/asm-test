// scene_host.h — the GL render-to-texture bridge for the 3D spacetime overview
// pane (docs/internal/archive/gui/10-spacetime-3d-overview.md — the integration surfacing
// pass). draw_shell is backend-free: it links NO GL and is driven by ImGui's null
// backend in tests, which is what keeps ui/shell.o out of asmtest-viewer's engine
// closure AND drivable headlessly. So the shell reaches the GL scene ONLY through
// this abstract host: main.cpp injects a real one (ui/gl_scene_host.cpp, over a
// live scene3d::Scene), and the null test backend leaves ShellState::scene_host
// null — the pane then weaves its pure space/ models and draws the HUD, and shows
// a placard where the GL viewport would be. The interface names only the pure
// space/ models, the pure camera, and an ImTextureID; it pulls in no GL header, so
// including it costs a backend-free TU nothing.
#ifndef ASMDESK_UI_SCENE_HOST_H
#define ASMDESK_UI_SCENE_HOST_H

#include <cstdint>
#include <vector>

#include "imgui.h" // ImTextureID

#include "scene3d/atmosphere.h" // Atmosphere (a POD; T3, 44-faithful-city-phase-a)
#include "scene3d/camera.h"     // Camera (pure math)
#include "scene3d/pick.h"  // T3 (47): PickBands, SceneHost::pick_bands()'s type
#include "scene3d/scene.h" // SceneLayers (a POD; no GL pulled in by the header)
#include "scene3d/scene_kind.h" // T1 (59): SceneKind, SceneFrame::kind
#include "scene3d/standalone.h" // T1 (59): the four non-plane scene models
#include "space/canopy.h" // T3 (56): ModuleCanopy, SceneFrame::canopies
#include "space/converge.h"
#include "space/mispred.h" // T5 (56): MispredLayer, SceneFrame::mispred
#include "space/opcode_terrain.h" // T4 (56): CellOpcode, SceneFrame::opcode_cells
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk {

// Everything the GL host needs to render one frame of the 3D overview. The shell
// builds the pure, engine-free space/ models and hands POINTERS to them; the host
// owns only GL (an offscreen colour framebuffer + a scene3d::Scene). `key`
// identifies the uploaded model set (a hash of the recording id), `gen` is its
// growth generation (the recording's event_count), and `slice_t` the playhead the
// terrain was cut at, so the host re-uploads the terrain/trajectory only when the
// recording changes, GROWS, or the playhead moves — not every frame. `gen` is what
// keeps a LIVE, growing capture's worldlines from freezing: its identity never
// changes as it grows, but its generation advances each batch.
struct SceneFrame {
    // T1 (59-standalone-scenes): WHICH substrate this frame describes. The
    // default is Plane and every field below keeps its exact pre-59 meaning,
    // so nothing in 56/57/58 has to change and the Plane path is
    // byte-identical. A non-Plane kind reads its OWN model pointer (further
    // down) and ignores terr/traj/conv/slice entirely — the four plane models
    // are not overloaded to mean something else, which is how a scene's
    // assumptions stay out of another scene's.
    //
    // ONE KIND AT A TIME. The pane switches; scenes do not compose. SceneLayers
    // is the additive-layer mechanism of the PLANE and does not extend across
    // substrates with different axes.
    scene3d::SceneKind kind = scene3d::SceneKind::Plane;

    const space::TerrainModel *terr = nullptr;
    const space::TrajectorySet *traj = nullptr;
    const space::ConvergenceSet *conv = nullptr;
    const space::Terrain *slice =
        nullptr;          // the terrain slice [0, t] to display
    // T3 (56-fidelity-and-module-layers): the per-module canopies at THIS
    // slice's t — t-gated like `slice` itself (raw_heat sums hits at-or-
    // before t), so the host re-uploads it on the SAME `slice_t` gate.
    // nullptr is treated exactly like an empty vector (no canopies drawn).
    const std::vector<space::ModuleCanopy> *canopies = nullptr;
    // T4 (56-fidelity-and-module-layers): the per-cell opcode classification
    // — a whole-recording fact like kind_by_cell, so the host uploads it on
    // the SAME gate as set_zoning/set_stat_terrain, never on a scrub.
    const std::vector<space::CellOpcode> *opcode_cells = nullptr;
    // T5 (56-fidelity-and-module-layers): the misprediction survey layer —
    // a whole-recording survey aggregate like opcode_cells above, same gate.
    const space::MispredLayer *mispred = nullptr;
    uint64_t key = 0;     // recording identity
    uint64_t gen = 0;     // recording growth generation
    uint64_t slice_t = 0; // the t `slice` was cut at
    scene3d::Camera cam;
    scene3d::SceneLayers layers;
    int fbw = 0, fbh = 0; // desired offscreen size, in pixels

    // T3 (44-faithful-city-phase-a): the already-damped weather sky config,
    // recomputed by the shell every frame from fidelity_severity() and passed
    // straight through (Scene does no damping of its own).
    scene3d::Atmosphere atmo;
    // T5: the terrain-time sun position in [0, 1] (sv.hud.t / max(nsteps, 1)),
    // recomputed every frame from existing HudState fields — no new persisted
    // UI state duplicates it. Reserved for a future sun-angle use; T3's sky
    // itself keys on the fidelity tier, not this axis (see shell.cpp's own
    // doc comment on why the two clocks stay separate).
    float sun = 0.0f;
    // T5/T6: the SECOND, independent playhead (SceneView::follow_step) —
    // TrajPoint.t's OWN per-tid axis, NEVER fused with `slice_t` (the
    // terrain-time axis) or with Selection.step (the flat views' execution-
    // step axis). See ui/shell.cpp's draw_scene_overview doc comment for the
    // axis-mismatch decision this field embodies.
    uint64_t follow_step = 0;
    // 50 T2 (two-way-brushing): the flat views' Selection, projected through
    // its ADDRESS onto this plane (space/locate.h's scene_locate_off/_step) —
    // a spatial POINTER, not a third clock. `has_highlight` false means either
    // nothing is selected in this recording or it could not be placed; the
    // HUD states which (see ui/shell.cpp's own doc comment on the axis-
    // mismatch decision this mirrors, T5/T6 above).
    bool has_highlight = false;
    uint32_t highlight_cell = 0;

    // --- T1 (59-standalone-scenes): the non-plane substrates ---------------
    // One pointer per SceneKind past Plane, each read ONLY when `kind` names
    // it. Separate members rather than one overloaded pointer (or a reuse of
    // `terr`) so a host can never mistake one substrate's model for another's,
    // and so adding a kind is additive to this struct rather than a change to
    // an existing field's meaning. All null under the Plane kind, which is
    // what keeps the plane frame byte-identical.
    const scene3d::DivergenceScene *divergence = nullptr;   // T2
    const scene3d::InvocationScene *invocation = nullptr;   // T3
    const scene3d::ModuleRibbonScene *ribbon = nullptr;     // T4
    const scene3d::LanePrismScene *prism = nullptr;         // T5

    // The axes this frame's kind declares — a pure function of `kind`
    // (scene3d::scene_axes), exposed here so the HUD and the tests read the
    // same one. There is no way to set an axis label directly: a scene cannot
    // ship an unlabelled axis because the label is not the scene's to choose.
    scene3d::SceneAxes axes() const { return scene3d::scene_axes(kind); }
};

// The abstract GL host. main.cpp constructs one (make_gl_scene_host), calls init()
// once with a current GL context, and points ShellState::scene_host at it. Every
// method that touches GL requires the caller's context to be current — which the
// shell's draw runs under in main.cpp's frame loop.
class SceneHost {
  public:
    virtual ~SceneHost() = default;

    // Build the GL scene (shaders, FBOs). Context must be current. Idempotent.
    virtual void init() = 0;
    // Release GL objects. Call before the GL context is torn down.
    virtual void shutdown() = 0;

    virtual bool ready() const = 0;        // shaders built, FBO complete
    virtual const char *error() const = 0; // why not, when !ready()

    // Render `f` into the offscreen texture and return its ImGui id (0 on failure
    // / not ready). The host re-uploads the models keyed on (f.key, f.slice_t).
    virtual ImTextureID render(const SceneFrame &f) = 0;

    // Read the pick id under a texture-local pixel (origin TOP-left), reusing the
    // geometry uploaded by the most recent render(). 0 == background / none.
    virtual uint32_t pick(const scene3d::Camera &cam, int fbw, int fbh, int x,
                          int y) = 0;

    // T4 (49): the world-Y-per-step scale the last render()'s trajectory
    // upload used (0 before any upload) — the HUD ruler needs it to place
    // ticks at the same world Y the trajectory geometry sits at.
    virtual float traj_scale() const = 0;

    // T3 (47-scene-inspect-and-pickable-overlays): the actual vertex/conv/spur
    // band sizes the last render()'s upload used — the ground truth
    // decode_pick needs (scene3d::PickBands) so a click/hover decode reads
    // what was REALLY uploaded rather than re-deriving the counts a second
    // time (the exact "no second lookup path" discipline pick.h's id-band
    // comments already state). {} (the PickBands default: unbounded vertex
    // band, no conv/spur band) before any upload.
    virtual scene3d::PickBands pick_bands() const = 0;
};

} // namespace asmdesk
#endif // ASMDESK_UI_SCENE_HOST_H
