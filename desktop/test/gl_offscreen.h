// gl_offscreen.h — a surfaceless EGL context, an RGBA8 FBO, and one call that
// turns a Recording into a rendered Plane-scene frame. The EGL/FBO/readback
// bodies are LIFTED UNCHANGED from test_scene_fbo.cpp so that the FBO smoke and
// the motif gate share one context path instead of two that can silently
// diverge.
//
// Header-only but NOT dependency-free: including this pulls in EGL, GL and the
// whole scene3d/space object closure, so a binary using it must link
// DESKTOP_GL_TEST_OBJS and belongs in DESKTOP_GL_TESTS, never DESKTOP_TESTS.
// (camera.h and trajscale.h are the engine-free citizens; this is not one.)
//
// Every scene3d type below is written `scene3d::`-qualified. They live in
// asmdesk::scene3d, not asmdesk, and only Recording resolves unqualified from
// inside asmdesk::testing. test_scene_fbo.cpp names them bare ONLY because it
// carries a file-scope `using namespace asmdesk::scene3d;`; a header must not
// borrow that, and must not "fix" it by adding a using-directive of its own.
#ifndef ASMDESK_TEST_GL_OFFSCREEN_H
#define ASMDESK_TEST_GL_OFFSCREEN_H

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include "doc/recording.h"
#include "scene3d/camera.h"
#include "scene3d/scene.h"
#include "space/converge.h"
#include "space/opcode_terrain.h" // build_opcode_terrain, opcode_guest_from_arch
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

namespace asmdesk::testing {

// A read-back frame. Row 0 is the BOTTOM row — glReadPixels' own order, kept
// rather than flipped because every consumer compares frames to frames.
struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px; // w*h*4, RGBA8
};

// The clear colour capture_image() uses, as the bytes glReadPixels returns.
// The same value test_scene_fbo.cpp already clears to, so a frame means the
// same thing whichever test produced it — and "blank" has one definition.
inline constexpr uint8_t kClearRGB[3] = {5, 5, 8};

// Fraction of pixels carrying anything other than the clear colour. A
// FRACTION, not a count, so a caller's threshold does not silently depend on
// the framebuffer size. The 12 is a driver-rounding slack, not a perceptual
// threshold — llvmpipe need not land exactly on kClearRGB.
inline float image_ink_fraction(const Image &a) {
    if (a.px.empty())
        return 0.0f;
    const size_t n = size_t(a.w) * size_t(a.h);
    size_t ink = 0;
    for (size_t i = 0; i < n; ++i) {
        const int dr = int(a.px[i * 4 + 0]) - int(kClearRGB[0]);
        const int dg = int(a.px[i * 4 + 1]) - int(kClearRGB[1]);
        const int db = int(a.px[i * 4 + 2]) - int(kClearRGB[2]);
        if (std::abs(dr) + std::abs(dg) + std::abs(db) > 12)
            ink++;
    }
    return float(ink) / float(n);
}

inline bool image_blank(const Image &a) {
    return image_ink_fraction(a) < 0.001f;
}

// Is a committed recording present? A missing one is a broken checkout, never
// a reason to skip; this only reports, and the caller says what it means.
inline bool scene_exists(const char *dir, const char *name) {
    std::ifstream f(std::string(dir) + "/" + name);
    return f.good();
}

// --- the EGL context, brought up at most once per process --------------------
// egl_up's body is byte-for-byte test_scene_fbo.cpp's. It HANDS BACK the
// display and context rather than owning them, because that caller keeps them
// in locals; egl_up_once adds the two statics around it for callers that need
// them to outlive the call. Do not "simplify" by dropping the out-params.
inline bool egl_up(EGLDisplay *out_dpy, EGLContext *out_ctx, std::string *why) {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
        "eglGetPlatformDisplayEXT");
    if (getPD)
        dpy = getPD(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) {
        *why = "no EGL display";
        return false;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) {
        *why = "eglInitialize failed";
        return false;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        *why = "no desktop-GL EGL API";
        return false;
    }
    const EGLint cfg_attr[] = {EGL_SURFACE_TYPE,
                               EGL_PBUFFER_BIT,
                               EGL_RENDERABLE_TYPE,
                               EGL_OPENGL_BIT,
                               EGL_RED_SIZE,
                               8,
                               EGL_GREEN_SIZE,
                               8,
                               EGL_BLUE_SIZE,
                               8,
                               EGL_ALPHA_SIZE,
                               8,
                               EGL_DEPTH_SIZE,
                               24,
                               EGL_NONE};
    EGLConfig cfg;
    EGLint num = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &num) || num < 1) {
        *why = "no usable EGL config";
        return false;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                               EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) {
        *why = "eglCreateContext failed";
        return false;
    }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        *why = "surfaceless make-current failed";
        return false;
    }
    *out_dpy = dpy;
    *out_ctx = ctx;
    return true;
}

inline bool egl_up_once(std::string *why) {
    static EGLDisplay dpy = EGL_NO_DISPLAY;
    static EGLContext ctx = EGL_NO_CONTEXT;
    return egl_up(&dpy, &ctx, why);
}

// Returns false with a reason where no GL device is reachable. Callers decide
// what that MEANS: test_scene_fbo self-skips (its pure half has already run);
// the motif gate FAILS, because its entire content is the picture.
inline bool gl_context_available(std::string *why) {
    static bool tried = false, ok = false;
    static std::string reason;
    if (!tried) {
        tried = true;
        ok = egl_up_once(&reason);
    }
    if (!ok && why)
        *why = reason;
    return ok;
}

// --- the framebuffer and the readback ---------------------------------------
// A simple RGBA8 + depth24 colour framebuffer for reading scene colour back.
struct ColorFbo {
    GLuint fbo = 0, tex = 0, rbo = 0;
    int w = 0, h = 0;
    bool create(int width, int height) {
        w = width;
        h = height;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rbo);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
               GL_FRAMEBUFFER_COMPLETE;
    }
};

inline Image capture_image(scene3d::Scene &scene, const scene3d::Camera &cam,
                           const ColorFbo &cf,
                           const scene3d::SceneLayers &layers) {
    glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
    glViewport(0, 0, cf.w, cf.h);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f); // == kClearRGB
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    scene.render(cam, cf.w, cf.h, layers);
    Image img;
    img.w = cf.w;
    img.h = cf.h;
    img.px.resize(size_t(cf.w) * size_t(cf.h) * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, cf.w, cf.h, GL_RGBA, GL_UNSIGNED_BYTE, img.px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return img;
}

// --- one recording -> one rendered frame ------------------------------------
// The layout is set on the Projection BEFORE build_terrain, and this is the
// whole reason this function exists rather than a caller doing it inline: the
// terrain's cells are keyed on the projection's mapping, so switching layout
// afterwards would paint an atlas floor with Hilbert heights and the picture
// would be a composite of two coordinate systems.
inline Image render_plane_scene(scene3d::Scene &scene, const ColorFbo &cf,
                                const scene3d::Camera &cam,
                                const Recording &rec,
                                const scene3d::SceneLayers &layers,
                                space::Projection::Layout layout) {
    space::Projection proj =
        space::build_projection(space::regions_from_codeimage(rec));
    proj.layout = layout;
    space::rebuild_layout(proj);
    space::TerrainModel terr = space::build_terrain(std::move(proj), rec);
    const space::TrajectorySet traj = space::build_trajectories(rec, terr.proj);
    scene.nsteps = static_cast<uint32_t>(terr.nsteps);
    scene.set_terrain(terr.full());
    scene.set_trajectories(traj, terr.proj);
    scene.set_convergences(space::ConvergenceSet{}, terr.proj);
    // THE OPCODE CHANNEL'S DATA. SceneLayers::opcode only selects a tint — the
    // byte map it samples is the R8UI texture set_opcode_terrain uploads, and
    // WITHOUT this call `opcode = true` re-tints nothing, so a distinctness
    // gate over it would fail for a reason having nothing to do with the
    // encoding. Slice-invariant, so once per weave is right.
    scene.set_opcode_terrain(
        space::build_opcode_terrain(terr, rec,
                                    space::opcode_guest_from_arch(rec.arch)),
        terr.w, terr.h);
    return capture_image(scene, cam, cf, layers);
}

} // namespace asmdesk::testing
#endif // ASMDESK_TEST_GL_OFFSCREEN_H
