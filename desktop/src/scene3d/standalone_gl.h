// standalone_gl.h — the GL draw half of the four non-plane substrates
// (docs/internal/gui/59-standalone-scenes.md T1 step 2, T2–T5's geometry).
//
// The split mirrors scene.{h,cpp} exactly: standalone.{h,cpp} is the pure
// model (no GL, no ImGui, no engine — the half every test drives), and THIS TU
// owns the GL objects. It shares the plane's program link and R32UI pick
// target through scene3d/glcommon.h, and NOTHING else: it has no grid mesh, no
// height texture and no trajectory buffers, because a scene whose geometry is
// ribs between two ribbons has nothing to gain from any of them.
//
// One renderer serves all four kinds. They differ in how the pure model
// becomes vertices, not in how vertices become pixels, so a class per kind
// would be four copies of the same VAO plumbing.
//
// LINE WIDTH: every line here is drawn at width 1.0. glLineWidth above 1.0 is
// invalid on a core forward-compatible context (the profile this app's own
// Apple path requests) and may report exactly [1,1] — see Scene::
// aliased_line_width_range's note and 55 T6. Where a wider stroke would help
// legibility the geometry says so in a comment rather than calling glLineWidth.
#ifndef ASMDESK_SCENE3D_STANDALONE_GL_H
#define ASMDESK_SCENE3D_STANDALONE_GL_H

#include <cstdint>
#include <string>
#include <vector>

#include "scene3d/camera.h"
#include "scene3d/glcommon.h"
#include "scene3d/pick.h"
#include "scene3d/scene_kind.h"
#include "scene3d/standalone.h"
#include "space/sessionflow.h"

namespace asmdesk::scene3d {

// What a standalone renderer is pointed at for one frame. Exactly one pointer
// is read, chosen by `kind`; the rest are ignored. (The shell hands this
// straight through from SceneFrame — one struct rather than four upload calls,
// because a scene switch must replace the geometry wholesale, never merge it.)
struct StandaloneFrame {
    SceneKind kind = SceneKind::Plane;
    const DivergenceScene *divergence = nullptr;
    const InvocationScene *invocation = nullptr;
    const ModuleRibbonScene *ribbon = nullptr;
    const LanePrismScene *prism = nullptr;
    const space::SessionFlowScene *flow = nullptr;
};

class StandaloneRenderer {
  public:
    StandaloneRenderer() = default;
    ~StandaloneRenderer();
    StandaloneRenderer(const StandaloneRenderer &) = delete;
    StandaloneRenderer &operator=(const StandaloneRenderer &) = delete;

    // Build the two programs (colour + pick). Context must be current.
    bool init_gl(std::string *err = nullptr);
    bool ready() const { return ready_; }
    const std::string &error() const { return err_; }
    void shutdown();

    // Turn the pure model into vertices. Wholesale: the previous kind's
    // geometry is dropped, never merged (scenes do not compose).
    void upload(const StandaloneFrame &f);

    // Draw the uploaded geometry into the currently-bound framebuffer.
    void render(const Camera &cam, int fbw, int fbh);

    // Colour-ID pick, into this renderer's own R32UI target (the SHARED
    // GlPickTarget). 0 == background.
    uint32_t pick(const Camera &cam, int fbw, int fbh, int x, int y);
    // The whole id buffer, for the headless GL smoke (mirrors
    // Scene::render_pick_buffer).
    void render_pick_buffer(const Camera &cam, int fbw, int fbh,
                            std::vector<uint32_t> &out);

    // The bands the LAST upload actually used — the ground truth a decode
    // needs (PickBands::kind + ::nelem), read back from what was really
    // uploaded rather than re-derived. {} before any upload.
    PickBands pick_bands() const { return bands_; }

  private:
    struct Vtx {
        float x, y, z;
        float r, g, b, a;
    };
    struct PickVtx {
        float x, y, z;
        uint32_t id;
    };

    void free_geometry();
    void build_divergence(const DivergenceScene &s);
    void build_invocation(const InvocationScene &s);
    void build_ribbon(const ModuleRibbonScene &s);
    void build_prism(const LanePrismScene &s);
    void build_flow(const space::SessionFlowScene &s);
    // Push one filled triangle (the flow's ribbon surfaces + seam walls —
    // the ONE batch here that is a surface, not a line).
    void tri(const float p0[3], const float p1[3], const float p2[3],
             const float rgba[4]);
    void flush_geometry();
    // Push one line segment / one axis-aligned box outline.
    void line(float x0, float y0, float z0, float x1, float y1, float z1,
              const float rgba[4]);
    void box(float cx, float cy0, float cy1, float cz, float half,
             const float rgba[4]);
    // A pickable element's hit proxy: a GL_POINTS vertex carrying its id.
    void hit(float x, float y, float z, uint64_t element_index);

    bool ready_ = false;
    std::string err_;
    unsigned prog_col_ = 0, prog_pick_ = 0;
    unsigned vao_lines_ = 0, vbo_lines_ = 0;
    unsigned vao_hits_ = 0, vbo_hits_ = 0;
    unsigned vao_tris_ = 0, vbo_tris_ = 0;
    int line_count_ = 0, hit_count_ = 0, tri_count_ = 0;
    std::vector<Vtx> lines_;
    std::vector<PickVtx> hits_;
    std::vector<Vtx> tris_;
    SceneKind kind_ = SceneKind::Plane;
    PickBands bands_;
    GlPickTarget pick_target_;
};

} // namespace asmdesk::scene3d
#endif // ASMDESK_SCENE3D_STANDALONE_GL_H
