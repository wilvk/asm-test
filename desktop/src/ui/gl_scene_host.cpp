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
#include "scene3d/standalone_gl.h" // T1 (59): the non-plane substrates
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
        standalone_.shutdown(); // T1 (59)
        standalone_inited_ = standalone_ready_ = false;
        ready_ = false;
    }

    bool ready() const override { return ready_; }
    const char *error() const override { return err_.c_str(); }

    ImTextureID render(const SceneFrame &f) override {
        if (!ready_)
            return (ImTextureID)0;
        int w = f.fbw < 1 ? 1 : f.fbw;
        int h = f.fbh < 1 ? 1 : f.fbh;
        // T1 (59-standalone-scenes): ONE SCENE AT A TIME. A non-plane kind is
        // rendered by its own renderer into the SAME offscreen texture, and
        // the plane's uploads are neither consulted nor disturbed — the two
        // substrates share the framebuffer, never the geometry.
        if (f.kind != scene3d::SceneKind::Plane)
            return render_standalone(f, w, h);
        if (f.slice == nullptr || f.terr == nullptr)
            return (ImTextureID)0;
        ensure_fbo(w, h);
        if (fbo_ == 0)
            return (ImTextureID)0;
        // T1 (59): record the plane as last-rendered, mirroring
        // render_standalone's kind_ = f.kind. Without this, kind_ stays pinned
        // to the last standalone scene after switching back here, and pick()/
        // pick_bands() route every click to the wrong substrate.
        kind_ = scene3d::SceneKind::Plane;

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
            // T4 (58): the lifetime pillars span first..last OBSERVED touch
            // over the whole recording, so they ride this gate too — a Gantt
            // that moved with the playhead would not be a Gantt.
            static const space::LifetimePillars kNoLifetime;
            scene_.set_lifetime_pillars(f.lifetime ? *f.lifetime : kNoLifetime);
            // T5 (58): the access-order ribbon is whole-recording too.
            static const space::DataRibbon kNoRibbon;
            scene_.set_data_ribbon(f.ribbon ? *f.ribbon : kNoRibbon);
            // T6 (58): the sediment columns are whole-recording too.
            static const space::SedimentColumns kNoSediment;
            scene_.set_sediment_columns(f.sediment ? *f.sediment : kNoSediment);
            // 57-causal-layers: whole-recording aggregates, same gate. The
            // crossing spurs hang on worldline vertices, so this must come
            // AFTER set_trajectories above (which fixes traj_scale) — it
            // does, and set_crossing_layer's own doc comment states the
            // requirement rather than leaving it to call-site order alone.
            static const space::CrossingLayer kNoCrossings;
            scene_.set_crossing_layer(f.crossings ? *f.crossings
                                                  : kNoCrossings);
            static const space::TaintFront kNoTaint;
            scene_.set_taint_front(f.taint ? *f.taint : kNoTaint);
            static const space::BlameForest kNoBlame;
            scene_.set_blame_forest(f.blame ? *f.blame : kNoBlame);
            static const space::PathRidge kNoRidge;
            static const space::RidgeSurvey kNoRidgeSurvey;
            scene_.set_path_ridge(
                f.ridge ? *f.ridge : kNoRidge,
                f.ridge_survey ? *f.ridge_survey : kNoRidgeSurvey);
            up_key_ = f.key;
            up_gen_ = f.gen;
            up_t_ = f.slice_t + 1; // force the terrain upload below
            // 51 T2: a new/grown weave means a new Projection, so the focus
            // mask below must be re-derived even if the focused region index
            // did not change. -2 is "no mask uploaded yet" (distinct from -1,
            // a live "no region focused").
            up_focus_region_ = -2;
        }
        if (f.slice_t != up_t_) {
            scene_.nsteps = static_cast<uint32_t>(f.terr->nsteps);
            scene_.set_terrain(*f.slice);
            // T3 (56): re-lift on the SAME gate as the terrain slice itself —
            // raw_heat is t-gated exactly like `slice`'s own per-cell heights.
            static const std::vector<space::ModuleCanopy> kNoCanopies;
            scene_.set_module_canopies(f.canopies ? *f.canopies : kNoCanopies,
                                       f.terr->proj);
            // T2 (58): the twin relief is cut at the SAME inclusive [0, t] as
            // the terrain slice, so it rides the same gate — never a stale
            // relief standing over a freshly-scrubbed terrain.
            static const space::DataReliefLayer kNoRelief;
            scene_.set_data_relief(f.relief ? *f.relief : kNoRelief);
            // T3 (58): same gate, plus its own dwell-window generation below.
            static const space::WorkingSetTide kNoTide;
            scene_.set_working_set_tide(f.tide ? *f.tide : kNoTide);
            up_tide_gen_ = f.tide_gen;
            up_t_ = f.slice_t;
        }
        // T3 (58): the dwell window is part of the tide's DEFINITION, so a
        // window move must re-upload even though the playhead did not — a
        // stale tide would draw a recency claim for a window the reader is no
        // longer looking at.
        if (f.tide_gen != up_tide_gen_) {
            static const space::WorkingSetTide kNoTide2;
            scene_.set_working_set_tide(f.tide ? *f.tide : kNoTide2);
            up_tide_gen_ = f.tide_gen;
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
        // 51 T1/T2: the subject filter. The tid/kind halves are pure
        // draw-time uniforms; the region half needs the per-cell mask, which
        // depends only on (Projection, region index) and is therefore
        // uploaded on ITS OWN gate — never on a playhead scrub, and never per
        // frame.
        scene_.focus = f.focus;
        if (f.focus.region != up_focus_region_) {
            if (f.focus.region >= 0 && f.focus_mask && !f.focus_mask->empty())
                scene_.set_focus_mask(*f.focus_mask, f.terr->w, f.terr->h);
            else
                scene_.clear_focus_mask();
            up_focus_region_ = f.focus.region;
        }

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
        // T1 (59): route to whichever substrate was last rendered. Both save
        // and restore the bound draw framebuffer themselves, so the ImGui
        // renderer's target survives the pick pass either way.
        if (kind_ != scene3d::SceneKind::Plane)
            return standalone_.pick(cam, fbw, fbh, x, y);
        // Scene::pick saves and restores the bound draw framebuffer itself, so the
        // ImGui renderer's target (the default framebuffer) survives the pick pass.
        return scene_.pick(cam, fbw, fbh, x, y);
    }

    float traj_scale() const override { return scene_.traj_scale(); }

    // T3 (47-scene-inspect-and-pickable-overlays): ground truth from the
    // scene's own last upload — see SceneHost::pick_bands()'s doc comment.
    // T1 (59): from whichever substrate is live, so a decode reads the bands
    // of the scene that actually drew the pixel it is decoding.
    scene3d::PickBands pick_bands() const override {
        return kind_ == scene3d::SceneKind::Plane ? scene_.pick_bands()
                                                  : standalone_.pick_bands();
    }

    ~GlSceneHost() override { free_fbo(); }

  private:
    // T1 (59): the non-plane path. Lazily initialises its own programs (a
    // session that never opens a standalone scene builds no extra shaders),
    // re-uploads on a kind change or a recording change/growth, and draws into
    // the same offscreen texture the plane path returns.
    ImTextureID render_standalone(const SceneFrame &f, int w, int h) {
        if (!standalone_inited_) {
            standalone_inited_ = true;
            std::string e;
            standalone_ready_ = standalone_.init_gl(&e);
            if (!standalone_ready_)
                err_ = e;
        }
        if (!standalone_ready_)
            return (ImTextureID)0;
        ensure_fbo(w, h);
        if (fbo_ == 0)
            return (ImTextureID)0;
        scene3d::StandaloneFrame sf;
        sf.kind = f.kind;
        sf.divergence = f.divergence;
        sf.invocation = f.invocation;
        sf.ribbon = f.module_ribbon;
        sf.prism = f.prism;
        sf.flow = f.flow;
        // A kind switch ALWAYS re-uploads: the previous kind's geometry means
        // nothing here, and merging it would be exactly the composition this
        // brief rules out.
        if (kind_ != f.kind || sa_key_ != f.key || sa_gen_ != f.gen ||
            !sa_have_) {
            standalone_.upload(sf);
            sa_key_ = f.key;
            sa_gen_ = f.gen;
            sa_have_ = true;
        }
        kind_ = f.kind;
        GLint prev_fbo = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, w, h);
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        standalone_.render(f.cam, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
        return (ImTextureID)(intptr_t)tex_;
    }

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
    // T1 (59): the second substrate's renderer. A SEPARATE object with its own
    // programs, geometry and pick target — the plane's machinery is not
    // generalised to serve it (T1 step 2's own instruction), only the program
    // link and the R32UI target are shared, through scene3d/glcommon.h.
    scene3d::StandaloneRenderer standalone_;
    bool standalone_inited_ = false;
    bool standalone_ready_ = false;
    scene3d::SceneKind kind_ = scene3d::SceneKind::Plane; // last rendered
    uint64_t sa_key_ = 0, sa_gen_ = 0;
    bool sa_have_ = false;

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
    // T3 (58): the working-set dwell window last uploaded. UINT64_MAX so the
    // very first frame always re-uploads (a real window is never that value).
    uint64_t up_tide_gen_ = UINT64_MAX;
    bool have_upload_ = false;
    // 51 T2: which region index the focus mask currently uploaded is for.
    // -2 == nothing uploaded yet (distinct from -1, "no region focused").
    int32_t up_focus_region_ = -2;
};

} // namespace

std::unique_ptr<SceneHost> make_gl_scene_host() {
    return std::unique_ptr<SceneHost>(new GlSceneHost());
}

} // namespace asmdesk
