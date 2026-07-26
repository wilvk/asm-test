// scene.h — the GL scene of the 3D spacetime overview
// (docs/internal/gui/10-spacetime-3d-overview.md T4). Renders a Terrain slice
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

#include "scene3d/camera.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::scene3d {

// The HUD's layer toggles (T4 step 4). Off means "not drawn", never "faked flat".
struct SceneLayers {
    bool terrain = true;
    bool exact = true;        // opaque exact trajectories
    bool statistical = true;  // stippled/translucent statistical residency
    bool access_marks = true; // spurs from a PC vertex to its data cell
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

    // Upload a terrain slice: heights are normalised to [0,1] internally (so the
    // frag shader's clamp(vHeight,0,1) reads a hot cell as brighter than a cold
    // one regardless of the raw log-density magnitude). Sizes the grid to t.w.
    void set_terrain(const space::Terrain &t);
    // Upload the trajectories, projecting each PC vertex through `proj`.
    void set_trajectories(const space::TrajectorySet &ts,
                          const space::Projection &proj);

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

    void shutdown();
    uint32_t grid_n() const { return n_; }

  private:
    bool ready_ = false;
    std::string err_;
    uint32_t n_ = 0;

    unsigned prog_terrain_ = 0;
    unsigned prog_traj_ = 0;
    unsigned prog_pick_terrain_ = 0;
    unsigned prog_pick_pt_ = 0;

    unsigned vao_grid_ = 0, vbo_cell_ = 0, ibo_grid_ = 0;
    int grid_index_count_ = 0;
    unsigned tex_height_ = 0, tex_flags_ = 0;

    struct Line {
        unsigned vao = 0, vbo = 0;
        int count = 0;
        bool statistical = false;
        float color[4] = {1, 1, 1, 1};
    };
    std::vector<Line> traj_lines_;
    Line access_spurs_; // all spurs in one GL_LINES buffer

    unsigned vao_pts_ = 0, vbo_pts_pos_ = 0, vbo_pts_id_ = 0;
    int pts_count_ = 0;

    unsigned fbo_pick_ = 0, tex_pick_ = 0, rbo_pick_depth_ = 0;
    int pick_w_ = 0, pick_h_ = 0;

    void build_grid(uint32_t n);
    void ensure_pick_fbo(int w, int h);
    void free_traj();
    void draw_terrain_common(unsigned prog, const float mvp[16]);
    // Render the id pass into the internal pick FBO (leaves it bound); returns the
    // previously-bound draw framebuffer so the caller can restore it after reading.
    int render_pick_into_fbo(const Camera &cam, int fbw, int fbh);
};

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_SCENE_H
