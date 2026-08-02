// gl_scene_host.cpp — the GL implementation of gl_scene_host.h / scene_host.h.
// Owns an offscreen RGBA8+depth colour framebuffer and a scene3d::Scene; render()
// uploads a terrain slice + trajectories + convergence arcs (only when the model
// or playhead changed), draws the scene into the FBO, and returns the colour
// texture as an ImTextureID for the shell to blit with ImGui::Image. pick() reuses
// the scene's own colour-ID pass.
//
// GL is reached exactly as scene.cpp reaches it — GL_GLEXT_PROTOTYPES + libGL on
// Linux/Mesa, the OpenGL framework on Darwin — and this TU includes NO glfw3.h, so
// (on Darwin) <OpenGL/gl3.h> never meets the legacy <OpenGL/gl.h> GLFW drags in,
// the same rule scene.cpp keeps. It links no engine, so it ships in the render-only
// viewer too (D4); the headless tests never compile it (they have no GL context).
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <cstdint>
#include <string>

#include "scene3d/scene.h"
#include "ui/gl_scene_host.h"
#include "ui/scene_gate.h"

namespace asmdesk {

namespace {

class GlSceneHost : public SceneHost {
  public:
    void init() override {
        if (inited_)
            return;
        inited_ = true;
        // The context must be current here (main.cpp calls this after standing up
        // the GL backend). A shader that will not build on this driver leaves
        // ready_ false and error() set, and the pane shows the reason.
        ready_ = scene_.init_gl(&err_);
        // T5 (55-scene-render-quality): MSAA defaults OFF on Scene itself (so
        // golden/headless renders stay bit-exact); the interactive app is the
        // one caller that wants smoothed lines, so it is the one that opts
        // in. ensure_compose_targets degrades to non-multisampled on a driver
        // that cannot complete this — never a black pane.
        if (ready_)
            scene_.msaa_samples = 4;
    }

    void shutdown() override {
        free_fbo();
        scene_.shutdown();
        ready_ = false;
    }

    bool ready() const override { return ready_; }
    const char *error() const override { return err_.c_str(); }

    ImTextureID render(const SceneFrame &f) override {
        if (!ready_ || f.slice == nullptr || f.terr == nullptr)
            return (ImTextureID)0;
        int w = f.fbw < 1 ? 1 : f.fbw;
        int h = f.fbh < 1 ? 1 : f.fbh;
        ensure_fbo(w, h);
        if (fbo_ == 0)
            return (ImTextureID)0;

        // Re-upload the worldlines/arcs when the recording changes OR GROWS (a
        // live capture keeps one identity but its generation advances each batch),
        // and the terrain when the sliced playhead moves. Gating the worldlines on
        // identity alone froze a growing capture's trajectories after batch 1.
        if (scene_needs_traj_upload(f.key, f.gen, up_key_, up_gen_,
                                    have_upload_)) {
            scene_.nsteps = static_cast<uint32_t>(f.terr->nsteps);
            scene_.set_trajectories(*f.traj, f.terr->proj);
            scene_.set_convergences(f.conv ? *f.conv : space::ConvergenceSet{},
                                    f.terr->proj);
            // T1/T4 (44-faithful-city-phase-a): both are slice-invariant
            // (kind_by_cell never varies with t; a survey is a whole-
            // recording aggregate), so they upload on the SAME gate as the
            // worldlines — once per weave/growth batch, never on a scrub.
            scene_.set_zoning(f.terr->kind_by_cell, f.terr->w, f.terr->h);
            if (f.terr->has_stat)
                scene_.set_stat_terrain(f.terr->stat);
            else
                scene_.clear_stat_terrain();
            // T4 (56): the opcode classification is a whole-recording fact
            // like kind_by_cell — same upload gate.
            static const std::vector<space::CellOpcode> kNoOpcodeCells;
            scene_.set_opcode_terrain(
                f.opcode_cells ? *f.opcode_cells : kNoOpcodeCells, f.terr->w,
                f.terr->h);
            // T5 (56): the misprediction layer is a whole-recording survey
            // aggregate too — same upload gate.
            static const space::MispredLayer kNoMispred;
            scene_.set_mispred_layer(f.mispred ? *f.mispred : kNoMispred);
            up_key_ = f.key;
            up_gen_ = f.gen;
            up_t_ = f.slice_t + 1; // force the terrain upload below
        }
        if (f.slice_t != up_t_) {
            scene_.nsteps = static_cast<uint32_t>(f.terr->nsteps);
            scene_.set_terrain(*f.slice);
            // T3 (56): re-lift on the SAME gate as the terrain slice itself —
            // raw_heat is t-gated exactly like `slice`'s own per-cell heights.
            static const std::vector<space::ModuleCanopy> kNoCanopies;
            scene_.set_module_canopies(f.canopies ? *f.canopies : kNoCanopies,
                                       f.terr->proj);
            up_t_ = f.slice_t;
        }
        have_upload_ = true;

        // T3/T6 (44): the sky and the followed citizen are per-FRAME state
        // (the atmosphere is already damped by the shell every frame; the
        // followed step advances under its own Transport) — set every call,
        // never gated on the upload condition above.
        scene_.set_atmosphere(f.atmo);
        scene_.follow_step = f.follow_step;
        // T1 (49): the terrain-residency playhead — SceneFrame already
        // carries it (slice_t), so this needs no new field, just passing it
        // through every frame like follow_step above.
        scene_.slice_step = f.slice_t;
        // 50 T2: the located selection, the same draw-time-uniform pattern.
        scene_.highlight_cell =
            f.has_highlight ? static_cast<int32_t>(f.highlight_cell) : -1;

        GLint prev_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, w, h);
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        scene_.render(f.cam, w, h, f.layers);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
        return (ImTextureID)(intptr_t)tex_;
    }

    uint32_t pick(const scene3d::Camera &cam, int fbw, int fbh, int x,
                  int y) override {
        if (!ready_)
            return 0;
        // Scene::pick saves and restores the bound draw framebuffer itself, so the
        // ImGui renderer's target (the default framebuffer) survives the pick pass.
        return scene_.pick(cam, fbw, fbh, x, y);
    }

    float traj_scale() const override { return scene_.traj_scale(); }

    // T3 (47-scene-inspect-and-pickable-overlays): ground truth from the
    // scene's own last upload — see SceneHost::pick_bands()'s doc comment.
    scene3d::PickBands pick_bands() const override {
        return scene_.pick_bands();
    }

    ~GlSceneHost() override { free_fbo(); }

  private:
    void ensure_fbo(int w, int h) {
        if (fbo_ && fbo_w_ == w && fbo_h_ == h)
            return;
        free_fbo();
        fbo_w_ = w;
        fbo_h_ = h;
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glGenRenderbuffers(1, &rbo_);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex_, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rbo_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
            GL_FRAMEBUFFER_COMPLETE) {
            free_fbo(); // leave fbo_ == 0 so render() reports no frame
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void free_fbo() {
        if (fbo_)
            glDeleteFramebuffers(1, &fbo_);
        if (rbo_)
            glDeleteRenderbuffers(1, &rbo_);
        if (tex_)
            glDeleteTextures(1, &tex_);
        fbo_ = rbo_ = tex_ = 0;
        fbo_w_ = fbo_h_ = 0;
    }

    scene3d::Scene scene_;
    bool inited_ = false;
    bool ready_ = false;
    std::string err_;

    GLuint fbo_ = 0, tex_ = 0, rbo_ = 0;
    int fbo_w_ = 0, fbo_h_ = 0;

    // Upload cache: the recording key + growth generation + the terrain-slice
    // playhead last uploaded.
    uint64_t up_key_ = 0;
    uint64_t up_gen_ = 0;
    uint64_t up_t_ = 0;
    bool have_upload_ = false;
};

} // namespace

std::unique_ptr<SceneHost> make_gl_scene_host() {
    return std::unique_ptr<SceneHost>(new GlSceneHost());
}

} // namespace asmdesk
