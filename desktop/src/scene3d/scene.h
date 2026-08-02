// scene.h — the GL scene of the 3D spacetime overview
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T4). Renders a Terrain slice
// (the displaced, kind-coloured height field) and a TrajectorySet (opaque exact
// tubes vs stippled statistical residency, plus access-mark spurs) under an orbit
// Camera, and resolves colour-ID picks back to a 04 deep-link via pick.h.
//
// Engine-free (D4): this TU includes GL and the pure space/ models, but NO
// Unicorn/Keystone/Capstone and NO ImGui — so it compiles into asmtest-viewer
// (the render-only, permissive binary) exactly as into the full app. The HUD is a
// SEPARATE TU (hud.{h,cpp}); the picking id/router logic is another (pick.{h,cpp},
// GL-free). The scene owns the GL objects and the offscreen pick framebuffer.
//
// All methods that touch GL require the caller's GL context to be current.
#ifndef ASMDESK_SCENE3D_SCENE_H
#define ASMDESK_SCENE3D_SCENE_H

#include <cstdint>
#include <string>
#include <vector>

#include "scene3d/atmosphere.h" // T3 (44): the fidelity weather sky's pure config
#include "scene3d/camera.h"
#include "scene3d/pick.h" // T3 (47): PickBands, pick_bands()'s return type
#include "space/converge.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// The HUD's layer toggles (T4 step 4). Off means "not drawn", never "faked flat".
// T7 (44-faithful-city-phase-a): the four new city-reskin bools all default
// true (the "city" preset needs no separate named-preset mechanism — the
// struct's own default-member initializers ARE the out-of-the-box default,
// matching the original 5 bools) and each gates exactly one Phase-A system
// independently of the others.
struct SceneLayers {
    bool terrain = true;
    bool exact = true;        // opaque exact trajectories
    bool statistical = true;  // stippled/translucent statistical residency
    bool access_marks = true; // spurs from a PC vertex to its data cell
    bool convergence =
        true;              // bright cross-thread convergence arcs (a hint, T5)
    bool zoning = true;    // T1: kind-hued terrain (kind_by_cell / uKind)
    bool weather = true;   // T3: the fidelity weather sky quad
    bool ghost_fog = true; // T4: the separate stippled statistical terrain
    bool vehicle = true;   // T6: the followed-citizen head + comet tail
    // T3 (49-one-time-truth): iso-density contour bands on the terrain — a
    // re-encoding of vHeight (D7: adds no claim), so a HUD toggle can turn it
    // off without hiding data. Default on.
    bool contours = true;
};

class Scene {
  public:
    Scene() = default;
    ~Scene();
    Scene(const Scene &) = delete;
    Scene &operator=(const Scene &) = delete;

    // Compile the shaders and create the grid mesh, textures and pick FBO. The GL
    // context must be current. False + error() when a shader will not build on
    // this driver (the caller then shows the message rather than a blank scene).
    bool init_gl(std::string *err = nullptr);
    bool ready() const { return ready_; }
    const std::string &error() const { return err_; }

    // Vertical scale: world Y per trace step for the trajectory. 0 => auto from
    // nsteps at upload time. Set (with nsteps) BEFORE set_trajectories.
    float time_scale = 0.0f;
    uint32_t nsteps = 0;
    float y_scale = 0.35f; // terrain height exaggeration (uYScale)

    // T1 (49-one-time-truth): the terrain-residency playhead (SceneFrame::slice_t),
    // set per frame like follow_step already is — a draw-time uniform, no upload.
    // Named for the axis it walks, `slice_step`, never `head`/`step`/`t`, so the
    // two clocks (this one and follow_step, T5/T6's OWN axis below) stay
    // distinguishable at every call site. render() clips the trajectory/spur draw
    // to this playhead: `[0, slice_step]` renders full-bright, past it dims (never
    // discards — the data is real, just not yet reached in this view). Defaults
    // to UINT64_MAX ("no cut yet configured"), never 0 — a caller that never sets
    // this field (every render() before this brief, and every existing GL test)
    // must see today's whole-recording render, not a retroactively clipped one.
    uint64_t slice_step = UINT64_MAX;

    // Upload a terrain slice: heights are normalised to [0,1] internally (so the
    // frag shader's clamp(vHeight,0,1) reads a hot cell as brighter than a cold
    // one regardless of the raw log-density magnitude). Sizes the grid to t.w.
    void set_terrain(const space::Terrain &t);
    // T1 (44-faithful-city-phase-a): upload the per-cell region-kind byte map
    // (space::TerrainModel::kind_by_cell) as an R8UI texture — clones
    // set_terrain's texture-creation shape exactly (gen-if-absent, GL_NEAREST
    // + GL_CLAMP_TO_EDGE, one glTexImage2D per weave). Slice-invariant: call
    // ONCE per weave, never on a playhead scrub.
    void set_zoning(const std::vector<uint8_t> &kind_by_cell, uint32_t w,
                    uint32_t h);
    // T4 (44-faithful-city-phase-a): upload the SEPARATE statistical terrain
    // (space::TerrainModel::stat) as its own height/flags texture pair — the
    // T6 isolation invariant made geometry, never merged with the exact
    // terrain. Call ONLY when TerrainModel::has_stat is true (the caller's
    // job — an absent survey calls clear_stat_terrain() instead), and ONCE
    // per weave like set_zoning (a survey is a whole-recording aggregate, not
    // keyed on the playhead).
    void set_stat_terrain(const space::Terrain &stat);
    // The survey is absent for this weave: stop drawing the ghost-fog surface
    // (the previously-uploaded textures are left allocated, just unused).
    void clear_stat_terrain() { has_stat_terrain_ = false; }
    // Upload the trajectories, projecting each PC vertex through `proj`.
    void set_trajectories(const space::TrajectorySet &ts,
                          const space::Projection &proj);
    // Upload the live-overlay convergence marks (T5): each becomes a bright arc
    // bowing above the plane between the two converging vertices (a HINT — the
    // renderer draws it in its own distinct colour, never as a trajectory tube).
    // Managed independently of set_trajectories, so re-uploading the paths does
    // not wipe the arcs; pass an empty set to clear them.
    void set_convergences(const space::ConvergenceSet &cs,
                          const space::Projection &proj);

    // T3 (44-faithful-city-phase-a): store the (already-damped, plain-float)
    // sky config for the next render() call. Time-stateless — the shell damps
    // the tier transition every frame and passes the already-lerped result;
    // Scene does no damping of its own.
    void set_atmosphere(const Atmosphere &a) { atmo_ = a; }

    // T5/T6 (44-faithful-city-phase-a): which TrajPoint.t is the followed
    // "citizen" head — SceneFrame.follow_step / SceneView.follow_step's OWN
    // axis (T5's documented axis choice), NOT the terrain-time playhead. Set
    // every frame before render(); a step with no PLACED vertex among the
    // exact (non-statistical) trajectories draws no vehicle — never a
    // mis-snapped one. Public, like time_scale/nsteps/y_scale above: it needs
    // no re-upload (T6 step 1's "no VBO schema change" option), just a
    // draw-time CPU lookup over the retained per-vertex facts (see Line
    // below), so a plain field matches this class's existing convention for
    // per-frame-set, no-upload state.
    uint64_t follow_step = 0;

    // Draw the lit scene into the currently-bound framebuffer (viewport fbw*fbh).
    void render(const Camera &cam, int fbw, int fbh, const SceneLayers &layers);

    // Pick pass: render ids into the internal R32UI FBO (sized fbw*fbh) and read
    // the id under window pixel (x, y) — y from the TOP. 0 == background.
    uint32_t pick(const Camera &cam, int fbw, int fbh, int x, int y);

    // The whole id buffer for this camera (row-major, bottom-left origin as GL
    // reads it), sized fbw*fbh. The headless FBO smoke uses it to locate a cell
    // on screen without knowing the projection maths a click would need.
    void render_pick_buffer(const Camera &cam, int fbw, int fbh,
                            std::vector<uint32_t> &out);

    // T3 (47-scene-inspect-and-pickable-overlays): the actual band sizes the
    // MOST RECENT set_trajectories()/set_convergences() upload used — the
    // ground truth a pick decode needs (PickBands, pick.h). Reading this back
    // from what was really uploaded (rather than re-deriving the counts a
    // second time from the pure models) is what keeps a click/hover decode
    // from ever aliasing two different bands onto the same id.
    PickBands pick_bands() const {
        return {static_cast<uint64_t>(pts_count_),
                static_cast<uint64_t>(nconv_), static_cast<uint64_t>(nspur_)};
    }

    void shutdown();
    uint32_t grid_n() const { return n_; }
    // T4 (49): the world-Y-per-step scale the last set_trajectories() upload
    // used (0 before any upload) — the HUD ruler needs it to place its ticks
    // at the SAME world Y the trajectory geometry itself sits at.
    float traj_scale() const { return traj_scale_; }

  private:
    bool ready_ = false;
    std::string err_;
    uint32_t n_ = 0;

    unsigned prog_terrain_ = 0;
    unsigned prog_traj_ = 0;
    unsigned prog_pick_terrain_ = 0;
    unsigned prog_pick_pt_ = 0;
    unsigned prog_stat_ = 0; // T4: the ghost-fog terrain program
    unsigned prog_sky_ = 0;  // T3: the weather sky quad program

    unsigned vao_grid_ = 0, vbo_cell_ = 0, ibo_grid_ = 0;
    int grid_index_count_ = 0;
    unsigned tex_height_ = 0, tex_flags_ = 0;
    unsigned tex_kind_ = 0; // T1: R8UI region-kind-per-cell texture

    // T4: the SEPARATE statistical terrain's height/flags texture pair (same
    // upload shape as tex_height_/tex_flags_, a different pair of GL objects
    // so the two surfaces can never alias). has_stat_terrain_ gates the draw;
    // clear_stat_terrain() flips it off without freeing the textures.
    unsigned tex_height_stat_ = 0, tex_flags_stat_ = 0;
    bool has_stat_terrain_ = false;

    // T3: the full-screen NDC sky quad (2 triangles, drawn as a strip).
    unsigned vao_sky_ = 0, vbo_sky_ = 0;
    Atmosphere atmo_;

    // T6: the followed-citizen head glyph — a single point, its world
    // position recomputed by find_head() each render() call (a CPU lookup
    // over Line::pt_* below) and re-uploaded via glBufferData (one vertex, a
    // negligible per-frame cost; this is NOT the trajectory VBO schema the
    // doc asks to leave alone).
    unsigned vao_head_ = 0, vbo_head_ = 0;
    // T2 (49): the execution-front glyph — same shape as vao_head_/vbo_head_
    // (a second one-vertex buffer is the same negligible cost), but a
    // DIFFERENT mark on a DIFFERENT clock: the front sits at the last placed
    // vertex at-or-before slice_step (terrain-residency time), the vehicle at
    // the exact vertex named by follow_step (its own per-tid axis). Distinct
    // GL objects so the two glyphs' lifecycles never alias.
    unsigned vao_front_ = 0, vbo_front_ = 0;

    struct Line {
        unsigned vao = 0, vbo = 0;
        int count = 0;
        bool statistical = false;
        float color[4] = {1, 1, 1, 1};
        // T6: CPU-retained per-PC-vertex facts, parallel arrays over every
        // `!is_access` TrajPoint this trajectory offered to the plane (upload
        // order == pick_vertex_order's order for this trajectory), so the
        // followed-citizen head can be located by `follow_step` alone at
        // render time — no VBO schema change, no re-upload on every tick.
        std::vector<uint64_t> pt_t;       // TrajPoint::t (this tid's own axis)
        std::vector<uint8_t> pt_placed;   // TrajPoint::placed && projected
        std::vector<float> pt_pos;        // 3 floats/entry; valid iff pt_placed
        std::vector<uint32_t> pt_pick_id; // the SAME id the pick pass writes
    };
    std::vector<Line> traj_lines_;
    Line access_spurs_; // all spurs in one GL_LINES buffer
    Line
        conv_arcs_; // all convergence arcs, tessellated into one GL_LINES buffer
    float traj_scale_ =
        0.02f; // the world-Y-per-step scale set_trajectories used

    unsigned vao_pts_ = 0, vbo_pts_pos_ = 0, vbo_pts_id_ = 0;
    int pts_count_ = 0;

    // T3 (47-scene-inspect-and-pickable-overlays): conv_arcs_/access_spurs_'
    // OWN pick-pass geometry — a SEPARATE vao from their colour-draw one
    // (access_spurs_.vao/conv_arcs_.vao are bound to prog_traj_'s "pos"-only
    // layout; the pick pass needs prog_pick_pt_'s "pos"+"vid" layout, mirroring
    // vao_pts_/vbo_pts_id_ above). Reuses the SAME position buffer
    // (access_spurs_.vbo / conv_arcs_.vbo) rather than re-uploading positions
    // twice — only the id array is new. nconv_/nspur_ are the counts
    // pick_bands() reports: nconv_ = the number of ConvergenceMark units (one
    // id per WHOLE arc, not per tessellated segment); nspur_ = the number of
    // spur line segments (one id per spur, i.e. per vertex PAIR).
    unsigned vao_conv_pick_ = 0, vbo_conv_pick_id_ = 0;
    unsigned vao_spur_pick_ = 0, vbo_spur_pick_id_ = 0;
    int nconv_ = 0, nspur_ = 0;

    unsigned fbo_pick_ = 0, tex_pick_ = 0, rbo_pick_depth_ = 0;
    int pick_w_ = 0, pick_h_ = 0;

    void build_grid(uint32_t n);
    void build_sky_quad();
    void ensure_pick_fbo(int w, int h);
    void free_traj();
    void free_conv();
    // `zoning` gates T1's kind tint (uZoning uniform) — false renders the
    // plain amber ramp (kindHue[Code]) regardless of uKind, so toggling
    // SceneLayers::zoning off changes only the tint, never the pick pass
    // (which does not declare uZoning at all).
    void draw_terrain_common(unsigned prog, const float mvp[16],
                             bool zoning = true, float contour_levels = 0.0f);
    void draw_stat_terrain(const float mvp[16]);
    void draw_sky();
    // T6 (44)/T2 (49): locate a vertex on an exact (non-statistical)
    // trajectory by either axis — the first trajectory (lowest tid, matching
    // by_tid's ascending iteration in trajectory.cpp) that yields a match.
    // `exact` true: the EXACT match `find_head` always used (follow_step, the
    // vehicle's own per-tid axis) — a placed vertex with pt_t == target, false
    // when nothing matches or the match is unplaced. `exact` false: the LAST
    // placed vertex with pt_t <= target (slice_step, the terrain-residency
    // playhead) — the execution front. `out_line` is the matching Line's
    // index in traj_lines_ (for the comet-tail uniform / glyph colour).
    bool find_vertex(bool exact, uint64_t target, float out_pos[3],
                     int *out_line) const;
    bool find_head(float out_pos[3], int *out_line) const {
        return find_vertex(/*exact=*/true, follow_step, out_pos, out_line);
    }
    // T2 (49): the execution front — the last placed vertex at-or-before
    // slice_step, i.e. exactly the boundary T1's dim/no-dim cut draws at.
    bool find_front(float out_pos[3], int *out_line) const {
        return find_vertex(/*exact=*/false, slice_step, out_pos, out_line);
    }
    // Render the id pass into the internal pick FBO (leaves it bound); returns the
    // previously-bound draw framebuffer so the caller can restore it after reading.
    int render_pick_into_fbo(const Camera &cam, int fbw, int fbh);
};

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_SCENE_H
