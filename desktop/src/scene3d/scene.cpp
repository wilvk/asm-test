// scene.cpp — the GL implementation of scene.h. Raw OpenGL (3.0 core features:
// VAOs, VBOs, integer + float textures, vertex texture fetch, an R32UI pick FBO)
// under the same context 03 stands up. GL entry points are reached through
// GL_GLEXT_PROTOTYPES + libGL (the Linux/Mesa spelling — the desktop lane already
// links -lGL), so no glad/glew/gl3w dependency is added.
//
// Darwin spells the same thing differently and there is no GL/ directory to find:
// the entry points live in the OpenGL framework, which EXPORTS the 3.2 core set
// directly, so <OpenGL/gl3.h> is both the header and the loader and
// GL_GLEXT_PROTOTYPES has nothing to do. Every function this file calls is core
// 3.x (VAOs, FBOs, glBindFragDataLocation, glClearBufferuiv,
// glVertexAttribIPointer), so the framework covers the set with no extension
// loader — the same "no glad/glew" property, reached by the platform's own route.
// gl3.h must not meet the legacy <OpenGL/gl.h> GLFW pulls in; nothing here
// includes glfw3.h, so the two never share a TU.
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/gl3.h>
#include <OpenGL/gl3ext.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "scene3d/glcommon.h" // T1 (59): the SHARED program link + pick target
#include "scene3d/pick.h"
#include "scene3d/scene.h"
#include "scene3d/shaders/embedded.h"

namespace asmdesk::scene3d {

namespace {

// T3 (49): the terrain's iso-density contour band count — enough bands to
// read as a height key without turning the ramp to noise at Phase-A's grid
// sizes.
constexpr float kContourLevels = 12.0f;

// T1 (59-standalone-scenes) step 2: the program compile/link is one of the
// three things a second substrate genuinely SHARES with the plane, so its
// implementation moved to scene3d/glcommon.cpp and every scene links the same
// one. This is a spelling alias only — the call sites below are unchanged, and
// the attribute-location constants (kAttrPos/kAttrVid/kFragId) now live in
// glcommon.h so the two files cannot pick different slots.
GLuint link_program(const char *vs, const char *fs, bool has_vid, bool pick,
                    std::string &err) {
    return gl_link_program(vs, fs, has_vid, pick, err);
}

// A small per-tid palette (opaque exact paths); statistical layers are drawn
// translucent from the same hue.
//
// 51 T1: the table itself moved to the PURE scene3d/focus.h (tid_palette) so
// the HUD thread roster's swatch and the drawn worldline share ONE source —
// unlike overlay_encoding_swatches(), which has to keep a deliberately-synced
// copy because hud.cpp cannot include this GL TU. This wrapper stays so the
// call sites below read unchanged.
void tid_color(int idx, float out[4]) { tid_palette(idx, out); }

} // namespace

Scene::~Scene() { shutdown(); }

bool Scene::init_gl(std::string *err) {
    std::string e;
    prog_terrain_ = link_program(shaders::kTerrainVert, shaders::kTerrainFrag,
                                 false, false, e);
    if (prog_terrain_)
        prog_traj_ = link_program(shaders::kTrajVert, shaders::kTrajFrag, false,
                                  false, e);
    if (prog_traj_)
        prog_pick_terrain_ = link_program(
            shaders::kTerrainVert, shaders::kPickTerrainFrag, false, true, e);
    if (prog_pick_terrain_)
        prog_pick_pt_ = link_program(shaders::kPickPointVert,
                                     shaders::kPickPointFrag, true, true, e);
    // T4 (44): the ghost-fog terrain reuses kTerrainVert (it needs only vUV +
    // the displaced depth, no flags-dependent branch to duplicate) with the
    // new kStatFrag.
    if (prog_pick_pt_)
        prog_stat_ = link_program(shaders::kTerrainVert, shaders::kStatFrag,
                                  false, false, e);
    // T3 (44): the weather sky quad, a separate minimal vert/frag pair.
    if (prog_stat_)
        prog_sky_ =
            link_program(shaders::kSkyVert, shaders::kSkyFrag, false, false, e);
    // T1 (55): the EDL fullscreen pass — joins the same required chain as
    // every other program here (a driver that cannot build it fails ready()
    // loudly, like every shader above, rather than silently disabling EDL).
    if (prog_sky_)
        prog_edl_ =
            link_program(shaders::kEdlVert, shaders::kEdlFrag, false, false, e);
    // T3 (56): the module-canopy quad program — joins the same required chain.
    if (prog_edl_)
        prog_canopy_ = link_program(shaders::kCanopyVert, shaders::kCanopyFrag,
                                    false, false, e);
    if (!prog_canopy_) {
        err_ = e;
        if (err)
            *err = e;
        return false;
    }
    build_sky_quad();
    // T6 (55) step 2: the driver's own answer to how wide glLineWidth can
    // actually go here — see the field's own doc comment (scene.h).
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, aliased_line_width_range);
    ready_ = true;
    return true;
}

void Scene::build_sky_quad() {
    if (vao_sky_)
        return;
    // A full-screen NDC quad, drawn at the far plane with depth-write off
    // (T3 step 5): a triangle strip of 2 XY corners each in [-1, 1].
    static const float kQuad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glGenVertexArrays(1, &vao_sky_);
    glGenBuffers(1, &vbo_sky_);
    glBindVertexArray(vao_sky_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_sky_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
}

// T1/T5 (55-scene-render-quality): create (or resize/resample) fbo_draw_ +
// fbo_resolve_. Idempotent — a no-op once the size AND the effective sample
// count already match, so render() can call this every frame for free.
void Scene::ensure_compose_targets(int w, int h) {
    int samples = static_cast<int>(msaa_samples);
    if (samples > 1) {
        GLint max_samples = 0;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        if (samples > max_samples)
            samples = max_samples;
    } else {
        samples = 0;
    }
    if (fbo_draw_ && fbo_resolve_ && compose_w_ == w && compose_h_ == h &&
        compose_samples_ == samples)
        return;

    free_compose_targets();
    compose_w_ = w;
    compose_h_ = h;

    // The resolve target: single-sample colour + depth as TEXTURES (never
    // renderbuffers) — the EDL pass and the plain-blit fallback both need to
    // SAMPLE these, which a renderbuffer cannot offer a plain sampler2D.
    glGenTextures(1, &tex_resolve_color_);
    glBindTexture(GL_TEXTURE_2D, tex_resolve_color_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                nullptr);
    glGenTextures(1, &tex_resolve_depth_);
    glBindTexture(GL_TEXTURE_2D, tex_resolve_depth_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0,
                GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &fbo_resolve_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_resolve_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex_resolve_color_, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           tex_resolve_depth_, 0);
    const bool resolve_ok =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // The draw target: renderbuffers, multisampled when samples >= 2. Tried at
    // the clamped sample count first; T5 step 4 (degrade rather than fail): a
    // driver that refuses THIS multisample renderbuffer is not a driver that
    // cannot do offscreen rendering at all, so on failure retry once at 0
    // samples before giving up.
    bool draw_ok = false;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            if (samples == 0)
                break; // already tried 0 samples once; nothing left to retry
            if (rbo_draw_color_)
                glDeleteRenderbuffers(1, &rbo_draw_color_);
            if (rbo_draw_depth_)
                glDeleteRenderbuffers(1, &rbo_draw_depth_);
            if (fbo_draw_)
                glDeleteFramebuffers(1, &fbo_draw_);
            rbo_draw_color_ = rbo_draw_depth_ = fbo_draw_ = 0;
            samples = 0;
        }
        glGenRenderbuffers(1, &rbo_draw_color_);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo_draw_color_);
        if (samples >= 2)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                             GL_RGBA8, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
        glGenRenderbuffers(1, &rbo_draw_depth_);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo_draw_depth_);
        if (samples >= 2)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples,
                                             GL_DEPTH_COMPONENT24, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glGenFramebuffers(1, &fbo_draw_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, rbo_draw_color_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rbo_draw_depth_);
        draw_ok =
            glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (draw_ok)
            break;
    }

    compose_samples_ = samples;
    compose_ready_ = draw_ok && resolve_ok;
}

void Scene::free_compose_targets() {
    if (rbo_draw_color_)
        glDeleteRenderbuffers(1, &rbo_draw_color_);
    if (rbo_draw_depth_)
        glDeleteRenderbuffers(1, &rbo_draw_depth_);
    if (fbo_draw_)
        glDeleteFramebuffers(1, &fbo_draw_);
    if (tex_resolve_color_)
        glDeleteTextures(1, &tex_resolve_color_);
    if (tex_resolve_depth_)
        glDeleteTextures(1, &tex_resolve_depth_);
    if (fbo_resolve_)
        glDeleteFramebuffers(1, &fbo_resolve_);
    rbo_draw_color_ = rbo_draw_depth_ = fbo_draw_ = 0;
    tex_resolve_color_ = tex_resolve_depth_ = fbo_resolve_ = 0;
    compose_w_ = compose_h_ = 0;
    compose_samples_ = -1;
    compose_ready_ = false;
}

// T1 (55): the fullscreen EDL composite — reads fbo_resolve_'s colour+depth,
// writes the shaded result into whichever framebuffer is CURRENTLY bound
// (render() binds the caller's framebuffer immediately before calling this).
void Scene::draw_edl_pass(const Camera &cam, int fbw, int fbh) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glUseProgram(prog_edl_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_resolve_color_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_resolve_depth_);
    GLint uColorLoc = glGetUniformLocation(prog_edl_, "uColor");
    if (uColorLoc >= 0)
        glUniform1i(uColorLoc, 0);
    GLint uDepthLoc = glGetUniformLocation(prog_edl_, "uDepth");
    if (uDepthLoc >= 0)
        glUniform1i(uDepthLoc, 1);
    glUniform2f(glGetUniformLocation(prog_edl_, "uTexel"),
               fbw > 0 ? 1.0f / fbw : 0.0f, fbh > 0 ? 1.0f / fbh : 0.0f);
    glUniform1f(glGetUniformLocation(prog_edl_, "uEdlStrength"), edl_strength);
    glUniform1f(glGetUniformLocation(prog_edl_, "uEdlRadiusPx"),
               edl_radius_px);
    glUniform1f(glGetUniformLocation(prog_edl_, "uNear"), cam.znear);
    glUniform1f(glGetUniformLocation(prog_edl_, "uFar"), cam.zfar);
    glBindVertexArray(vao_sky_); // reuse the sky quad's NDC-quad geometry
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
}

void Scene::build_grid(uint32_t n) {
    if (n_ == n && vao_grid_)
        return;
    n_ = n;
    // n*n vertices at cell coordinates (x,y); (n-1)^2 quads -> 2 triangles each.
    std::vector<float> cells;
    cells.reserve(static_cast<size_t>(n) * n * 2);
    for (uint32_t y = 0; y < n; y++)
        for (uint32_t x = 0; x < n; x++) {
            cells.push_back(static_cast<float>(x));
            cells.push_back(static_cast<float>(y));
        }
    std::vector<unsigned> idx;
    idx.reserve(static_cast<size_t>(n - 1) * (n - 1) * 6);
    auto at = [n](uint32_t x, uint32_t y) { return y * n + x; };
    for (uint32_t y = 0; y + 1 < n; y++)
        for (uint32_t x = 0; x + 1 < n; x++) {
            unsigned a = at(x, y), b = at(x + 1, y), c = at(x, y + 1),
                     d = at(x + 1, y + 1);
            idx.insert(idx.end(), {a, b, c, b, d, c});
        }
    grid_index_count_ = static_cast<int>(idx.size());

    if (!vao_grid_)
        glGenVertexArrays(1, &vao_grid_);
    if (!vbo_cell_)
        glGenBuffers(1, &vbo_cell_);
    if (!ibo_grid_)
        glGenBuffers(1, &ibo_grid_);
    glBindVertexArray(vao_grid_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_cell_);
    glBufferData(GL_ARRAY_BUFFER, cells.size() * sizeof(float), cells.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(kAttrPos);
    glVertexAttribPointer(kAttrPos, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo_grid_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(unsigned),
                 idx.data(), GL_STATIC_DRAW);
    glBindVertexArray(0);
}

void Scene::set_terrain(const space::Terrain &t) {
    if (t.w == 0 || t.h == 0)
        return;
    build_grid(t.w);

    // Normalise heights to [0,1] so clamp(vHeight,0,1) ranks cells by density.
    float maxh = 0.0f;
    for (float h : t.height)
        maxh = std::max(maxh, h);
    const float scale = maxh > 0.0f ? 1.0f / maxh : 1.0f;
    std::vector<float> norm(t.height.size());
    for (size_t i = 0; i < t.height.size(); i++)
        norm[i] = t.height[i] * scale;

    if (!tex_height_)
        glGenTextures(1, &tex_height_);
    glBindTexture(GL_TEXTURE_2D, tex_height_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, t.w, t.h, 0, GL_RED, GL_FLOAT,
                 norm.data());

    if (!tex_flags_)
        glGenTextures(1, &tex_flags_);
    glBindTexture(GL_TEXTURE_2D, tex_flags_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, t.w, t.h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_INT, t.flags.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// 51 T2 (scene-focus-and-scale): the focused REGION's per-cell mask — the one
// piece of the subject filter a fragment shader cannot derive on its own (a
// Hilbert footprint is a real cell SET, not a rectangle). Clones set_zoning's
// texture shape verbatim; an empty/mis-sized mask clears the filter rather
// than uploading a plane of zeroes, because a filter that hid EVERYTHING
// would be a lie about the recording, not a filter.
void Scene::set_focus_mask(const std::vector<uint8_t> &mask, uint32_t w,
                           uint32_t h) {
    if (w == 0 || h == 0 || mask.size() != static_cast<size_t>(w) * h) {
        has_focus_mask_ = false;
        return;
    }
    if (!tex_focus_)
        glGenTextures(1, &tex_focus_);
    glBindTexture(GL_TEXTURE_2D, tex_focus_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_BYTE, mask.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    has_focus_mask_ = true;
}

void Scene::set_zoning(const std::vector<uint8_t> &kind_by_cell, uint32_t w,
                       uint32_t h) {
    if (w == 0 || h == 0 || kind_by_cell.size() != static_cast<size_t>(w) * h)
        return;
    if (!tex_kind_)
        glGenTextures(1, &tex_kind_);
    glBindTexture(GL_TEXTURE_2D, tex_kind_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_BYTE, kind_by_cell.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

// T4 (56-fidelity-and-module-layers): the per-cell dominant-opcode-class byte
// map. 255 (never a real OpClass value, which is 0..7) marks a cell
// build_opcode_terrain produced no entry for — a data cell, or a code cell
// with no CodeCell at all — so the shader's `oc < 8u` guard leaves it
// untinted rather than reading texel 0 (Unknown) by accident.
void Scene::set_opcode_terrain(const std::vector<space::CellOpcode> &cells,
                               uint32_t w, uint32_t h) {
    if (w == 0 || h == 0)
        return;
    std::vector<uint8_t> byte_map(static_cast<size_t>(w) * h, 255u);
    for (const space::CellOpcode &co : cells)
        if (co.cell < byte_map.size())
            byte_map[co.cell] = static_cast<uint8_t>(co.dominant);
    if (!tex_opclass_)
        glGenTextures(1, &tex_opclass_);
    glBindTexture(GL_TEXTURE_2D, tex_opclass_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, w, h, 0, GL_RED_INTEGER,
                GL_UNSIGNED_BYTE, byte_map.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Scene::set_stat_terrain(const space::Terrain &t) {
    if (t.w == 0 || t.h == 0)
        return;
    build_grid(
        t.w); // same dims as the exact terrain; idempotent if already built

    float maxh = 0.0f;
    for (float h : t.height)
        maxh = std::max(maxh, h);
    const float scale = maxh > 0.0f ? 1.0f / maxh : 1.0f;
    std::vector<float> norm(t.height.size());
    for (size_t i = 0; i < t.height.size(); i++)
        norm[i] = t.height[i] * scale;

    if (!tex_height_stat_)
        glGenTextures(1, &tex_height_stat_);
    glBindTexture(GL_TEXTURE_2D, tex_height_stat_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, t.w, t.h, 0, GL_RED, GL_FLOAT,
                 norm.data());

    if (!tex_flags_stat_)
        glGenTextures(1, &tex_flags_stat_);
    glBindTexture(GL_TEXTURE_2D, tex_flags_stat_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, t.w, t.h, 0, GL_RED_INTEGER,
                 GL_UNSIGNED_INT, t.flags.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    has_stat_terrain_ = true;
}

void Scene::free_canopies() {
    for (CanopyDraw &d : canopy_draws_) {
        if (d.vbo)
            glDeleteBuffers(1, &d.vbo);
        if (d.vao)
            glDeleteVertexArrays(1, &d.vao);
    }
    canopy_draws_.clear();
}

// T3 (56-fidelity-and-module-layers): one flat quad per canopy, at its
// footprint's bounding box (space::ModuleCanopy::min_u/v..max_u/v) and world
// Y = height * y_scale — the SAME exaggeration the terrain's own vertex
// shader applies, so a canopy's height reads on the same scale as the
// terrain beneath it. A statistical entry sits a fixed 0.02 world units
// higher (never merged with the exact quad — the T6 isolation invariant)
// and is desaturated + more transparent, mirroring the ghost-fog terrain's
// own "separate, dimmer" idiom without a second shader technique.
void Scene::set_module_canopies(const std::vector<space::ModuleCanopy> &canopies,
                                const space::Projection &proj) {
    free_canopies();
    canopy_draws_.reserve(canopies.size());
    for (const space::ModuleCanopy &mc : canopies) {
        if (mc.region >= proj.regions.size())
            continue; // defensive: a canopy always names a real region
        const space::RegionStyle st =
            space::region_style(proj.regions[mc.region].kind);
        const float y = static_cast<float>(mc.height) * y_scale +
                        (mc.statistical ? 0.02f : 0.0f);
        float r = st.r, g = st.g, b = st.b;
        if (mc.torn) {
            // The SAME red-gash mix the terrain shader applies to a torn
            // cell (embedded.h's kTerrainFrag) — a torn module reads hatched
            // in spirit (loudly red-shifted), not silently unremarked.
            r = r * 0.3f + 1.0f * 0.7f;
            g = g * 0.3f + 0.15f * 0.7f;
            b = b * 0.3f + 0.15f * 0.7f;
        }
        const float alpha = mc.statistical ? 0.22f : 0.32f;
        if (mc.statistical) {
            const float grey = (r + g + b) / 3.0f;
            r = g = b = grey; // desaturated, never the exact palette's hue
        }

        const float x0 = mc.min_u, x1 = mc.max_u;
        const float z0 = mc.min_v, z1 = mc.max_v;
        const float quad[18] = {
            x0, y, z0, x1, y, z0, x1, y, z1, // first triangle
            x0, y, z0, x1, y, z1, x0, y, z1, // second triangle
        };
        CanopyDraw d;
        d.color[0] = r;
        d.color[1] = g;
        d.color[2] = b;
        d.color[3] = alpha;
        glGenVertexArrays(1, &d.vao);
        glGenBuffers(1, &d.vbo);
        glBindVertexArray(d.vao);
        glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        canopy_draws_.push_back(d);
    }
}

void Scene::free_mispred() {
    for (MispredArcDraw &d : mispred_arcs_) {
        if (d.vbo)
            glDeleteBuffers(1, &d.vbo);
        if (d.vao)
            glDeleteVertexArrays(1, &d.vao);
    }
    mispred_arcs_.clear();
    for (MispredColumnDraw &d : mispred_columns_) {
        if (d.vbo_sheath)
            glDeleteBuffers(1, &d.vbo_sheath);
        if (d.vao_sheath)
            glDeleteVertexArrays(1, &d.vao_sheath);
        if (d.vbo_core)
            glDeleteBuffers(1, &d.vbo_core);
        if (d.vao_core)
            glDeleteVertexArrays(1, &d.vao_core);
    }
    mispred_columns_.clear();
}

namespace {
// cool (low mispred/count) -> amber (high) — the SAME ramp idiom
// misprediction rate uses everywhere else this catalog reads it.
void mispred_ramp(double ratio, float *r, float *g, float *b) {
    ratio = std::min(1.0, std::max(0.0, ratio));
    const float cool[3] = {0.3f, 0.55f, 0.95f};
    const float amber[3] = {1.0f, 0.65f, 0.15f};
    *r = static_cast<float>(cool[0] + (amber[0] - cool[0]) * ratio);
    *g = static_cast<float>(cool[1] + (amber[1] - cool[1]) * ratio);
    *b = static_cast<float>(cool[2] + (amber[2] - cool[2]) * ratio);
}
} // namespace

// T5 (56-fidelity-and-module-layers): a whole-recording survey aggregate —
// upload once per weave, like the stat terrain. One draw per arc/site (the
// same "a handful" scale canopies assume; hot edges are already a RANKED,
// bounded set upstream, obs_hotedges_top's own convention).
void Scene::set_mispred_layer(const space::MispredLayer &layer) {
    free_mispred();
    mispred_arcs_.reserve(layer.arcs.size());
    for (const space::MispredArc &a : layer.arcs) {
        const float ya = 0.0f, yb = 0.0f; // arcs ride the plane itself, not
                                          // the trajectory time axis
        const float dx = a.ub - a.ua, dz = a.vb - a.va;
        const float chord = std::sqrt(dx * dx + dz * dz);
        const double logCount = std::log10(static_cast<double>(a.count) + 1.0);
        const float lift =
            0.05f + 0.5f * static_cast<float>(logCount) * (0.3f + chord);
        const float cx = 0.5f * (a.ua + a.ub);
        const float cy = std::max(ya, yb) + lift;
        const float cz = 0.5f * (a.va + a.vb);
        const int kSteps = 16;
        std::vector<float> pts;
        pts.reserve(static_cast<size_t>(kSteps + 1) * 3);
        for (int k = 0; k <= kSteps; k++) {
            const float s = static_cast<float>(k) / kSteps;
            const float o = 1.0f - s;
            pts.push_back(o * o * a.ua + 2 * o * s * cx + s * s * a.ub);
            pts.push_back(o * o * ya + 2 * o * s * cy + s * s * yb);
            pts.push_back(o * o * a.va + 2 * o * s * cz + s * s * a.vb);
        }
        MispredArcDraw d;
        d.count = kSteps + 1;
        const double ratio =
            a.count > 0 ? static_cast<double>(a.mispred) / a.count : 0.0;
        mispred_ramp(ratio, &d.color[0], &d.color[1], &d.color[2]);
        // is_return: never dashed here (a scoped simplification — kTrajFrag
        // has no dash mode; see this brief's own status note) but a hair
        // dimmer, so it is at least visually distinguishable.
        d.color[3] = a.is_return ? 0.55f : 0.85f;
        d.line_width =
            std::min(4.0f, std::max(1.0f, 1.0f + static_cast<float>(logCount)));
        glGenVertexArrays(1, &d.vao);
        glGenBuffers(1, &d.vbo);
        glBindVertexArray(d.vao);
        glBindBuffer(GL_ARRAY_BUFFER, d.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(pts.size() * sizeof(float)),
                    pts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        mispred_arcs_.push_back(d);
    }

    mispred_columns_.reserve(layer.sites.size());
    for (const space::MispredSite &s : layer.sites) {
        const float sheath_h =
            static_cast<float>(std::log1p(static_cast<double>(s.count)));
        const float core_h =
            static_cast<float>(std::log1p(static_cast<double>(s.mispred)));
        MispredColumnDraw d;
        d.sheath_color[3] = 0.30f;
        const double ratio =
            s.count > 0 ? static_cast<double>(s.mispred) / s.count : 0.0;
        mispred_ramp(ratio, &d.core_color[0], &d.core_color[1],
                    &d.core_color[2]);
        // mispred == 0: a genuinely cool, ZERO-HEIGHT core inside a full
        // sheath — a real measured low, deliberately distinct from a layer
        // that is absent (T5's own fidelity note).
        const float sheath[6] = {s.u, 0.0f, s.v, s.u, sheath_h, s.v};
        const float core[6] = {s.u, 0.0f, s.v, s.u, core_h, s.v};
        glGenVertexArrays(1, &d.vao_sheath);
        glGenBuffers(1, &d.vbo_sheath);
        glBindVertexArray(d.vao_sheath);
        glBindBuffer(GL_ARRAY_BUFFER, d.vbo_sheath);
        glBufferData(GL_ARRAY_BUFFER, sizeof(sheath), sheath, GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        glGenVertexArrays(1, &d.vao_core);
        glGenBuffers(1, &d.vbo_core);
        glBindVertexArray(d.vao_core);
        glBindBuffer(GL_ARRAY_BUFFER, d.vbo_core);
        glBufferData(GL_ARRAY_BUFFER, sizeof(core), core, GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        mispred_columns_.push_back(d);
    }
}

void Scene::free_traj() {
    for (Line &l : traj_lines_) {
        if (l.vbo)
            glDeleteBuffers(1, &l.vbo);
        if (l.vao)
            glDeleteVertexArrays(1, &l.vao);
    }
    traj_lines_.clear();
    if (access_spurs_.vbo)
        glDeleteBuffers(1, &access_spurs_.vbo);
    if (access_spurs_.vao)
        glDeleteVertexArrays(1, &access_spurs_.vao);
    access_spurs_ = Line{};
    if (vbo_pts_pos_)
        glDeleteBuffers(1, &vbo_pts_pos_);
    if (vbo_pts_id_)
        glDeleteBuffers(1, &vbo_pts_id_);
    if (vao_pts_)
        glDeleteVertexArrays(1, &vao_pts_);
    vbo_pts_pos_ = vbo_pts_id_ = vao_pts_ = 0;
    pts_count_ = 0;
    // T3 (47): the spur PICK id buffer (built in set_convergences, once npts
    // AND nconv are both known — see that function's own doc comment) reuses
    // access_spurs_.vbo for position, so it must not outlive it.
    if (vbo_spur_pick_id_)
        glDeleteBuffers(1, &vbo_spur_pick_id_);
    if (vao_spur_pick_)
        glDeleteVertexArrays(1, &vao_spur_pick_);
    vbo_spur_pick_id_ = vao_spur_pick_ = 0;
    nspur_ = 0;
}

void Scene::set_trajectories(const space::TrajectorySet &ts,
                             const space::Projection &proj) {
    free_traj();
    const float scale =
        time_scale > 0.0f ? time_scale : (nsteps > 0 ? 0.6f / nsteps : 0.02f);
    traj_scale_ = scale; // T6: render() derives uHeadY from this later

    std::vector<float> spur_verts; // GL_LINES pairs
    std::vector<float> pts_pos;    // pick points (all PC vertices)
    std::vector<unsigned> pts_id;
    uint64_t vindex = 0;
    int tid_seq = 0;

    for (const space::Trajectory &tr : ts.trajectories) {
        std::vector<float> line; // PC vertices as a strip
        float last_pc[3] = {0, 0, 0};
        bool have_last_pc = false;
        // T6: parallel per-PC-vertex facts (see Line::pt_* doc comment) — one
        // entry per `!is_access` point, in the SAME order pick_vertex_order
        // enumerates them, so pt_pick_id can reuse pick_id_vertex(n_, vindex)
        // directly (no second id scheme).
        std::vector<uint64_t> pt_t;
        std::vector<uint8_t> pt_placed;
        std::vector<float> pt_pos;
        std::vector<uint32_t> pt_pick_id;
        for (const space::TrajPoint &pt : tr.points) {
            float u = 0, v = 0;
            bool projected = proj.project(pt.addr, &u, &v);
            float y = static_cast<float>(pt.t) * scale;
            if (!pt.is_access) {
                // Every PC vertex is pickable, in the canonical order pick.h
                // enumerates (project failures still consume an index so the
                // scene's ids stay aligned with resolve_pick's replay).
                pts_pos.insert(pts_pos.end(), {projected ? u : -1.0f, y,
                                               projected ? v : -1.0f});
                const uint32_t pid = pick_id_vertex(n_, vindex);
                pts_id.push_back(pid);
                vindex++;
                pt_t.push_back(pt.t);
                pt_placed.push_back((pt.placed && projected) ? 1u : 0u);
                pt_pos.insert(pt_pos.end(),
                              {projected ? u : 0.0f, y, projected ? v : 0.0f});
                pt_pick_id.push_back(pid);
                if (projected) {
                    line.insert(line.end(), {u, y, v});
                    last_pc[0] = u;
                    last_pc[1] = y;
                    last_pc[2] = v;
                    have_last_pc = true;
                }
            } else if (projected && have_last_pc) {
                // Access-mark spur: PC vertex -> the data cell at the same height.
                spur_verts.insert(spur_verts.end(), {last_pc[0], last_pc[1],
                                                     last_pc[2], u, y, v});
            }
        }
        if (line.size() >= 6) { // at least two vertices
            Line l;
            l.statistical = (tr.flags & space::TRAJ_STATISTICAL) != 0;
            // 51 T1: carry the thread identity into the renderer. The split
            // was already here (one Line per Trajectory); only the identity
            // was missing, so render() could not ghost the other threads.
            l.tid = tr.tid;
            tid_color(tr.tid >= 0 ? tr.tid : tid_seq, l.color);
            if (l.statistical)
                l.color[3] = 0.45f; // translucent — never a solid exact tube
            l.count = static_cast<int>(line.size() / 3);
            glGenVertexArrays(1, &l.vao);
            glGenBuffers(1, &l.vbo);
            glBindVertexArray(l.vao);
            glBindBuffer(GL_ARRAY_BUFFER, l.vbo);
            glBufferData(GL_ARRAY_BUFFER, line.size() * sizeof(float),
                         line.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(kAttrPos);
            glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
            glBindVertexArray(0);
            l.pt_t = std::move(pt_t);
            l.pt_placed = std::move(pt_placed);
            l.pt_pos = std::move(pt_pos);
            l.pt_pick_id = std::move(pt_pick_id);
            traj_lines_.push_back(std::move(l));
        }
        tid_seq++;
    }

    if (!spur_verts.empty()) {
        Line &l = access_spurs_;
        l.count = static_cast<int>(spur_verts.size() / 3);
        l.color[0] = 0.85f;
        l.color[1] = 0.85f;
        l.color[2] = 0.90f;
        l.color[3] = 0.8f;
        glGenVertexArrays(1, &l.vao);
        glGenBuffers(1, &l.vbo);
        glBindVertexArray(l.vao);
        glBindBuffer(GL_ARRAY_BUFFER, l.vbo);
        glBufferData(GL_ARRAY_BUFFER, spur_verts.size() * sizeof(float),
                     spur_verts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
    }

    if (!pts_pos.empty()) {
        pts_count_ = static_cast<int>(pts_pos.size() / 3);
        glGenVertexArrays(1, &vao_pts_);
        glGenBuffers(1, &vbo_pts_pos_);
        glGenBuffers(1, &vbo_pts_id_);
        glBindVertexArray(vao_pts_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_pts_pos_);
        glBufferData(GL_ARRAY_BUFFER, pts_pos.size() * sizeof(float),
                     pts_pos.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_pts_id_);
        glBufferData(GL_ARRAY_BUFFER, pts_id.size() * sizeof(unsigned),
                     pts_id.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrVid);
        glVertexAttribIPointer(kAttrVid, 1, GL_UNSIGNED_INT, 0, nullptr);
        glBindVertexArray(0);
    }
}

void Scene::free_conv() {
    if (conv_arcs_.vbo)
        glDeleteBuffers(1, &conv_arcs_.vbo);
    if (conv_arcs_.vao)
        glDeleteVertexArrays(1, &conv_arcs_.vao);
    conv_arcs_ = Line{};
    // T3 (47): the conv/spur PICK id buffers are built together below (both
    // need `npts`, and the spur band's own encoding additionally needs
    // `nconv` — see pick_id_spur's doc comment — so both are only knowable
    // once THIS function runs, after set_trajectories has fixed npts).
    if (vbo_conv_pick_id_)
        glDeleteBuffers(1, &vbo_conv_pick_id_);
    if (vao_conv_pick_)
        glDeleteVertexArrays(1, &vao_conv_pick_);
    vbo_conv_pick_id_ = vao_conv_pick_ = 0;
    nconv_ = 0;
    if (vbo_spur_pick_id_)
        glDeleteBuffers(1, &vbo_spur_pick_id_);
    if (vao_spur_pick_)
        glDeleteVertexArrays(1, &vao_spur_pick_);
    vbo_spur_pick_id_ = vao_spur_pick_ = 0;
    nspur_ = 0;
}

void Scene::set_convergences(const space::ConvergenceSet &cs,
                             const space::Projection &proj) {
    // Independent of the trajectory buffers (set_trajectories does NOT touch this),
    // so uploading the growing paths each frame does not wipe the arcs. Arc vertices
    // are baked world-space positions, so they stay valid across a path re-upload.
    free_conv();
    const float scale =
        time_scale > 0.0f ? time_scale : (nsteps > 0 ? 0.6f / nsteps : 0.02f);

    // T3 (47): nconv_ is the FULL mark count — pick_id_conv's band is sized by
    // cs.marks.size(), not by how many actually draw, so an id past an
    // unplaced-endpoint mark still lands correctly on the NEXT drawable one
    // (an empty/unplaced mark changes no other id, the brief's own bar).
    nconv_ = static_cast<int>(cs.marks.size());

    std::vector<float>
        segs; // GL_LINES pairs: each arc tessellated into segments
    std::vector<unsigned> conv_ids; // one id per vertex in `segs`, 32/arc
    for (size_t mi = 0; mi < cs.marks.size(); ++mi) {
        const space::ConvergenceMark &m = cs.marks[mi];
        float ua = 0, va = 0, ub = 0, vb = 0;
        if (!proj.project(m.addr_a, &ua, &va) ||
            !proj.project(m.addr_b, &ub, &vb))
            continue; // an unplaced endpoint draws no arc (faithful: no false line)
        const float ya = static_cast<float>(m.t_a) * scale;
        const float yb = static_cast<float>(m.t_b) * scale;
        // A quadratic-bezier bow: the control point sits above the higher endpoint
        // so the arc lifts clear of the terrain and reads as a cross-thread link,
        // not another trajectory line. The lift grows with the chord so distant
        // threads arc higher.
        const float dx = ub - ua, dz = vb - va;
        const float chord = std::sqrt(dx * dx + dz * dz);
        const float lift = 0.12f + 0.45f * chord;
        const float cx = 0.5f * (ua + ub);
        const float cy = std::max(ya, yb) + lift;
        const float cz = 0.5f * (va + vb);
        const int kSteps = 16;
        const unsigned pid = pick_id_conv(n_, static_cast<uint64_t>(pts_count_),
                                          static_cast<uint64_t>(mi));
        float px = ua, py = ya, pz = va;
        for (int k = 1; k <= kSteps; k++) {
            float s = static_cast<float>(k) / kSteps;
            float o = 1.0f - s;
            // B(s) = o^2 P0 + 2 o s C + s^2 P2
            float qx = o * o * ua + 2 * o * s * cx + s * s * ub;
            float qy = o * o * ya + 2 * o * s * cy + s * s * yb;
            float qz = o * o * va + 2 * o * s * cz + s * s * vb;
            segs.insert(segs.end(), {px, py, pz, qx, qy, qz});
            conv_ids.push_back(pid); // T3: the WHOLE arc shares one id
            conv_ids.push_back(pid);
            px = qx;
            py = qy;
            pz = qz;
        }
    }

    if (!segs.empty()) {
        Line &l = conv_arcs_;
        // A bright magenta distinct from every tid colour and from the torn-red
        // gash and the lavender access spur — the "two threads on the same line"
        // hint reads at a glance and cannot be mistaken for a path.
        l.color[0] = 1.0f;
        l.color[1] = 0.25f;
        l.color[2] = 0.85f;
        l.color[3] = 0.95f;
        l.count = static_cast<int>(segs.size() / 3);
        glGenVertexArrays(1, &l.vao);
        glGenBuffers(1, &l.vbo);
        glBindVertexArray(l.vao);
        glBindBuffer(GL_ARRAY_BUFFER, l.vbo);
        glBufferData(GL_ARRAY_BUFFER, segs.size() * sizeof(float), segs.data(),
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);

        // T3: the pick-pass twin of the SAME position buffer (reused, not
        // re-uploaded) plus the per-vertex id array just built above — mirrors
        // vao_pts_/vbo_pts_id_'s existing "pos"+"vid" layout for prog_pick_pt_.
        glGenVertexArrays(1, &vao_conv_pick_);
        glGenBuffers(1, &vbo_conv_pick_id_);
        glBindVertexArray(vao_conv_pick_);
        glBindBuffer(GL_ARRAY_BUFFER, l.vbo);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_conv_pick_id_);
        glBufferData(GL_ARRAY_BUFFER, conv_ids.size() * sizeof(unsigned),
                     conv_ids.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrVid);
        glVertexAttribIPointer(kAttrVid, 1, GL_UNSIGNED_INT, 0, nullptr);
        glBindVertexArray(0);
    }

    // T3: the access-spur pick-id buffer — built HERE (not in
    // set_trajectories) because pick_id_spur needs BOTH npts (fixed by the
    // most recent set_trajectories, which always runs before this per
    // gl_scene_host.cpp's upload gate) and nconv_ (just computed above).
    // access_spurs_' POSITION buffer is untouched (built by set_trajectories,
    // reused here) — this only adds the parallel id array.
    if (access_spurs_.vbo && access_spurs_.count > 0) {
        nspur_ = access_spurs_.count / 2;
        std::vector<unsigned> spur_ids;
        spur_ids.reserve(static_cast<size_t>(access_spurs_.count));
        for (int si = 0; si < nspur_; ++si) {
            const unsigned pid = pick_id_spur(
                n_, static_cast<uint64_t>(pts_count_),
                static_cast<uint64_t>(nconv_), static_cast<uint64_t>(si));
            spur_ids.push_back(pid);
            spur_ids.push_back(pid); // both endpoints of the spur share one id
        }
        glGenVertexArrays(1, &vao_spur_pick_);
        glGenBuffers(1, &vbo_spur_pick_id_);
        glBindVertexArray(vao_spur_pick_);
        glBindBuffer(GL_ARRAY_BUFFER, access_spurs_.vbo);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_spur_pick_id_);
        glBufferData(GL_ARRAY_BUFFER, spur_ids.size() * sizeof(unsigned),
                     spur_ids.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(kAttrVid);
        glVertexAttribIPointer(kAttrVid, 1, GL_UNSIGNED_INT, 0, nullptr);
        glBindVertexArray(0);
    }
}

void Scene::draw_terrain_common(unsigned prog, const float mvp[16], bool zoning,
                                float contour_levels, bool confidence,
                                bool opcode) {
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_height_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_flags_);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_kind_); // T1: harmless if 0 / unused
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tex_opclass_); // T4 (56): harmless if 0/unused
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, tex_focus_); // 51 T2: harmless if 0/unused
    glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
    glUniform1f(glGetUniformLocation(prog, "uYScale"), y_scale);
    glUniform1f(glGetUniformLocation(prog, "uN"), static_cast<float>(n_));
    GLint uh = glGetUniformLocation(prog, "uHeight");
    if (uh >= 0)
        glUniform1i(uh, 0);
    GLint uf = glGetUniformLocation(prog, "uFlags");
    if (uf >= 0)
        glUniform1i(uf, 1);
    GLint uk = glGetUniformLocation(prog, "uKind");
    if (uk >= 0)
        glUniform1i(uk, 2);
    // T7: SceneLayers::zoning off -> uZoning=0, the shader falls back to
    // kindHue[Code] (today's plain amber ramp) regardless of uKind.
    GLint uz = glGetUniformLocation(prog, "uZoning");
    if (uz >= 0)
        glUniform1i(uz, zoning ? 1 : 0);
    // T3 (49): <=0 disables banding — absent from the pick shader (loc -1,
    // harmless) and from prog_stat_ (the ghost-fog terrain never bands).
    GLint uc = glGetUniformLocation(prog, "uContourLevels");
    if (uc >= 0)
        glUniform1f(uc, contour_levels);
    // 50 T2: only kTerrainFrag (the exact terrain) declares uHighlightCell —
    // absent (loc -1, harmless) from the pick shader and prog_stat_, so this
    // call is safe for every draw_terrain_common caller regardless of `prog`.
    GLint uhc = glGetUniformLocation(prog, "uHighlightCell");
    if (uhc >= 0)
        glUniform1i(uhc, highlight_cell);
    // T2 (56-fidelity-and-module-layers): absent from the pick shader and
    // prog_stat_ (loc -1, harmless) — only kTerrainFrag declares it.
    GLint uconf = glGetUniformLocation(prog, "uConfidence");
    if (uconf >= 0)
        glUniform1i(uconf, confidence ? 1 : 0);
    // T4 (56): absent from the pick shader and prog_stat_ (loc -1, harmless)
    // — only kTerrainFrag declares uOpcode/uOpClass.
    GLint uop = glGetUniformLocation(prog, "uOpcode");
    if (uop >= 0)
        glUniform1i(uop, opcode ? 1 : 0);
    GLint uoc = glGetUniformLocation(prog, "uOpClass");
    if (uoc >= 0)
        glUniform1i(uoc, 3);
    // 51 T2: the subject filter. Declared ONLY by kTerrainFrag — absent (loc
    // -1, harmless) from the pick shader and prog_stat_, so a filter can
    // never reach the PICK pass (a desaturated cell must stay exactly as
    // clickable as a saturated one) nor the ghost-fog surface.
    GLint ukm = glGetUniformLocation(prog, "uKindMask");
    if (ukm >= 0)
        glUniform1ui(ukm, focus.kind_mask);
    GLint ufr = glGetUniformLocation(prog, "uHasFocusRegion");
    if (ufr >= 0)
        glUniform1i(ufr, (focus.region >= 0 && has_focus_mask_) ? 1 : 0);
    GLint ufm = glGetUniformLocation(prog, "uFocusMask");
    if (ufm >= 0)
        glUniform1i(ufm, 4);
    glBindVertexArray(vao_grid_);
    glDrawElements(GL_TRIANGLES, grid_index_count_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

// T4 (44): the SEPARATE statistical terrain — same draw shape as
// draw_terrain_common, but its own program + its own texture pair, so it can
// never alias the exact terrain's.
void Scene::draw_stat_terrain(const float mvp[16]) {
    glUseProgram(prog_stat_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_height_stat_);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_flags_stat_);
    glUniformMatrix4fv(glGetUniformLocation(prog_stat_, "uMVP"), 1, GL_FALSE,
                       mvp);
    glUniform1f(glGetUniformLocation(prog_stat_, "uYScale"), y_scale);
    glUniform1f(glGetUniformLocation(prog_stat_, "uN"), static_cast<float>(n_));
    GLint uh = glGetUniformLocation(prog_stat_, "uHeight");
    if (uh >= 0)
        glUniform1i(uh, 0);
    GLint uf = glGetUniformLocation(prog_stat_, "uFlags");
    if (uf >= 0)
        glUniform1i(uf, 1);
    glBindVertexArray(vao_grid_);
    glDrawElements(GL_TRIANGLES, grid_index_count_, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

// T3 (44): the weather sky quad — drawn FIRST, far plane, depth-write off.
void Scene::draw_sky() {
    if (!prog_sky_ || !vao_sky_)
        return;
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(prog_sky_);
    glUniform3fv(glGetUniformLocation(prog_sky_, "uAmbient"), 1, atmo_.ambient);
    glUniform3fv(glGetUniformLocation(prog_sky_, "uFrontColor"), 1,
                 atmo_.front);
    glUniform3fv(glGetUniformLocation(prog_sky_, "uSunDir"), 1, atmo_.sun_dir);
    glUniform1f(glGetUniformLocation(prog_sky_, "uFogDensity"),
                atmo_.fog_density);
    glBindVertexArray(vao_sky_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
}

// T3 (56-fidelity-and-module-layers): the per-module residency skyline — a
// handful of translucent quads, depth-tested against the terrain (so a
// canopy behind the camera's near geometry does not falsely win) but not
// depth-WRITTEN (so two overlapping canopies both remain visible rather than
// the nearer one occluding the farther, matching every other translucent
// surface's own depth-mask discipline on this plane).
void Scene::draw_canopies(const float mvp[16]) {
    if (!prog_canopy_ || canopy_draws_.empty())
        return;
    glUseProgram(prog_canopy_);
    glUniformMatrix4fv(glGetUniformLocation(prog_canopy_, "uMVP"), 1, GL_FALSE,
                       mvp);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    const GLint uColor = glGetUniformLocation(prog_canopy_, "uColor");
    for (const CanopyDraw &d : canopy_draws_) {
        glUniform4fv(uColor, 1, d.color);
        glBindVertexArray(d.vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}

// T5 (56-fidelity-and-module-layers): the misprediction survey layer — bias
// arcs (tessellated bezier polylines) + site columns (sheath + core),
// reusing prog_traj_ (kTrajVert/kTrajFrag) exactly as-is: uStipple=0 (never
// stippled — a stipple is THIS FAMILY's stated statistical mark on the
// trajectories, and this layer already carries its own STATISTICAL label in
// the HUD, not a second stipple technique), uHasHead=0, uHasTimeCut=0 (this
// layer is not on the trace-time axis at all).
void Scene::draw_mispred(const float mvp[16]) {
    if (!prog_traj_ || (mispred_arcs_.empty() && mispred_columns_.empty()))
        return;
    glUseProgram(prog_traj_);
    glUniformMatrix4fv(glGetUniformLocation(prog_traj_, "uMVP"), 1, GL_FALSE,
                       mvp);
    const GLint uColor = glGetUniformLocation(prog_traj_, "uColor");
    const GLint uStipple = glGetUniformLocation(prog_traj_, "uStipple");
    const GLint uHasHead = glGetUniformLocation(prog_traj_, "uHasHead");
    const GLint uHasTimeCut = glGetUniformLocation(prog_traj_, "uHasTimeCut");
    glUniform1i(uStipple, 0);
    glUniform1i(uHasHead, 0);
    glUniform1i(uHasTimeCut, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    for (const MispredArcDraw &d : mispred_arcs_) {
        glUniform4fv(uColor, 1, d.color);
        glLineWidth(d.line_width);
        glBindVertexArray(d.vao);
        glDrawArrays(GL_LINE_STRIP, 0, d.count);
    }
    for (const MispredColumnDraw &d : mispred_columns_) {
        glLineWidth(3.0f);
        glUniform4fv(uColor, 1, d.sheath_color);
        glBindVertexArray(d.vao_sheath);
        glDrawArrays(GL_LINES, 0, 2);
        glLineWidth(1.5f);
        glUniform4fv(uColor, 1, d.core_color);
        glBindVertexArray(d.vao_core);
        glDrawArrays(GL_LINES, 0, 2);
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
}

bool Scene::find_vertex(bool exact, uint64_t target, float out_pos[3],
                        int *out_line) const {
    for (size_t li = 0; li < traj_lines_.size(); ++li) {
        const Line &l = traj_lines_[li];
        if (l.statistical)
            continue; // T6: only an exact trajectory can host a vehicle/front
        // pt_t is ascending (TrajPoint.t is a monotonic per-tid counter,
        // space/types.h) — a linear scan is fine at Phase-A's cell/vertex
        // counts and keeps this branch trivially readable; binary_search
        // would need a paired index lookup for no real win here.
        if (exact) {
            for (size_t i = 0; i < l.pt_t.size(); ++i) {
                if (l.pt_t[i] != target)
                    continue;
                if (!l.pt_placed[i])
                    return false; // T6: an unplaced target step draws NOTHING
                out_pos[0] = l.pt_pos[i * 3 + 0];
                out_pos[1] = l.pt_pos[i * 3 + 1];
                out_pos[2] = l.pt_pos[i * 3 + 2];
                if (out_line)
                    *out_line = static_cast<int>(li);
                return true;
            }
        } else {
            // T2 (49): the last PLACED vertex with pt_t <= target — the
            // execution front. No placed vertex at or before target => no
            // glyph (never a snapped one, matching find_head's own rule).
            bool found = false;
            size_t best = 0;
            for (size_t i = 0; i < l.pt_t.size(); ++i) {
                if (l.pt_t[i] > target)
                    break;
                if (l.pt_placed[i]) {
                    found = true;
                    best = i;
                }
            }
            if (found) {
                out_pos[0] = l.pt_pos[best * 3 + 0];
                out_pos[1] = l.pt_pos[best * 3 + 1];
                out_pos[2] = l.pt_pos[best * 3 + 2];
                if (out_line)
                    *out_line = static_cast<int>(li);
                return true;
            }
        }
    }
    return false;
}

void Scene::render(const Camera &cam, int fbw, int fbh,
                   const SceneLayers &layers) {
    if (!ready_ || n_ == 0)
        return;
    float mvp[16];
    cam.mvp(mvp, fbh > 0 ? static_cast<float>(fbw) / fbh : 1.0f);

    // T1/T5 (55): render the raw geometry into Scene's OWN target (so a depth
    // TEXTURE is always available for EDL, and so the target can be
    // multisampled) rather than directly into whatever the caller bound.
    // compose_ready_ == false (T5 step 4: this driver would not complete
    // EITHER target even at 0 samples) falls all the way back to the
    // pre-55 behaviour — draw straight into the caller's already-bound,
    // already-cleared framebuffer — never a black pane.
    GLint caller_fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &caller_fbo);
    ensure_compose_targets(fbw, fbh);
    const bool compose = compose_ready_;
    if (compose) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_draw_);
        glViewport(0, 0, fbw, fbh);
        // The same clear the callers (gl_scene_host.cpp / test_scene_fbo.cpp)
        // used to apply to the framebuffer THEY bound — now applied here,
        // since render() no longer draws into that framebuffer directly.
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    glViewport(0, 0, fbw, fbh);

    // T3: the sky, first, before anything depth-tested.
    if (layers.weather)
        draw_sky();

    glEnable(GL_DEPTH_TEST);

    if (layers.terrain)
        draw_terrain_common(prog_terrain_, mvp, layers.zoning,
                            layers.contours ? kContourLevels : 0.0f,
                            layers.confidence, layers.opcode);

    // T4 (44, depth-write fixed by 55's dithering): the ghost-fog terrain,
    // after the exact terrain. kStatFrag now composites by DISCARD (T4, 55)
    // rather than alpha blend, so a surviving fragment is fully opaque and
    // depth-tests/writes exactly like ordinary geometry — no GL_BLEND, no
    // depth-mask trick needed: whichever surface (exact or ghost) is nearer
    // the camera at a given pixel legitimately wins the z-test, which is
    // what fixes the pre-55 bug (this pass used to leave depth-write ON for
    // the BLENDED trajectory pass below while this pass turned it OFF only
    // for itself — an inconsistency that made draw order matter for two
    // translucent surfaces; there is no blended pass left to be inconsistent
    // with).
    if (layers.ghost_fog && has_stat_terrain_)
        draw_stat_terrain(mvp);

    // T3 (56): the module-canopy skyline, above both terrains — a whole-
    // module overview riding over the per-cell surfaces it summarises.
    if (layers.canopy)
        draw_canopies(mvp);

    // T5 (56): the misprediction survey layer — statistical, never merged
    // into the exact terrain/trajectories below.
    if (layers.mispred)
        draw_mispred(mvp);

    // 58 (memory data-cell family): the five data-half layers, in ONE call —
    // their upload + draw code lives in scene3d/data_layers_gl.cpp so this
    // file keeps a single call site. Drawn after the terrains and before the
    // trajectories: they stand ON the data half of the plane and the
    // worldlines pass through them.
    draw_data_layers(mvp, layers);

    // T6: locate the followed citizen's head, once, before the trajectory
    // draw loop below (both the per-line tail uniform and the head glyph use
    // the same lookup).
    float head_pos[3] = {0, 0, 0};
    int head_line = -1;
    const bool have_head = layers.vehicle && find_head(head_pos, &head_line);

    // T2 (49): locate the execution front — the exact trajectory's own
    // boundary at slice_step, a DIFFERENT clock from the vehicle's
    // follow_step (see find_vertex's doc comment). Gated on `exact` (it marks
    // a point on that layer), never on `vehicle`. Its own colour (never a
    // per-tid one), so which line it fell on is not needed.
    float front_pos[3] = {0, 0, 0};
    const bool have_front = layers.exact && find_front(front_pos, nullptr);

    // Trajectories over the terrain.
    glUseProgram(prog_traj_);
    glUniformMatrix4fv(glGetUniformLocation(prog_traj_, "uMVP"), 1, GL_FALSE,
                       mvp);
    glUniform1f(glGetUniformLocation(prog_traj_, "uPointSize"), 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GLint uColor = glGetUniformLocation(prog_traj_, "uColor");
    GLint uStipple = glGetUniformLocation(prog_traj_, "uStipple");
    GLint uHasHead = glGetUniformLocation(prog_traj_, "uHasHead");
    GLint uHeadY = glGetUniformLocation(prog_traj_, "uHeadY");
    GLint uTailHalf = glGetUniformLocation(prog_traj_, "uTailHalf");
    GLint uTimeCutY = glGetUniformLocation(prog_traj_, "uTimeCutY");
    GLint uHasTimeCut = glGetUniformLocation(prog_traj_, "uHasTimeCut");
    const float head_y = static_cast<float>(follow_step) * traj_scale_;
    const float tail_half = 3.0f * traj_scale_; // ~3 steps either side
    // T1 (49): clip the path to the terrain playhead — no cut (byte-identical
    // to the pre-49 render) once slice_step reaches the end of the recording.
    const bool has_time_cut = slice_step < nsteps;
    const float time_cut_y = static_cast<float>(slice_step) * traj_scale_;
    glUniform1f(uTimeCutY, time_cut_y);
    glUniform1i(uHasTimeCut, has_time_cut ? 1 : 0);
    glLineWidth(2.0f);
    for (size_t li = 0; li < traj_lines_.size(); ++li) {
        const Line &l = traj_lines_[li];
        if (l.statistical && !layers.statistical)
            continue;
        if (!l.statistical && !layers.exact)
            continue;
        // 51 T1: GHOST, NEVER HIDE. A non-subject worldline keeps its hue and
        // its geometry and loses only alpha, so "another thread ran here" is
        // still on screen — hiding it would claim the thread did not run.
        // focus_line_alpha returns the literal 1.0f whenever no tid is
        // focused, so an unfocused scene's uniforms are byte-identical to the
        // pre-51 render. The one exception is T4's distance budget, which may
        // return 0 (drop) — and announces the dropped class by name.
        const float fa = focus_line_alpha(focus, l.tid, l.statistical);
        if (fa <= 0.0f)
            continue;
        const float lc[4] = {l.color[0], l.color[1], l.color[2],
                             l.color[3] * fa};
        glUniform4fv(uColor, 1, lc);
        glUniform1i(uStipple, l.statistical ? 1 : 0);
        const bool is_head_line =
            have_head && static_cast<int>(li) == head_line;
        glUniform1i(uHasHead, is_head_line ? 1 : 0);
        if (is_head_line) {
            glUniform1f(uHeadY, head_y);
            glUniform1f(uTailHalf, tail_half);
        }
        glBindVertexArray(l.vao);
        glDrawArrays(GL_LINE_STRIP, 0, l.count);
    }
    glUniform1i(uHasHead, 0); // T6: never leak the tail effect onto spurs/arcs
    if (layers.access_marks && access_spurs_.count) {
        glUniform4fv(uColor, 1, access_spurs_.color);
        glUniform1i(uStipple, 0);
        glLineWidth(1.0f);
        glBindVertexArray(access_spurs_.vao);
        glDrawArrays(GL_LINES, 0, access_spurs_.count);
    }
    // Convergence arcs last, so the hint rides over the paths it links. Solid (never
    // stippled — a stipple is the statistical mark) and thick, so it pops.
    // T1 (49) step 5: EXCLUDED from the time cut — an arc carries two times
    // (t_a, t_b) and dimming it against slice_step alone would misstate which
    // side is in the past, so it renders unclipped either side of the
    // playhead.
    glUniform1i(uHasTimeCut, 0);
    if (layers.convergence && conv_arcs_.count) {
        glUniform4fv(uColor, 1, conv_arcs_.color);
        glUniform1i(uStipple, 0);
        glLineWidth(3.0f);
        glBindVertexArray(conv_arcs_.vao);
        glDrawArrays(GL_LINES, 0, conv_arcs_.count);
    }
    glBindVertexArray(0);

    // T6: the head glyph — a single bright point at head_pos, tid-coloured
    // (the followed line's own colour), reusing prog_traj_'s existing
    // gl_PointSize support (kTrajVert already sets it). No new pick pass: the
    // head vertex is already among the existing pickable PC points
    // (vbo_pts_pos_/vbo_pts_id_), so a click there resolves to the SAME id
    // the underlying PC vertex carries (T6's "no new id space" rule).
    if (have_head) {
        if (!vao_head_) {
            glGenVertexArrays(1, &vao_head_);
            glGenBuffers(1, &vbo_head_);
        }
        glBindVertexArray(vao_head_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_head_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(head_pos), head_pos,
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glUniform1i(uStipple, 0);
        glUniform1i(uHasHead, 0);
        const float *hc =
            (head_line >= 0) ? traj_lines_[static_cast<size_t>(head_line)].color
                             : nullptr;
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        glUniform4fv(uColor, 1, hc ? hc : white);
        glUniform1f(glGetUniformLocation(prog_traj_, "uPointSize"), 10.0f);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDrawArrays(GL_POINTS, 0, 1);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glBindVertexArray(0);
    }

    // T2 (49): the execution-front glyph — the path's leading edge AT the
    // playhead, a visible point rather than an inference from where the dim
    // starts. A fixed accent colour and a different size from the vehicle
    // (never the tid palette, never white) so the two marks read as distinct
    // even when both are on screen at once.
    if (have_front) {
        if (!vao_front_) {
            glGenVertexArrays(1, &vao_front_);
            glGenBuffers(1, &vbo_front_);
        }
        glBindVertexArray(vao_front_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_front_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(front_pos), front_pos,
                     GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(kAttrPos);
        glVertexAttribPointer(kAttrPos, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glUniform1i(uStipple, 0);
        glUniform1i(uHasHead, 0);
        const float front_color[4] = {1.0f, 0.65f, 0.15f, 1.0f}; // beacon amber
        glUniform4fv(uColor, 1, front_color);
        glUniform1f(glGetUniformLocation(prog_traj_, "uPointSize"), 7.0f);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glDrawArrays(GL_POINTS, 0, 1);
        glDisable(GL_PROGRAM_POINT_SIZE);
        glBindVertexArray(0);
    }

    glDisable(GL_BLEND);

    // T1/T5 (55): resolve (blit, always — a plain copy when compose_samples_
    // is 0, an MSAA resolve when it is >= 2, ONE code path either way) into
    // the single-sample colour+depth textures, then composite into the
    // framebuffer the CALLER had bound at entry — restoring render()'s
    // external contract (the caller's framebuffer holds the final image).
    if (compose) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_draw_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_resolve_);
        glBlitFramebuffer(0, 0, fbw, fbh, 0, 0, fbw, fbh,
                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT,
                          GL_NEAREST);

        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(caller_fbo));
        glViewport(0, 0, fbw, fbh);
        if (layers.edl && prog_edl_) {
            draw_edl_pass(cam, fbw, fbh);
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_resolve_);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,
                              static_cast<GLuint>(caller_fbo));
            glBlitFramebuffer(0, 0, fbw, fbh, 0, 0, fbw, fbh,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(caller_fbo));
        }
    }
}

int Scene::render_pick_into_fbo(const Camera &cam, int fbw, int fbh) {
    // T1 (59) step 2: the R32UI target + its readback is the second of the
    // three genuinely shared pieces, so it is now scene3d::GlPickTarget
    // (glcommon.h) and a standalone scene picks through the same code. What
    // stays HERE is what is plane-specific: which geometry is drawn into it.
    const int prev_fbo = pick_target_.bind_and_clear(fbw, fbh);

    float mvp[16];
    cam.mvp(mvp, static_cast<float>(fbw) / fbh);

    // Terrain ids.
    glUseProgram(prog_pick_terrain_);
    glUniform1ui(glGetUniformLocation(prog_pick_terrain_, "uIdBase"), 1u);
    draw_terrain_common(prog_pick_terrain_, mvp);

    // Trajectory-vertex ids (points, drawn a touch larger so a near-vertex click
    // resolves). Depth test lets a vertex in front of the terrain win.
    if (pts_count_) {
        glUseProgram(prog_pick_pt_);
        glUniformMatrix4fv(glGetUniformLocation(prog_pick_pt_, "uMVP"), 1,
                           GL_FALSE, mvp);
        glUniform1f(glGetUniformLocation(prog_pick_pt_, "uPointSize"), 5.0f);
        glEnable(GL_PROGRAM_POINT_SIZE);
        glBindVertexArray(vao_pts_);
        glDrawArrays(GL_POINTS, 0, pts_count_);
        glBindVertexArray(0);
        glDisable(GL_PROGRAM_POINT_SIZE);
    }

    // T3 (47-scene-inspect-and-pickable-overlays): the two overlay-line pick
    // bands, drawn AFTER the points with prog_pick_pt_'s own "pos"+"vid"
    // layout (its frag shader just writes the incoming id — kPickPointFrag —
    // so it needs no changes to serve GL_LINES instead of GL_POINTS). A
    // widened line width makes a thin arc/spur actually clickable, mirroring
    // why the pick points are drawn larger than they render.
    if (vao_conv_pick_ && nconv_ > 0) {
        glUseProgram(prog_pick_pt_);
        glUniformMatrix4fv(glGetUniformLocation(prog_pick_pt_, "uMVP"), 1,
                           GL_FALSE, mvp);
        glLineWidth(6.0f);
        glBindVertexArray(vao_conv_pick_);
        glDrawArrays(GL_LINES, 0, conv_arcs_.count);
        glBindVertexArray(0);
    }
    if (vao_spur_pick_ && nspur_ > 0) {
        glUseProgram(prog_pick_pt_);
        glUniformMatrix4fv(glGetUniformLocation(prog_pick_pt_, "uMVP"), 1,
                           GL_FALSE, mvp);
        glLineWidth(5.0f);
        glBindVertexArray(vao_spur_pick_);
        glDrawArrays(GL_LINES, 0, access_spurs_.count);
        glBindVertexArray(0);
    }
    glLineWidth(1.0f); // restore: the colour pass sets its own width per layer
    return prev_fbo;
}

uint32_t Scene::pick(const Camera &cam, int fbw, int fbh, int x, int y) {
    if (!ready_ || n_ == 0 || fbw <= 0 || fbh <= 0)
        return 0;
    int prev_fbo = render_pick_into_fbo(cam, fbw, fbh);
    // Read the id under (x, y). glReadPixels is bottom-left origin; the caller
    // gives y from the top (GlPickTarget::read_pixel does the flip).
    const uint32_t id = pick_target_.read_pixel(x, y, fbh);
    GlPickTarget::restore(prev_fbo);
    return id;
}

void Scene::render_pick_buffer(const Camera &cam, int fbw, int fbh,
                               std::vector<uint32_t> &out) {
    out.assign(static_cast<size_t>(fbw) * fbh, 0u);
    if (!ready_ || n_ == 0 || fbw <= 0 || fbh <= 0)
        return;
    int prev_fbo = render_pick_into_fbo(cam, fbw, fbh);
    pick_target_.read_all(fbw, fbh, out);
    GlPickTarget::restore(prev_fbo);
}

void Scene::shutdown() {
    free_traj();
    free_conv();
    free_canopies();
    free_mispred();
    free_data_layers(); // 58: the data-cell family (scene3d/data_layers_gl.cpp)
    if (vbo_cell_)
        glDeleteBuffers(1, &vbo_cell_);
    if (ibo_grid_)
        glDeleteBuffers(1, &ibo_grid_);
    if (vao_grid_)
        glDeleteVertexArrays(1, &vao_grid_);
    if (tex_height_)
        glDeleteTextures(1, &tex_height_);
    if (tex_flags_)
        glDeleteTextures(1, &tex_flags_);
    if (tex_kind_)
        glDeleteTextures(1, &tex_kind_);
    if (tex_opclass_)
        glDeleteTextures(1, &tex_opclass_);
    if (tex_focus_) // 51 T2
        glDeleteTextures(1, &tex_focus_);
    if (tex_height_stat_)
        glDeleteTextures(1, &tex_height_stat_);
    if (tex_flags_stat_)
        glDeleteTextures(1, &tex_flags_stat_);
    if (vbo_sky_)
        glDeleteBuffers(1, &vbo_sky_);
    if (vao_sky_)
        glDeleteVertexArrays(1, &vao_sky_);
    if (vbo_head_)
        glDeleteBuffers(1, &vbo_head_);
    if (vao_head_)
        glDeleteVertexArrays(1, &vao_head_);
    if (vbo_front_)
        glDeleteBuffers(1, &vbo_front_);
    if (vao_front_)
        glDeleteVertexArrays(1, &vao_front_);
    pick_target_.free_gl(); // T1 (59): the shared R32UI target
    free_compose_targets(); // T1/T5 (55)
    for (unsigned p : {prog_terrain_, prog_traj_, prog_pick_terrain_,
                       prog_pick_pt_, prog_stat_, prog_sky_, prog_edl_,
                       prog_canopy_})
        if (p)
            glDeleteProgram(p);
    vbo_cell_ = ibo_grid_ = vao_grid_ = tex_height_ = tex_flags_ = 0;
    tex_kind_ = tex_height_stat_ = tex_flags_stat_ = tex_opclass_ = 0;
    tex_focus_ = 0; // 51 T2
    vbo_sky_ = vao_sky_ = vbo_head_ = vao_head_ = 0;
    vbo_front_ = vao_front_ = 0;
    has_stat_terrain_ = false;
    has_focus_mask_ = false; // 51 T2
    // 59 T1 folded tex_pick_/rbo_pick_depth_/fbo_pick_ into pick_target_
    // (scene3d/glcommon.h), freed and zeroed by pick_target_.free_gl() above —
    // so there are no raw pick handles left here to reset.
    prog_terrain_ = prog_traj_ = prog_pick_terrain_ = prog_pick_pt_ = 0;
    prog_stat_ = prog_sky_ = prog_edl_ = prog_canopy_ = 0;
    ready_ = false;
    n_ = 0;
}

} // namespace asmdesk::scene3d
