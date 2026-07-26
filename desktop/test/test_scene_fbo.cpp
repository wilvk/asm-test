// test_scene_fbo.cpp — the gated GL smoke of the 3D scene (docs/internal/gui/
// 10-spacetime-3d-overview.md T4). Two halves:
//
//   1. PURE (always runs, even with no GL): the colour-ID pick scheme decode and
//      its resolution to a 04 deep-link (pick.h). This is the "a pick reaches the
//      router" contract, asserted with no context at all.
//   2. GL SMOKE (self-skips with a printed reason where no GL is reachable):
//      build a scene from a hand-authored abs+codeimage recording, render the
//      terrain + trajectory to offscreen framebuffers, glReadPixels and assert
//      (a) a hot cell is brighter than a cold cell, (b) a TORN fixture paints red
//      in its region, (c) the pick FBO returns the right id for a known cell.
//
// The offscreen context is created with EGL surfaceless + software Mesa
// (LIBGL_ALWAYS_SOFTWARE=1 in the lane), so the smoke runs in a container with NO
// X server — more headless-robust than a hidden GLFW window (which still needs a
// display). A machine with no EGL/GL device prints why and skips (exit 0); the
// pure half has already run, and the pure camera math runs in test_camera.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "scene3d/camera.h"
#include "scene3d/pick.h"
#include "scene3d/scene.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

using namespace asmdesk;
using namespace asmdesk::scene3d;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static Recording load(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        std::fprintf(stderr, "FAIL fixture load: %s\n", err.c_str());
        failures++;
        return Recording{};
    }
    return *rec;
}

static const char *kHdrExact =
    "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":\"test\","
    "\"version\":\"0\"},\"provenance\":{\"backend\":\"emu-l0\",\"exact\":true,"
    "\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";

// A non-torn abs recording: a code region at 0x400000, a HOT offset (5 hits) and
// a COLD offset (1 hit), closed with a clean `end` so it is not TORN.
static std::string ndjson_hotcold() {
    std::string nd = kHdrExact;
    nd += "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,\"version\":0}\n";
    for (int i = 0; i < 5; i++)
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"; // hot
    nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194368}\n";     // cold
    nd += "{\"k\":\"end\",\"events\":7,\"truncated\":false}\n";
    return nd;
}

// A TORN recording: same region, one offset, closed truncated -> the cell is torn.
static std::string ndjson_torn() {
    std::string nd = kHdrExact;
    nd += "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,\"version\":0}\n";
    for (int i = 0; i < 3; i++)
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n";
    nd += "{\"k\":\"end\",\"events\":4,\"truncated\":true}\n";
    return nd;
}

static uint32_t cell_of(const space::Projection &proj, uint32_t n,
                        uint64_t addr) {
    float u = 0, v = 0;
    proj.project(addr, &u, &v);
    uint32_t x = static_cast<uint32_t>(u * n), y = static_cast<uint32_t>(v * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    return y * n + x;
}

// ---- the pure half: id decode + router resolution (no GL) -------------------
static void pure_pick_checks() {
    space::TerrainModel terr = space::build_terrain(
        space::build_projection(
            space::regions_from_codeimage(load(ndjson_hotcold()))),
        load(ndjson_hotcold()));
    space::TrajectorySet traj =
        space::build_trajectories(load(ndjson_hotcold()));
    const uint32_t n = terr.w;
    check("terrain sized", n > 0, "n == 0");

    // background id -> nothing -> no link.
    check("id 0 decodes to None", decode_pick(0, n).kind == Pick::None,
          "not None");
    check("None resolves to no link",
          !resolve_pick(terr, traj, "r", Pick{}).has_value(), "got a link");

    // a terrain cell id -> Cell -> a canvas deep-link at the code offset.
    uint32_t hot = cell_of(terr.proj, n, 4194304);
    Pick pc = decode_pick(pick_id_cell(hot), n);
    check("cell id decodes to Cell", pc.kind == Pick::Cell && pc.cell == hot,
          "wrong cell");
    auto link = resolve_pick(terr, traj, "rec.asmtrace", pc);
    check("cell resolves to a link", link.has_value(), "no link");
    if (link) {
        check("cell link opens the canvas", link->view == dt_view::canvas,
              "wrong view");
        check("cell link carries the code offset (0)",
              link->off.has_value() && *link->off == 0, "wrong offset");
        check("cell link names the recording", link->rec == "rec.asmtrace",
              "wrong rec");
    }

    // a trajectory-vertex id -> Vertex -> a slice deep-link at that step.
    if (!traj.trajectories.empty()) {
        uint32_t vid = pick_id_vertex(n, 0);
        Pick pv = decode_pick(vid, n);
        check("vertex id decodes to Vertex",
              pv.kind == Pick::Vertex && pv.vertex == 0, "wrong vertex");
        auto vl = resolve_pick(terr, traj, "rec.asmtrace", pv);
        check("vertex resolves to a slice link",
              vl.has_value() && vl->view == dt_view::slice &&
                  vl->step.has_value(),
              "no slice link");
    }
}

// ---- the GL half ------------------------------------------------------------
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

static void skip(const char *why) {
    std::printf("test_scene_fbo: SKIP GL smoke — %s (pure checks ran)\n", why);
}

// Bring up a surfaceless EGL + desktop-GL context. Returns false (and prints a
// reason) on any failure, so a host with no GL device self-skips.
static bool egl_up(EGLDisplay *out_dpy, EGLContext *out_ctx, std::string *why) {
    EGLDisplay dpy = EGL_NO_DISPLAY;
    auto getPD = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
        "eglGetPlatformDisplayEXT");
    if (getPD)
        dpy =
            getPD(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
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

// A simple RGBA8 + depth24 colour framebuffer for reading terrain colour back.
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

static float lum(const unsigned char *p) {
    return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
}

int main() {
    // ===== 1. the pure half — always runs ====================================
    pure_pick_checks();

    // ===== 2. the GL smoke — self-skips where no GL is reachable =============
    EGLDisplay dpy = EGL_NO_DISPLAY;
    EGLContext ctx = EGL_NO_CONTEXT;
    std::string why;
    if (!egl_up(&dpy, &ctx, &why)) {
        skip(why.c_str());
        return failures ? 1 : 0;
    }

    Scene scene;
    std::string serr;
    if (!scene.init_gl(&serr)) {
        skip(("scene shaders would not build: " + serr).c_str());
        eglTerminate(dpy);
        return failures ? 1 : 0;
    }

    const int W = 160, H = 160;
    ColorFbo cf;
    if (!cf.create(W, H)) {
        skip("colour framebuffer incomplete on this driver");
        eglTerminate(dpy);
        return failures ? 1 : 0;
    }

    Camera cam; // default three-quarter view frames the unit plane

    // --- (a) hot cell brighter than cold cell -------------------------------
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_hotcold()))),
            load(ndjson_hotcold()));
        space::TrajectorySet traj =
            space::build_trajectories(load(ndjson_hotcold()));
        const uint32_t n = terr.w;
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);

        glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
        glViewport(0, 0, W, H);
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        scene.render(cam, W, H, SceneLayers{});
        std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Locate the hot and cold cells on screen via the id buffer (same camera,
        // same viewport, so a pick pixel and a colour pixel line up).
        std::vector<uint32_t> ids;
        scene.render_pick_buffer(cam, W, H, ids);
        uint32_t hot = cell_of(terr.proj, n, 4194304);
        uint32_t cold = cell_of(terr.proj, n, 4194368);
        int hot_px = -1, cold_px = -1;
        for (size_t i = 0; i < ids.size(); i++) {
            if (ids[i] == pick_id_cell(hot) && hot_px < 0)
                hot_px = static_cast<int>(i);
            if (ids[i] == pick_id_cell(cold) && cold_px < 0)
                cold_px = static_cast<int>(i);
        }
        check("hot cell is on screen", hot_px >= 0, "hot cell not rendered");
        check("cold cell is on screen", cold_px >= 0, "cold cell not rendered");
        if (hot_px >= 0 && cold_px >= 0) {
            float lh = lum(&px[hot_px * 4]);
            float lc = lum(&px[cold_px * 4]);
            check("hot cell is brighter than cold cell", lh > lc + 8.0f,
                  "hot not brighter");
        }

        // --- (c) the pick FBO returns the right id for a known cell centre ---
        if (hot_px >= 0) {
            int px_x = hot_px % W, py = hot_px / W; // bottom-left origin
            int y_top = H - 1 - py;
            uint32_t got = scene.pick(cam, W, H, px_x, y_top);
            check("pick returns the hot cell id", got == pick_id_cell(hot),
                  "wrong id");
            Pick d = decode_pick(got, n);
            check("picked id decodes to the hot cell",
                  d.kind == Pick::Cell && d.cell == hot, "decode mismatch");
        }
    }

    // --- (b) a TORN fixture writes red in its region ------------------------
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_torn()))),
            load(ndjson_torn()));
        space::TrajectorySet traj =
            space::build_trajectories(load(ndjson_torn()));
        const uint32_t n = terr.w;
        check("torn model is torn", terr.torn, "torn flag not set");
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);

        glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
        glViewport(0, 0, W, H);
        glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        scene.render(cam, W, H, SceneLayers{});
        std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        std::vector<uint32_t> ids;
        scene.render_pick_buffer(cam, W, H, ids);
        uint32_t torn = cell_of(terr.proj, n, 4194304);
        int tpx = -1;
        for (size_t i = 0; i < ids.size(); i++)
            if (ids[i] == pick_id_cell(torn)) {
                tpx = static_cast<int>(i);
                break;
            }
        check("torn cell is on screen", tpx >= 0, "torn cell not rendered");
        if (tpx >= 0) {
            const unsigned char *p = &px[tpx * 4];
            check("torn cell is red (r dominates g and b)",
                  p[0] > p[1] + 40 && p[0] > p[2] + 40, "not a red gash");
        }
    }

    scene.shutdown();
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);

    if (failures) {
        std::fprintf(stderr, "%d scene-FBO check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_scene_fbo: all checks passed (GL smoke ran)\n");
    return 0;
}
