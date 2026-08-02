// test_scene_fbo.cpp — the gated GL smoke of the 3D scene (docs/internal/gui/
// 10-spacetime-3d-overview.md T4, extended by T7). Two halves:
//
//   1. PURE (always runs, even with no GL): the colour-ID pick scheme decode and
//      its resolution to a 04 deep-link (pick.h), plus the MODEL facts of the
//      three committed golden scenes (T7) — basis, heat, TORN, STAT isolation,
//      and the closed rich-`mem` gate. Asserted with no context at all.
//   2. GL SMOKE (self-skips with a printed reason where no GL is reachable):
//      build scenes, render terrain + trajectories to offscreen framebuffers,
//      glReadPixels and assert (a) a hot cell is brighter than a cold cell,
//      (b) a TORN fixture paints red in its region, (c) the pick FBO returns the
//      right id for a known cell, (d) a convergence arc draws and toggles, and
//      — from the T7 GOLDEN SCENES — (e) the abs scene's coarse terrain AND its
//      exact trajectory both put pixels on screen, (f) the truncated scene's
//      cells paint the torn gash, (g) a survey-only scene renders its
//      STATISTICAL layer and *nothing* on the exact layer (isolation, at the
//      pixel level), (h) the synthetic rich-`mem` scene is INERT — it renders
//      pixel-identically to its coarse twin, the rich rung drawing nothing.
//
// The three golden scenes (T7): `scene-abs-loop` and `scene-abs-loop-truncated`
// are GENERATED into tests/golden-asmtrace/ by `make asmtrace-golden`
// (tools/asmtrace_record.c); the rich `mem` one is hand-authored under
// tests/golden-asmtrace/scenes/ because `mem` has no producer; the statistical
// one REUSES the committed desktop/test/fixtures/obs-survey-ibs.asmtrace rather
// than adding a second survey fixture that would say the same thing.
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
#include "space/converge.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif
#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

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

// Load a committed recording from disk (the T7 golden scenes + the reused
// survey fixture). A missing file is a FAILURE, never a skip: these are
// committed (D6), so absence is a broken checkout.
static Recording load_path(const char *dir, const char *name) {
    std::string path = std::string(dir) + "/" + name;
    std::string err;
    auto rec = load_recording_file(path, err);
    if (!rec) {
        std::fprintf(stderr, "FAIL load %s: %s\n", path.c_str(), err.c_str());
        failures++;
        return Recording{};
    }
    return *rec;
}

// The three models the scene is built from, for one recording. `extra` carries
// the regions a maps snapshot (07) would add where a fixture has no codeimage —
// a survey names its sampling window in `provenance`, not as a code region.
struct SceneModel {
    Recording rec;
    space::TerrainModel terr;
    space::TrajectorySet traj;
};
static SceneModel build_scene(Recording rec,
                              std::vector<space::Region> extra = {}) {
    SceneModel m;
    std::vector<space::Region> regs = space::regions_from_codeimage(rec);
    for (const space::Region &r : extra)
        regs.push_back(r);
    m.terr = space::build_terrain(space::build_projection(regs), rec);
    // 36 T2: production-faithful — the terrain's Projection anchors a rel/df PC
    // path onto the plane (a no-op for an abs recording), exactly as the shell
    // does, so an anchored path's vertices project and the scene draws its tube.
    m.traj = space::build_trajectories(rec, m.terr.proj);
    m.rec = std::move(rec);
    return m;
}

// The scene_hot_loop window the two generated scenes record (the producer maps
// it at REC_CODE_BASE, tools/asmtrace_record.c) — the addresses this file
// predicts heat at, read straight off the listing in that generator.
static const uint64_t kLoopBase = 0x00100000ull; // mov rax, rdi  (executed 1x)
static const uint64_t kLoopBody = 0x00100006ull; // add rcx, rax  (executed 3x)

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

// A two-thread abs recording (10-T5): tid 1 and tid 2 both touch 0x401200, so the
// completed stream carries exactly one cross-thread convergence — the arc smoke.
static std::string ndjson_threads() {
    std::string nd = kHdrExact;
    nd += "{\"k\":\"codeimage\",\"base\":4198400,\"len\":4096,\"version\":0}\n";
    nd +=
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4198400,\"tid\":1}\n"; // 0x401000
    nd +=
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4198416,\"tid\":2}\n"; // 0x401010
    nd +=
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4198464,\"tid\":1}\n"; // 0x401040
    nd +=
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4198912,\"tid\":2}\n"; // 0x401200 shared
    nd +=
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4198912,\"tid\":1}\n"; // 0x401200 shared
    nd +=
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4198528,\"tid\":2}\n"; // 0x401080
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

    // a trajectory-vertex id -> Vertex -> a timeline deep-link at that step (an
    // exact PC vertex reads on the Loom / operand timeline; T6).
    if (!traj.trajectories.empty()) {
        uint32_t vid = pick_id_vertex(n, 0);
        Pick pv = decode_pick(vid, n);
        check("vertex id decodes to Vertex",
              pv.kind == Pick::Vertex && pv.vertex == 0, "wrong vertex");
        auto vl = resolve_pick(terr, traj, "rec.asmtrace", pv);
        check("vertex resolves to a timeline link",
              vl.has_value() && vl->view == dt_view::timeline &&
                  vl->step.has_value(),
              "no timeline link");
    }
}

// The sampling window an ibs-op survey names in its provenance — the region a
// maps snapshot supplies for a fixture that carries no codeimage (the same
// window test_drillin uses, from obs-survey-ibs's provenance.window).
static space::Region survey_window() {
    space::Region r;
    r.base = 4198400;
    r.len = 4096;
    r.kind = space::Region::Data;
    return r;
}

// ---- the three T7 golden scenes: their MODEL facts (no GL) ------------------
// Asserted in the pure half so a host with no GL still proves the scenes build
// what the doc says they build; the GL half below then proves they RENDER it.
static void pure_scene_checks() {
    // (A) the coarse rung's happy path: abs basis, real heat, one exact tube.
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_GOLDEN_DIR, "scene-abs-loop.asmtrace"));
        check("golden abs scene: basis is absolute", m.terr.basis == "abs",
              m.terr.basis.c_str());
        check("golden abs scene: not torn", !m.terr.torn, "torn");
        check("golden abs scene: seven distinct code cells",
              m.terr.code.size() == 7, "wrong cell count");
        check("golden abs scene: 13 trace steps", m.terr.nsteps == 13,
              "wrong step count");
        // Heat is countable off the generator's listing: the loop body ran 3x,
        // the prologue once.
        uint32_t hot = cell_of(m.terr.proj, m.terr.w, kLoopBody);
        uint32_t cold = cell_of(m.terr.proj, m.terr.w, kLoopBase);
        uint32_t hot_heat = 0, cold_heat = 0;
        for (const auto &cc : m.terr.code) {
            if (cc.cell == hot)
                hot_heat = cc.full_heat;
            if (cc.cell == cold)
                cold_heat = cc.full_heat;
        }
        check("golden abs scene: the loop body is hot (3)", hot_heat == 3,
              "wrong hot heat");
        check("golden abs scene: the prologue is cold (1)", cold_heat == 1,
              "wrong cold heat");
        check("golden abs scene: exactly one trajectory",
              m.traj.trajectories.size() == 1, "wrong trajectory count");
        if (m.traj.trajectories.size() == 1) {
            const space::Trajectory &tr = m.traj.trajectories[0];
            check("golden abs scene: the trajectory is EXACT (no flags)",
                  tr.flags == 0, "relative or statistical");
            check("golden abs scene: 13 PC vertices", tr.points.size() == 13,
                  "wrong vertex count");
        }
        // The rich rung is absent here, and says so rather than going flat.
        check("golden abs scene: the mem gate is closed", !m.terr.mem_present,
              "mem present");
        check("golden abs scene: the coarse chip is set",
              !m.terr.mem_note.empty(), "no coarse note");
    }

    // (B) the torn twin: the SAME bytes, fewer steps held, every cell a floor.
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_GOLDEN_DIR, "scene-abs-loop-truncated.asmtrace"));
        check("golden torn scene: the recording declares truncation",
              m.rec.truncated(), "not truncated");
        check("golden torn scene: the footer counts the dropped steps",
              m.rec.dropped(), "no drops");
        check("golden torn scene: the terrain is torn", m.terr.torn,
              "torn flag not set");
        check("golden torn scene: only the held prefix is placed (6 steps)",
              m.terr.nsteps == 6, "wrong step count");
        space::Terrain full = m.terr.full();
        size_t populated = 0, torn_cells = 0;
        for (size_t i = 0; i < full.height.size(); i++) {
            if (full.height[i] <= 0.0f)
                continue;
            populated++;
            if ((full.flags[i] & space::TF_TORN) != 0u)
                torn_cells++;
        }
        check("golden torn scene: cells are populated", populated > 0,
              "an empty terrain");
        check("golden torn scene: EVERY populated cell is TORN",
              populated == torn_cells, "a populated cell escaped the tear");
        check("golden torn scene: no statistical layer leaked in",
              !m.terr.has_stat, "a STAT layer in an exact recording");
    }

    // (C) the statistical scene (reused fixture): STAT layer, empty exact one.
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_FIXTURE_DIR, "obs-survey-ibs.asmtrace"),
            {survey_window()});
        check("golden stat scene: exact:false provenance", m.rec.statistical(),
              "a survey must be exact:false");
        check("golden stat scene: a separate STAT layer exists",
              m.terr.has_stat, "no stat layer");
        check("golden stat scene: the EXACT terrain is empty (isolation)",
              m.terr.code.empty(), "an exact cell in a survey");
        size_t stat_cells = 0;
        for (uint32_t f : m.terr.stat.flags)
            if ((f & space::TF_STAT) != 0u)
                stat_cells++;
        check("golden stat scene: the residency cells are flagged STAT",
              stat_cells > 0, "no STAT cells");
        size_t exact_tubes = 0;
        for (const space::Trajectory &tr : m.traj.trajectories)
            if ((tr.flags & space::TRAJ_STATISTICAL) == 0)
                exact_tubes++;
        check("golden stat scene: NO exact trajectory tube", exact_tubes == 0,
              "an exact path leaked out of a survey");
    }

    // (D) the synthetic rich-`mem` scene: present, and INERT.
    {
        SceneModel coarse = build_scene(
            load_path(ASMTEST_GOLDEN_DIR, "scene-abs-loop.asmtrace"));
        SceneModel m = build_scene(load_path(ASMTEST_GOLDEN_DIR "/scenes",
                                             "mem-rich-synthetic.asmtrace"));
        check("synthetic mem scene: the kind IS present", m.terr.mem_present,
              "no mem events");
        check("synthetic mem scene: the trajectory saw the stream too",
              m.traj.mem_present, "trajectory mem gate closed");
        // Inert: with the coarse-rung projection (code regions only) the
        // accesses map to no region, so the rich rung adds NO data cell — the
        // gate stays closed until a producer, and a data region, land.
        check("synthetic mem scene: the rich rung adds no data cell",
              m.terr.data.empty(), "a data cell without a data region");
        // And the code half is its coarse twin's, cell for cell.
        check("synthetic mem scene: the coarse half is unchanged",
              m.terr.code.size() == coarse.terr.code.size() &&
                  m.terr.nsteps == coarse.terr.nsteps,
              "the mem lines perturbed the coarse terrain");
    }

    // (E) the LIVE dataflow scene (36 T4): absolute codeimage + region-relative
    // df_step, NO trace. The df_step rung feeds the terrain and the PC path
    // ANCHORS onto the single span (base+off) — and at least two of its vertices
    // project, so scene.cpp's line.size() >= 6 tube gate cannot silently drop the
    // trajectory (the very failure 36 repairs, and the assertion whose absence
    // hid it).
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_GOLDEN_DIR, "scene-df-loop.asmtrace"));
        check("golden df scene: no trace, so the canvas basis is empty",
              m.terr.basis.empty(), "a df-only scene has no canvas basis");
        check("golden df scene: height source is df_step",
              m.terr.height_source == "df_step", m.terr.height_source.c_str());
        check("golden df scene: the df rung placed code cells",
              !m.terr.code.empty(), "the df_step residency rung is empty");
        check("golden df scene: the PC path is rel AND anchored",
              m.traj.basis == "rel" && m.traj.anchored, "path not anchored");
        check("golden df scene: exactly one trajectory",
              m.traj.trajectories.size() == 1, "wrong trajectory count");
        size_t projected = 0;
        if (m.traj.trajectories.size() == 1)
            for (const space::TrajPoint &p : m.traj.trajectories[0].points) {
                if (p.is_access)
                    continue;
                float u = 0, v = 0;
                if (m.terr.proj.project(p.addr, &u, &v))
                    projected++;
            }
        check(
            "golden df scene: at least two vertices project (tube not dropped)",
            projected >= 2,
            ("only " + std::to_string(projected) + " projected").c_str());
    }

    // T1 (49): slice_step reaches the scene unchanged, and defaults to the
    // "unconfigured, no cut" sentinel — asserted here (no GL context needed)
    // so this survives even where the GL smoke below self-skips.
    {
        Scene s;
        check("Scene::slice_step defaults to UINT64_MAX (no cut configured)",
              s.slice_step == UINT64_MAX, "a default-constructed Scene must "
                                          "not retroactively clip a caller "
                                          "that never touches slice_step");
        s.slice_step = 42;
        check("Scene::slice_step is a plain settable field",
              s.slice_step == 42, "the value did not round-trip");
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

// Render one frame of the currently-uploaded scene into `cf` and read it back.
static std::vector<unsigned char> capture(Scene &scene, const Camera &cam,
                                          const ColorFbo &cf,
                                          const SceneLayers &layers) {
    glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
    glViewport(0, 0, cf.w, cf.h);
    glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    scene.render(cam, cf.w, cf.h, layers);
    std::vector<unsigned char> px(static_cast<size_t>(cf.w) * cf.h * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, cf.w, cf.h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return px;
}

// How many pixels two frames disagree on — the layer-toggle metric: a layer that
// draws something changes pixels when it is switched off, and one that draws
// nothing changes none.
static size_t pixels_differ(const std::vector<unsigned char> &a,
                            const std::vector<unsigned char> &b) {
    size_t n = 0;
    if (a.size() != b.size())
        return a.size() + b.size();
    for (size_t i = 0; i < a.size(); i += 4)
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2])
            n++;
    return n;
}

// Upload one scene model (terrain slice + trajectories), clearing any
// convergence arcs a previous case left behind.
static void upload(Scene &scene, const SceneModel &m, const space::Terrain &t) {
    scene.nsteps = static_cast<uint32_t>(m.terr.nsteps);
    scene.set_terrain(t);
    scene.set_trajectories(m.traj, m.terr.proj);
    scene.set_convergences(space::ConvergenceSet{}, m.terr.proj);
}

// The first screen pixel carrying `id` in the pick buffer, or -1.
static int pixel_of_id(Scene &scene, const Camera &cam, int W, int H,
                       uint32_t id) {
    std::vector<uint32_t> ids;
    scene.render_pick_buffer(cam, W, H, ids);
    for (size_t i = 0; i < ids.size(); i++)
        if (ids[i] == id)
            return static_cast<int>(i);
    return -1;
}

int main() {
    // ===== 1. the pure half — always runs ====================================
    pure_pick_checks();
    pure_scene_checks();

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

    // --- T1/T7 (44-faithful-city-phase-a): zoning toggles the HUE, never a --
    // pick id. A data cell (kind != Code) with real height (via `mem`, so the
    // height*kind blend is actually visible — kindHue mixes toward DARK at
    // height 0 regardless of kind) must paint a visibly different colour with
    // zoning on vs off; the pick pass takes no SceneLayers at all, so its id
    // is unaffected by construction — asserted here as the regression guard.
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,"
              "\"version\":0}\n";
        nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n";
        nd += "{\"k\":\"mem\",\"step\":0,\"ea\":6291456,\"size\":64,"
              "\"rw\":\"w\"}\n";
        nd += "{\"k\":\"end\",\"events\":3,\"truncated\":false}\n";
        Recording rec = load(nd);
        space::Region data_region;
        data_region.base = 0x600000;
        data_region.len = 4096;
        data_region.kind = space::Region::Data;
        std::vector<space::Region> regs = space::regions_from_codeimage(rec);
        regs.push_back(data_region);
        space::TerrainModel terr =
            space::build_terrain(space::build_projection(regs), rec);
        check("T1 GL: the rich-mem fixture built a data cell (test setup)",
              !terr.data.empty(), "no data cell built");
        space::TrajectorySet traj = space::build_trajectories(rec, terr.proj);
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_zoning(terr.kind_by_cell, terr.w, terr.h);
        scene.set_trajectories(traj, terr.proj);

        uint32_t data_cell = cell_of(terr.proj, terr.w, 0x600000);
        int dpx = pixel_of_id(scene, cam, W, H, pick_id_cell(data_cell));
        check("T1 GL: the data cell is on screen", dpx >= 0,
              "data cell not rendered");
        if (dpx >= 0) {
            SceneLayers zoning_on;
            SceneLayers zoning_off;
            zoning_off.zoning = false;
            std::vector<unsigned char> px_on = capture(scene, cam, cf, zoning_on);
            std::vector<unsigned char> px_off =
                capture(scene, cam, cf, zoning_off);
            const unsigned char *pon = &px_on[dpx * 4];
            const unsigned char *poff = &px_off[dpx * 4];
            check("T1 GL: the zoning toggle changes the data cell's hue",
                  pon[0] != poff[0] || pon[1] != poff[1] || pon[2] != poff[2],
                  "a Data-kind cell rendered identically with zoning on/off");
            int px_x = dpx % W, py = dpx / W; // bottom-left origin
            int y_top = H - 1 - py;
            uint32_t id_on = scene.pick(cam, W, H, px_x, y_top);
            check("T1 GL: the pick id is unaffected by the zoning toggle "
                  "(colour-pass only)",
                  id_on == pick_id_cell(data_cell),
                  "the pick id changed when zoning was toggled");
        }
    }

    // --- T3 (44-faithful-city-phase-a): the weather sky quad draws under ----
    // each fidelity tier, without GL error, and the weather layer toggle owns
    // the background pixels it draws. The tier->Atmosphere MAPPING itself
    // (byte-identical to the 2D banner's colours) is asserted in test_shell's
    // pure half, which needs no GL context; this is the GL-side "it renders"
    // smoke the T3 Tests section separately calls for.
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_hotcold()))),
            load(ndjson_hotcold()));
        space::TrajectorySet traj =
            space::build_trajectories(load(ndjson_hotcold()));
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);

        const scene3d::Atmosphere kTiers[3] = {
            [] {
                scene3d::Atmosphere a;
                a.front[0] = 0.10f;
                a.front[1] = 0.14f;
                a.front[2] = 0.22f;
                return a;
            }(),
            [] {
                scene3d::Atmosphere a;
                a.front[0] = 0.95f;
                a.front[1] = 0.75f;
                a.front[2] = 0.25f;
                a.fog_density = 0.35f;
                return a;
            }(),
            [] {
                scene3d::Atmosphere a;
                a.front[0] = 0.95f;
                a.front[1] = 0.45f;
                a.front[2] = 0.40f;
                a.fog_density = 0.6f;
                return a;
            }(),
        };
        SceneLayers weather_on;
        SceneLayers weather_off;
        weather_off.weather = false;
        std::vector<unsigned char> prev_on;
        for (int i = 0; i < 3; i++) {
            scene.set_atmosphere(kTiers[i]);
            std::vector<unsigned char> on = capture(scene, cam, cf, weather_on);
            check("T3 GL: the sky quad draws without GL error",
                  glGetError() == GL_NO_ERROR, "a GL error followed set_atmosphere/render");
            std::vector<unsigned char> off =
                capture(scene, cam, cf, weather_off);
            check("T3 GL: the weather layer toggle owns background pixels",
                  pixels_differ(on, off) > 0,
                  "turning weather off changed nothing — no sky was drawn");
            if (i > 0)
                check("T3 GL: distinct tiers paint visibly distinct skies",
                      pixels_differ(on, prev_on) > 0,
                      "two different Atmosphere values rendered identically");
            prev_on = on;
        }
    }

    // --- (e) 36 T4: the ANCHORED df trajectory puts pixels on screen ---------
    // scene-df-loop is a live dataflow capture (df_step + codeimage, no trace):
    // its PC path is rel and must ANCHOR to the single span (base+off) before the
    // scene can place it; an unanchored path projects nowhere and the tube is
    // silently dropped by the line.size() >= 6 gate. Render the trajectory layer
    // alone against a blank layer set — the anchored tube must add lit pixels.
    {
        Recording rec = load_path(ASMTEST_GOLDEN_DIR, "scene-df-loop.asmtrace");
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec, terr.proj);
        check("df GL: the path anchored", traj.anchored, "not anchored");
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);

        auto lit = [&](const SceneLayers &L) {
            glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
            glViewport(0, 0, W, H);
            glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            scene.render(cam, W, H, L);
            std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            int n = 0;
            for (size_t i = 0; i < px.size(); i += 4)
                if (px[i] > 16 || px[i + 1] > 16 || px[i + 2] > 16)
                    n++;
            return n;
        };
        SceneLayers tube_only{};
        tube_only.terrain = false;
        tube_only.statistical = false;
        tube_only.access_marks = false;
        tube_only.convergence = false; // exact (the tube) stays on
        // 44-faithful-city-phase-a T3: the weather sky quad now draws by
        // default (SceneLayers::weather) and would fill the WHOLE frame above
        // the 16-brightness `lit()` threshold, drowning out the tube's pixel
        // count this check reads — off here since this block is about the
        // trajectory tube, not the sky (T3 gets its own dedicated GL smoke).
        tube_only.weather = false;
        SceneLayers nothing = tube_only;
        nothing.exact = false;
        check("df GL: the anchored trajectory puts pixels on screen",
              lit(tube_only) > lit(nothing),
              "the anchored tube added no pixels — the path was dropped");
    }

    // --- (d) convergence arcs: a bright cross-thread arc renders, and toggles -
    // The live overlay's hint (10-T5): two tids sharing a cell draw a bright magenta
    // arc distinct from every tid colour and the torn-red gash. Rendered offscreen,
    // it is the only magenta on screen — so counting magenta pixels asserts both
    // that the arc draws and that its layer toggle turns it off.
    {
        Recording rec = load(ndjson_threads());
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        space::ConvergenceSet conv =
            space::detect_convergences(traj, terr.proj);
        check("threads recording has one convergence", conv.marks.size() == 1,
              "wrong convergence count");
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(conv, terr.proj);

        auto count_magenta = [&](const SceneLayers &layers) {
            glBindFramebuffer(GL_FRAMEBUFFER, cf.fbo);
            glViewport(0, 0, W, H);
            glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            scene.render(cam, W, H, layers);
            std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            int n = 0;
            for (size_t i = 0; i < px.size(); i += 4) {
                const unsigned char *p = &px[i];
                if (p[0] > 178 && p[2] > 153 && p[1] < 127)
                    n++; // bright magenta = the arc, nothing else in the palette
            }
            return n;
        };
        SceneLayers on; // convergence = true by default
        SceneLayers off;
        off.convergence = false;
        int with = count_magenta(on);
        int without = count_magenta(off);
        check("convergence arc renders (bright magenta present)", with > 0,
              "no arc pixels");
        check("convergence arc turns off with its layer", without == 0,
              "arc still visible with its layer off");
    }

    // --- T4 (44-faithful-city-phase-a): ghost districts — a stat surface ----
    // PHYSICALLY SEPARATE from the exact terrain, in the SAME scene. A
    // combined fixture (exact trace/codeimage in one region + a survey
    // targeting a disjoint data region) proves both surfaces render, the
    // ghost-fog layer toggle owns those extra pixels, presence survives a
    // playhead move (does not key on t), and an absent survey draws nothing.
    {
        std::string nd = kHdrExact;
        nd += "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,"
              "\"version\":0}\n";
        for (int i = 0; i < 3; i++)
            nd += "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n";
        nd += "{\"k\":\"survey\",\"sampler\":\"ibs-op\",\"edges\":["
              "{\"from_addr\":6291456,\"to_addr\":6291712,\"count\":50}],"
              "\"samples\":500,\"lost\":0}\n";
        nd += "{\"k\":\"end\",\"events\":5,\"truncated\":false}\n";
        Recording rec = load(nd);

        space::Region data_region;
        data_region.base = 0x600000;
        data_region.len = 4096;
        data_region.kind = space::Region::Data;
        std::vector<space::Region> regs = space::regions_from_codeimage(rec);
        regs.push_back(data_region);
        space::TerrainModel terr =
            space::build_terrain(space::build_projection(regs), rec);
        check("T4 GL: the combined fixture has a stat layer", terr.has_stat,
              "no survey layer built");
        check("T4 GL: the combined fixture also has exact code",
              !terr.code.empty(), "no exact code cells");
        space::TrajectorySet traj = space::build_trajectories(rec, terr.proj);

        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_stat_terrain(terr.stat);

        SceneLayers ghost_on;
        SceneLayers ghost_off;
        ghost_off.ghost_fog = false;
        std::vector<unsigned char> px_full_on = capture(scene, cam, cf, ghost_on);
        std::vector<unsigned char> px_full_off =
            capture(scene, cam, cf, ghost_off);
        check("T4 GL: the ghost-fog layer toggle puts pixels on screen",
              pixels_differ(px_full_on, px_full_off) > 0,
              "the stat surface drew nothing extra");

        // Presence survives a playhead move (hud.t change): move the exact
        // terrain to a DIFFERENT slice (the stat terrain is never re-uploaded
        // on a scrub, T4 step 4) and confirm the ghost-fog toggle still shows
        // a difference — proving the stat surface truly does not key on t.
        scene.set_terrain(terr.slice(0));
        std::vector<unsigned char> px_t0_on = capture(scene, cam, cf, ghost_on);
        std::vector<unsigned char> px_t0_off =
            capture(scene, cam, cf, ghost_off);
        check("T4 GL: the ghost-fog surface survives a playhead move",
              pixels_differ(px_t0_on, px_t0_off) > 0,
              "the stat surface vanished after the exact terrain re-sliced");

        // An absent survey uploads and draws nothing extra (no GL error, no
        // visible surface): clear_stat_terrain() is what the real caller
        // (gl_scene_host.cpp) calls when TerrainModel::has_stat is false.
        scene.clear_stat_terrain();
        std::vector<unsigned char> px_clear_on =
            capture(scene, cam, cf, ghost_on);
        std::vector<unsigned char> px_clear_off =
            capture(scene, cam, cf, ghost_off);
        check("T4 GL: has_stat=false draws nothing extra",
              pixels_differ(px_clear_on, px_clear_off) == 0,
              "the ghost-fog toggle changed pixels with no survey present");

        // Picking: a stat-only cell (no exact content) routes to hotedges via
        // the SAME cell-id space the exact terrain already uses (44's own
        // deviation note: the two surfaces share one (x,y) grid, so
        // resolve_pick's existing has_stat/TF_STAT branch already serves
        // both — no second pick-id band was needed).
        // Residency is credited to the survey edge's ARRIVAL (`to`) cell
        // (space/terrain.cpp's build_stat), so the flagged cell is at
        // to_addr = 0x600100, not the region base.
        uint32_t stat_cell = cell_of(terr.proj, terr.w, 0x600100);
        Pick spk = decode_pick(pick_id_cell(stat_cell), terr.w);
        auto slink = resolve_pick(terr, traj, "rec.asmtrace", spk);
        check("T4 GL: a stat-only cell resolves", slink.has_value(),
              "no link for the stat-only cell");
        if (slink)
            check("T4 GL: a stat-only cell routes to hotedges, never exact",
                  slink->view == dt_view::hotedges, "wrong view");
    }

    // --- T6 (44-faithful-city-phase-a): the followed citizen reuses its -----
    // underlying PC vertex's pick id — no new id space (the city doc's rule).
    // follow_step names a KNOWN placed vertex; the pick buffer at that
    // vertex's screen position must return the SAME id whether or not the
    // vehicle layer draws the head glyph over it.
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_hotcold()))),
            load(ndjson_hotcold()));
        space::TrajectorySet traj =
            space::build_trajectories(load(ndjson_hotcold()));
        const uint32_t n = terr.w;
        check("T6 GL: at least one trajectory to follow",
              !traj.trajectories.empty(), "no trajectory");
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);

        const uint64_t kFollow = 2; // t=2: a placed, exact PC vertex
        uint32_t vid = pick_id_vertex(n, kFollow);
        int vpx = pixel_of_id(scene, cam, W, H, vid);
        check("T6 GL: the followed vertex is on screen", vpx >= 0,
              "vertex not rendered");
        if (vpx >= 0) {
            scene.follow_step = kFollow;
            SceneLayers with_vehicle; // vehicle = true by default
            capture(scene, cam, cf, with_vehicle); // draws the head glyph
            int px_x = vpx % W, py = vpx / W;       // bottom-left origin
            int y_top = H - 1 - py;
            uint32_t got = scene.pick(cam, W, H, px_x, y_top);
            check("T6 GL: the vehicle draw does not disturb the underlying "
                  "vertex's pick id",
                  got == vid, "the head glyph changed the pick id underneath it");
        }

        // An absent/unplaced follow_step draws NOTHING — never a mis-snapped
        // vehicle. A step with no matching TrajPoint.t on any exact
        // trajectory: the vehicle layer toggle must then be a no-op.
        scene.follow_step = 999999;
        SceneLayers v_on;
        SceneLayers v_off;
        v_off.vehicle = false;
        check("T6 GL: an absent follow_step draws no vehicle",
              pixels_differ(capture(scene, cam, cf, v_on),
                            capture(scene, cam, cf, v_off)) == 0,
              "a follow_step naming no placed vertex still drew something");
    }

    // --- T1 (49-one-time-truth): the worldline clips (dims, NEVER discards) -
    // at the terrain playhead. At the end of the recording the cut is a no-op
    // (byte-identical to the pre-49 render); at half, the tail past the cut
    // dims but stays on screen.
    {
        Recording rec = load(ndjson_hotcold());
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        check("T1 GL: hotcold recording has a real step extent",
              terr.nsteps > 1, "no steps to clip");
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);
        scene.follow_step = 999999; // no vehicle glyph in this frame

        SceneLayers layers; // terrain + exact, defaults

        scene.slice_step = terr.nsteps; // at the end: step 4's no-op
        std::vector<unsigned char> px_end = capture(scene, cam, cf, layers);
        scene.slice_step = terr.nsteps / 2; // half: the tail must dim
        std::vector<unsigned char> px_half = capture(scene, cam, cf, layers);

        size_t dimmed = 0, driven_dark = 0;
        for (size_t i = 0; i + 3 < px_end.size(); i += 4) {
            float le = lum(&px_end[i]);
            float lh = lum(&px_half[i]);
            if (le > 40.0f) { // a clearly-lit path/spur pixel in the baseline
                if (lh < le - 5.0f)
                    dimmed++;
                if (lh <= 8.0f) // ~ the background clear colour's luminance
                    driven_dark++;
            }
        }
        check("T1 GL: at least one path pixel dims under a half playhead",
              dimmed > 0, "no dimming observed — is uTimeCutY reaching the "
                         "shader?");
        check("T1 GL: a dimmed pixel is never driven to background (dim, "
              "never discard)",
              driven_dark == 0,
              "a clipped pixel went dark enough to read as background");

        scene.slice_step = terr.nsteps + 5; // past the end too
        std::vector<unsigned char> px_past_end =
            capture(scene, cam, cf, layers);
        check("T1 GL: the cut is a no-op once slice_step reaches the end",
              px_past_end == px_end,
              "a playhead past the recording must render byte-identically "
              "to one at it");

        scene.slice_step = UINT64_MAX; // reset to the "unconfigured" default
    }

    // --- T2 (49-one-time-truth): the execution-front glyph marks the ---------
    // playhead's own boundary — a DIFFERENT clock and colour from the T6
    // followed-citizen vehicle.
    {
        Recording rec = load(ndjson_hotcold());
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        check("T2 GL: hotcold recording has a real step extent",
              terr.nsteps > 1, "not enough steps to place a mid-recording "
                              "front");
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);
        scene.follow_step = 999999; // no vehicle glyph confusing this count

        // The front glyph's own fixed colour (1.0, 0.65, 0.15) — distinct
        // from every tid hue in tid_color's palette, the vehicle's white
        // fallback, the torn-red gash and the convergence-arc magenta.
        auto count_amber = [&](const SceneLayers &layers) {
            std::vector<unsigned char> px = capture(scene, cam, cf, layers);
            int n = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                const unsigned char *p = &px[i];
                if (p[0] > 220 && p[1] > 130 && p[1] < 200 && p[2] < 90)
                    n++;
            }
            return n;
        };

        scene.slice_step = terr.nsteps / 2; // a mid-recording playhead
        SceneLayers on;  // exact = true (default): the front glyph gates on it
        SceneLayers off;
        off.exact = false;
        check("T2 GL: the execution front marks the playhead (amber "
              "present)",
              count_amber(on) > 0,
              "no front-glyph pixels at a mid playhead");
        check("T2 GL: the front glyph is gated on the exact layer",
              count_amber(off) == 0,
              "the front glyph drew with the exact layer off");

        // No trajectory at all => no qualifying vertex => no glyph, never a
        // snapped one (mirrors 44-T6's own rule for the vehicle).
        scene.set_trajectories(space::TrajectorySet{}, terr.proj);
        check("T2 GL: no trajectory data, no front glyph",
              count_amber(on) == 0,
              "a front glyph appeared with no trajectory to place it on");

        scene.slice_step = UINT64_MAX; // reset to the "unconfigured" default
    }

    // ===== 3. the T7 GOLDEN SCENES, rendered ================================
    // The pure half above asserted what each scene MODELS; these assert that the
    // scene puts it on screen — coarse terrain and an exact tube for the abs
    // scene, the torn gash for its truncated twin, a statistical layer with
    // nothing exact beside it, and a rich-`mem` scene that draws nothing extra.
    //
    // Rendered through the camera's TOP-DOWN preset into a LARGER framebuffer
    // than the cases above, and deliberately so: a golden scene's live region is
    // 18 bytes wide on a 64x64 plane, so at 160x160 in the three-quarter view a
    // cell is barely a pixel and the plane's far corner disappears behind its
    // neighbour. The top-down preset is the view the doc prescribes for reading
    // a cell ("3D to find, 2D to read" — camera.h's plain 2D-ish fallback), so
    // asserting the recorded heat through it is the faithful measurement, and it
    // exercises the preset as a side effect.
    const int GW = 384, GH = 384;
    ColorFbo gcf;
    Camera gcam;
    gcam.top_down();
    if (!gcf.create(GW, GH)) {
        skip("golden-scene framebuffer incomplete on this driver");
        scene.shutdown();
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(dpy, ctx);
        eglTerminate(dpy);
        return failures ? 1 : 0;
    }

    // --- (e) the abs scene: coarse terrain AND a trajectory both render -----
    std::vector<unsigned char> abs_frame;
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_GOLDEN_DIR, "scene-abs-loop.asmtrace"));
        upload(scene, m, m.terr.full());
        const uint32_t n = m.terr.w;

        SceneLayers all;
        abs_frame = capture(scene, gcam, gcf, all);

        // The terrain renders, and renders the recorded heat: the loop body's
        // cell is brighter than the prologue's.
        uint32_t hot = cell_of(m.terr.proj, n, kLoopBody);
        uint32_t cold = cell_of(m.terr.proj, n, kLoopBase);
        int hot_px = pixel_of_id(scene, gcam, GW, GH, pick_id_cell(hot));
        int cold_px = pixel_of_id(scene, gcam, GW, GH, pick_id_cell(cold));
        check("golden abs scene: the hot cell is on screen", hot_px >= 0,
              "hot cell not rendered");
        check("golden abs scene: the cold cell is on screen", cold_px >= 0,
              "cold cell not rendered");
        if (hot_px >= 0 && cold_px >= 0)
            check("golden abs scene: the loop body renders brighter",
                  lum(&abs_frame[hot_px * 4]) >
                      lum(&abs_frame[cold_px * 4]) + 8.0f,
                  "hot not brighter");

        // The exact trajectory draws: switching its layer off changes pixels.
        SceneLayers no_exact;
        no_exact.exact = false;
        std::vector<unsigned char> without =
            capture(scene, gcam, gcf, no_exact);
        check("golden abs scene: the exact trajectory puts pixels on screen",
              pixels_differ(abs_frame, without) > 0,
              "the exact layer toggle changed nothing — no tube was drawn");
        // Nothing statistical exists here, so its toggle must be a no-op.
        SceneLayers no_stat;
        no_stat.statistical = false;
        check("golden abs scene: no statistical mark to hide",
              pixels_differ(abs_frame, capture(scene, gcam, gcf, no_stat)) == 0,
              "a statistical mark in an exact recording");
    }

    // --- (f) the truncated twin: the torn gash renders ----------------------
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_GOLDEN_DIR, "scene-abs-loop-truncated.asmtrace"));
        upload(scene, m, m.terr.full());
        const uint32_t n = m.terr.w;
        std::vector<unsigned char> px =
            capture(scene, gcam, gcf, SceneLayers{});

        uint32_t torn = cell_of(m.terr.proj, n, kLoopBody);
        int tpx = pixel_of_id(scene, gcam, GW, GH, pick_id_cell(torn));
        check("golden torn scene: the torn cell is on screen", tpx >= 0,
              "torn cell not rendered");
        if (tpx >= 0) {
            const unsigned char *p = &px[tpx * 4];
            check("golden torn scene: the cell paints the red gash",
                  p[0] > p[1] + 40 && p[0] > p[2] + 40, "not a red gash");
        }
    }

    // --- (g) the statistical scene: a STAT layer, and nothing exact ---------
    // Isolation asserted at the PIXEL level: turning the EXACT layer off changes
    // no pixel (there is no exact tube to hide), while turning the STATISTICAL
    // layer off does — so what is on screen is the sampled layer and only that.
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_FIXTURE_DIR, "obs-survey-ibs.asmtrace"),
            {survey_window()});
        upload(scene, m, m.terr.stat); // the SEPARATE statistical terrain
        std::vector<unsigned char> px =
            capture(scene, gcam, gcf, SceneLayers{});

        SceneLayers no_stat;
        no_stat.statistical = false;
        SceneLayers no_exact;
        no_exact.exact = false;
        std::vector<unsigned char> off_px = capture(scene, gcam, gcf, no_stat);
        check("golden stat scene: the statistical layer puts pixels on screen",
              pixels_differ(px, off_px) > 0,
              "the statistical toggle changed nothing");
        check("golden stat scene: there is NOTHING on the exact layer",
              pixels_differ(px, capture(scene, gcam, gcf, no_exact)) == 0,
              "a sampled residency rendered as an exact tube");

        // The two checks above only say WHICH TOGGLE owns those pixels. They do
        // not say the sampled mark LOOKS sampled — and that is the fidelity
        // invariant (scene.h "opaque exact tubes vs stippled statistical
        // residency"; terrain.h "a sampled residency ... must never render as
        // [an exact density]"; the doc's "different marks — solid tube vs
        // translucent stipple"). Without the two below, deleting the shader's
        // stipple `discard` and raising the line alpha to 1.0 makes a survey
        // render pixel-identically to an exact tube with the whole suite still
        // green. So look at the marked pixels themselves, the way (d) reads the
        // arc's colour rather than merely toggling it.
        std::vector<size_t> marked; // pixel indices the statistical layer drew
        for (size_t i = 0, p = 0; i < px.size(); i += 4, p++)
            if (px[i] != off_px[i] || px[i + 1] != off_px[i + 1] ||
                px[i + 2] != off_px[i + 2])
                marked.push_back(p);

        // STIPPLED: the shader discards a band of `(x+y) mod 8`, so the drawn
        // pixels leave whole diagonal phases EMPTY. A solid line fills all eight
        // roughly evenly. Counting empty phases (rather than testing one exact
        // phase) keeps this independent of the pattern's offset and period.
        int phase[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (size_t p : marked)
            phase[((p % static_cast<size_t>(GW)) +
                   (p / static_cast<size_t>(GW))) %
                  8]++;
        int empty_phases = 0;
        for (int k = 0; k < 8; k++)
            if (phase[k] == 0)
                empty_phases++;
        check("golden stat scene: the sampled mark is STIPPLED, not solid",
              !marked.empty() && empty_phases >= 3,
              "the residency filled every diagonal phase — it drew as a solid "
              "tube, which is exactly what an exact path draws");

        // TRANSLUCENT: the line blends at alpha < 1 over a dark background, so
        // no marked pixel reaches the raw per-tid palette brightness an exact
        // tube paints (palette entry 0 is {0.95,0.85,0.25} -> red 242).
        int brightest = 0;
        for (size_t p : marked)
            if (px[p * 4] > brightest)
                brightest = px[p * 4];
        check("golden stat scene: the sampled mark is TRANSLUCENT, not opaque",
              !marked.empty() && brightest < 200,
              "a residency pixel reached full palette brightness — it drew as "
              "an opaque exact tube");
    }

    // --- (h) the synthetic rich-`mem` scene: present, and inert -------------
    // It is the abs scene plus hand-authored `mem` lines, so with the coarse-rung
    // projection it must render PIXEL-IDENTICALLY to that scene: the rich rung
    // draws nothing until a producer (and a data region) land, and it certainly
    // never invents a data plane.
    {
        SceneModel m = build_scene(load_path(ASMTEST_GOLDEN_DIR "/scenes",
                                             "mem-rich-synthetic.asmtrace"));
        upload(scene, m, m.terr.full());
        std::vector<unsigned char> px =
            capture(scene, gcam, gcf, SceneLayers{});
        check("synthetic mem scene: renders identically to its coarse twin",
              pixels_differ(abs_frame, px) == 0,
              "the inert rich rung changed the picture");

        SceneLayers no_marks;
        no_marks.access_marks = false;
        check("synthetic mem scene: no access-mark spur is drawn",
              pixels_differ(px, capture(scene, gcam, gcf, no_marks)) == 0,
              "a spur to a data cell that no region holds");
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
