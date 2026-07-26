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

        // Re-upload only when the recording or the sliced playhead changed — the
        // terrain slice depends on t; the trajectory + arcs depend only on the
        // recording, so a scrub re-uploads the height field alone.
        if (f.key != up_key_ || !have_upload_) {
            scene_.nsteps = static_cast<uint32_t>(f.terr->nsteps);
            scene_.set_trajectories(*f.traj, f.terr->proj);
            scene_.set_convergences(f.conv ? *f.conv : space::ConvergenceSet{},
                                    f.terr->proj);
            up_key_ = f.key;
            up_t_ = f.slice_t + 1; // force the terrain upload below
        }
        if (f.slice_t != up_t_) {
            scene_.nsteps = static_cast<uint32_t>(f.terr->nsteps);
            scene_.set_terrain(*f.slice);
            up_t_ = f.slice_t;
        }
        have_upload_ = true;

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
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
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

    // Upload cache: the recording key + the terrain-slice playhead last uploaded.
    uint64_t up_key_ = 0;
    uint64_t up_t_ = 0;
    bool have_upload_ = false;
};

} // namespace

std::unique_ptr<SceneHost> make_gl_scene_host() {
    return std::unique_ptr<SceneHost>(new GlSceneHost());
}

} // namespace asmdesk
