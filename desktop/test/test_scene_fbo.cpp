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
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "scene3d/camera.h"
#include "scene3d/pick.h"
#include "scene3d/scene.h"
#include "scene3d/scene_kind.h"    // T1 (59): SceneKind
#include "scene3d/standalone.h"    // T1 (59): the pure standalone models
#include "scene3d/standalone_gl.h" // T1 (59): the second substrate's renderer
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
#ifndef ASMTEST_DESKTOP_SRC_DIR
#error "ASMTEST_DESKTOP_SRC_DIR must be defined by the build (mk/desktop.mk)"
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
              s.slice_step == UINT64_MAX,
              "a default-constructed Scene must "
              "not retroactively clip a caller "
              "that never touches slice_step");
        s.slice_step = 42;
        check("Scene::slice_step is a plain settable field", s.slice_step == 42,
              "the value did not round-trip");
    }
}

// ---- T6 (55) step 1: no wide glLineWidth survives on the colour path --------
// The brief's own "Done when" is "No `glLineWidth` call above 1.0 remains".
// Two DO remain, deliberately and visibly: the pick pass keeps its pre-T6
// wide-line click-target route (byte-for-byte the draw it always made) and
// selects between that and the quad expansion at run time — see
// Scene::pick_widening. So the property that is actually true, and the one
// worth pinning, is narrower and stronger than a grep for the call: every
// glLineWidth argument left in the scene is either 1.0 or one of the two named
// pick-route constants. A future layer that quietly adds glLineWidth(4.0) to
// the colour path — the exact regression T6 exists to prevent, and one no
// pixel assertion would catch on a driver that happens to support it — fails
// here. Source-scanning follows test_theme.cpp's existing idiom
// (ASMTEST_DESKTOP_SRC_DIR).
static void pure_line_width_checks() {
    const std::string path =
        std::string(ASMTEST_DESKTOP_SRC_DIR) + "/scene3d/scene.cpp";
    std::ifstream in(path);
    check("T6: scene.cpp is readable for the line-width scan", in.good(),
          path.c_str());
    std::string line;
    int seen = 0;
    while (std::getline(in, line)) {
        const size_t at = line.find("glLineWidth(");
        if (at == std::string::npos)
            continue;
        const size_t open = at + std::strlen("glLineWidth(");
        const size_t close = line.find(')', open);
        if (close == std::string::npos)
            continue;
        const std::string arg = line.substr(open, close - open);
        seen++;
        const bool allowed = arg == "1.0f" || arg == "kPickConvWidthPx" ||
                             arg == "kPickSpurWidthPx";
        check(("T6: glLineWidth argument is 1.0 or a named pick-route "
               "constant, got <" +
               arg + ">")
                  .c_str(),
              allowed,
              "a wide glLineWidth on the colour path is exactly what T6 step 1 "
              "removed: it is silently clamped (or GL_INVALID_VALUE) on a core "
              "profile, so the mark class it separates disappears there");
    }
    check("T6: the line-width scan actually found the pick pass's own calls",
          seen >= 2,
          "no glLineWidth call was found at all — the scan is looking at the "
          "wrong file and would pass vacuously");
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

// T5 (55): the most common RGB triple in a capture — background, for any
// reasonably-framed scene, since the plane's silhouette occupies less area
// than the space around it. Used instead of a hardcoded clear-colour byte
// value (rounding is a driver detail, not this test's concern) or a fixed
// pixel index (not guaranteed background for every camera/fixture pairing).
static void mode_color(const std::vector<unsigned char> &px,
                       unsigned char out[3]) {
    std::map<uint32_t, size_t> counts;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        uint32_t key = (static_cast<uint32_t>(px[i]) << 16) |
                      (static_cast<uint32_t>(px[i + 1]) << 8) | px[i + 2];
        counts[key]++;
    }
    uint32_t best_key = 0;
    size_t best_n = 0;
    for (const auto &kv : counts)
        if (kv.second > best_n) {
            best_n = kv.second;
            best_key = kv.first;
        }
    out[0] = static_cast<unsigned char>((best_key >> 16) & 0xff);
    out[1] = static_cast<unsigned char>((best_key >> 8) & 0xff);
    out[2] = static_cast<unsigned char>(best_key & 0xff);
}

// T4 (55): the C++ mirror of kTrajFrag/kStatFrag's own `ign()` — same
// formula, verbatim — so a test can check that a shader's discard decision
// at a given pixel actually IS Interleaved Gradient Noise, not merely "some
// pattern or other". gl_FragCoord is pixel-CENTRE, bottom-left origin — the
// same convention glReadPixels/capture()'s buffer already uses, so a buffer
// index needs no flip to become (x, y).
static float ign_cpu(float x, float y) {
    auto fract = [](float v) { return v - std::floor(v); };
    return fract(52.9829189f * fract(x * 0.06711056f + y * 0.00583715f));
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
    pure_line_width_checks(); // T6 (55) step 1

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
    // T6 GL (55) step 2: the query actually ran and reports a sane range
    // (min >= 1, matching the spec's guaranteed floor; max >= min).
    check("T6 GL: GL_ALIASED_LINE_WIDTH_RANGE queried at init_gl",
          scene.aliased_line_width_range[0] >= 1.0f &&
              scene.aliased_line_width_range[1] >=
                  scene.aliased_line_width_range[0],
          "line-width range looks unqueried or invalid");

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
            std::vector<unsigned char> px_on =
                capture(scene, cam, cf, zoning_on);
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
                  glGetError() == GL_NO_ERROR,
                  "a GL error followed set_atmosphere/render");
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

        // --- T3 (47-scene-inspect-and-pickable-overlays): the arc is ---------
        // PICKABLE — its id (pick_id_conv, band-sized by the scene's own
        // pick_bands()) must appear somewhere in the pick buffer for this
        // known-convergence fixture, and decoding that id back must resolve
        // to Pick::Conv, index 0 (the only mark).
        {
            const scene3d::PickBands bands = scene.pick_bands();
            check("T3 GL: pick_bands reports one convergence mark",
                  bands.nconv == 1,
                  ("got " + std::to_string(bands.nconv)).c_str());
            const uint32_t want_id = pick_id_conv(terr.w, bands.npts, /*i=*/0);
            std::vector<uint32_t> ids;
            scene.render_pick_buffer(cam, W, H, ids);
            bool found = false;
            for (uint32_t id : ids)
                if (id == want_id) {
                    found = true;
                    break;
                }
            check("T3 GL: the convergence arc's id appears in the pick buffer",
                  found, "no pixel carried the arc's pick id");
            Pick decoded = decode_pick(want_id, terr.w, bands);
            check("T3 GL: the arc id decodes to Pick::Conv, index 0",
                  decoded.kind == Pick::Conv && decoded.conv == 0,
                  "decode mismatch");
        }
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
        std::vector<unsigned char> px_full_on =
            capture(scene, cam, cf, ghost_on);
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
        // 61 T5: follow_step is set BEFORE the pick render, and the order is
        // now load-bearing rather than incidental. This fixture visits address
        // 4194304 five times (t=0..4); with trace time off the Y axis those
        // five visits are ONE POINT IN SPACE, so exactly one of their ids can
        // own that pixel. Which one is not arbitrary — the scene re-draws the
        // FOLLOWED vertex so it wins, because doc 44's rule is that the
        // followed citizen reuses its underlying PC vertex's pick id and the
        // head glyph sits on exactly this point. Asking for t=2's id while
        // following t=0 would be asking to distinguish two visits that are no
        // longer distinguishable in space; the playhead is what selects among
        // them now. That is the axis budget being spent, not a pick defect.
        scene.follow_step = kFollow;
        int vpx = pixel_of_id(scene, cam, W, H, vid);
        check("T6 GL: the followed vertex is on screen", vpx >= 0,
              "vertex not rendered");
        if (vpx >= 0) {
            SceneLayers with_vehicle;              // vehicle = true by default
            capture(scene, cam, cf, with_vehicle); // draws the head glyph
            int px_x = vpx % W, py = vpx / W;      // bottom-left origin
            int y_top = H - 1 - py;
            uint32_t got = scene.pick(cam, W, H, px_x, y_top);
            check("T6 GL: the vehicle draw does not disturb the underlying "
                  "vertex's pick id",
                  got == vid,
                  "the head glyph changed the pick id underneath it");
            // 61 T5: and the follow is what CHOOSES among coincident visits.
            // Move the playhead to another visit of the SAME address and the
            // same pixel must now answer with THAT visit — otherwise the
            // earliest visit would own the point forever and following the
            // path would stop changing what a click means.
            scene.follow_step = 4; // another visit to 4194304
            const uint32_t got4 = scene.pick(cam, W, H, px_x, y_top);
            check("T6 GL: the followed visit owns the shared pixel",
                  got4 == pick_id_vertex(n, 4),
                  "with time off the Y axis several visits to one address "
                  "share a pixel, so the FOLLOWED one must win it; this pixel "
                  "still answered with a visit the reader is not on");
            scene.follow_step = kFollow; // leave the block as it found it
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
        // T2 (55) halos OFF for this measurement, and it costs the check
        // nothing: the halo pass draws with uHasTimeCut = 0, so it is not part
        // of the playhead cut at all. What it WOULD add is a confound — the
        // execution-front glyph moves when the playhead does, and the pixels it
        // vacates then show background-coloured halo chrome rather than the
        // terrain they used to. A pixel showing chrome is not a discarded
        // worldline, but the luminance rule below cannot tell the difference.
        // That a dimmed worldline is still visible WITH halos on is asserted
        // separately, in T2's own case.
        layers.halos = false;

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
              dimmed > 0,
              "no dimming observed — is uTimeCutY reaching the "
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
              terr.nsteps > 1,
              "not enough steps to place a mid-recording "
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
        SceneLayers on; // exact = true (default): the front glyph gates on it
        SceneLayers off;
        off.exact = false;
        check("T2 GL: the execution front marks the playhead (amber "
              "present)",
              count_amber(on) > 0, "no front-glyph pixels at a mid playhead");
        check("T2 GL: the front glyph is gated on the exact layer",
              count_amber(off) == 0,
              "the front glyph drew with the exact layer off");

        // No trajectory at all => no qualifying vertex => no glyph, never a
        // snapped one (mirrors 44-T6's own rule for the vehicle).
        scene.set_trajectories(space::TrajectorySet{}, terr.proj);
        check("T2 GL: no trajectory data, no front glyph", count_amber(on) == 0,
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
        //
        // Measured from a TERRAIN-ONLY frame, not from `abs_frame`. The claim
        // is about the terrain's own encoding, and pixel_of_id picks whichever
        // pixel of the cell comes first in the buffer — which for a top-down
        // view of a path that runs through the loop body is quite often a
        // pixel the worldline itself covers. That was always a latent flaw
        // (a line has always been drawn over the terrain); 55 T2's halo, by
        // widening what each line occupies, made it fire. Reading the surface
        // with the marks that sit on top of it turned off measures the thing
        // this check is named for.
        uint32_t hot = cell_of(m.terr.proj, n, kLoopBody);
        uint32_t cold = cell_of(m.terr.proj, n, kLoopBase);
        int hot_px = pixel_of_id(scene, gcam, GW, GH, pick_id_cell(hot));
        int cold_px = pixel_of_id(scene, gcam, GW, GH, pick_id_cell(cold));
        check("golden abs scene: the hot cell is on screen", hot_px >= 0,
              "hot cell not rendered");
        check("golden abs scene: the cold cell is on screen", cold_px >= 0,
              "cold cell not rendered");
        if (hot_px >= 0 && cold_px >= 0) {
            SceneLayers surface_only;
            surface_only.exact = surface_only.statistical = false;
            surface_only.access_marks = surface_only.convergence = false;
            surface_only.vehicle = surface_only.canopy = false;
            surface_only.mispred = false;
            std::vector<unsigned char> surf =
                capture(scene, gcam, gcf, surface_only);
            check("golden abs scene: the loop body renders brighter",
                  lum(&surf[hot_px * 4]) > lum(&surf[cold_px * 4]) + 8.0f,
                  "hot not brighter");
        }

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

        // T3 (55): a TORN cell never grows a contour band — its height is a
        // known LOWER BOUND, not a measurement, so a band would claim a
        // precision the rubble does not have. Checked at the specific torn
        // cell's own pixel (not the whole frame, which this fixture's other
        // content — CHURN, say — may legitimately still band).
        SceneLayers no_contours;
        no_contours.contours = false;
        std::vector<unsigned char> px_no_contours =
            capture(scene, gcam, gcf, no_contours);
        if (tpx >= 0) {
            const unsigned char *pc = &px[tpx * 4];
            const unsigned char *pnc = &px_no_contours[tpx * 4];
            check("T3 GL: a TORN cell carries no contour band",
                  pc[0] == pnc[0] && pc[1] == pnc[1] && pc[2] == pnc[2],
                  "the contour toggle changed the torn cell's own pixel");
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
        //
        // 55 T1: computed from an EDL-OFF pair, not px/off_px above. EDL is a
        // depth cue with its own sampling radius (kEdlFrag), so it darkens
        // BACKGROUND pixels NEAR the line too — pixels the line's OWN fragment
        // shader never touched — which is correct EDL behaviour but would
        // otherwise fill in the discard pattern's "empty" phases with a faint,
        // orthogonal difference and break this specific isolation. The base
        // stipple/translucency technique this section verifies is itself
        // independent of EDL (a later, separate pass), so it is verified with
        // EDL isolated out, exactly like every other single-variable toggle in
        // this test.
        SceneLayers stat_no_edl;
        stat_no_edl.edl = false;
        SceneLayers no_stat_no_edl = no_stat;
        no_stat_no_edl.edl = false;
        std::vector<unsigned char> px_no_edl =
            capture(scene, gcam, gcf, stat_no_edl);
        std::vector<unsigned char> off_px_no_edl =
            capture(scene, gcam, gcf, no_stat_no_edl);
        std::vector<size_t> marked; // pixel indices the statistical layer drew
        for (size_t i = 0, p = 0; i < px_no_edl.size(); i += 4, p++)
            if (px_no_edl[i] != off_px_no_edl[i] ||
                px_no_edl[i + 1] != off_px_no_edl[i + 1] ||
                px_no_edl[i + 2] != off_px_no_edl[i + 2])
                marked.push_back(p);

        // STIPPLED (55 T4: via Interleaved Gradient Noise, not the old
        // regular (x+y) mod 8 checkerboard — see kTrajFrag's own comment for
        // why): a marked (surviving) pixel is drawn FULLY OPAQUE now, so
        // "not solid" is no longer a per-pixel brightness/alpha question —
        // it is a DISCARD PATTERN question. Checked directly against the
        // shader's own formula (ign_cpu, verbatim) at each marked pixel's
        // (x, y): a survivor must have ign() <= the line's alpha (0.45,
        // scene.cpp's set_trajectories literal), so most marked pixels
        // should agree with the CPU-side prediction. A generous 90% bar
        // absorbs GPU/CPU float-rounding disagreement right at the
        // threshold; a shader that stopped discarding entirely (solid fill)
        // would fail this by roughly half, not by a few edge pixels.
        constexpr float kStatTrajAlpha = 0.45f; // set_trajectories' own literal
        int agree = 0;
        for (size_t p : marked) {
            float x = static_cast<float>(p % static_cast<size_t>(GW)) + 0.5f;
            float y = static_cast<float>(p / static_cast<size_t>(GW)) + 0.5f;
            if (ign_cpu(x, y) <= kStatTrajAlpha)
                agree++;
        }
        double agree_frac =
            marked.empty()
                ? 0.0
                : static_cast<double>(agree) / static_cast<double>(marked.size());
        check("golden stat scene: the sampled mark is STIPPLED via "
              "Interleaved Gradient Noise, not solid",
              !marked.empty() && agree_frac > 0.9,
              "marked pixels do not match the shader's own IGN discard "
              "formula — looks solid (discard removed) rather than dithered");
        // (55 T4's replacement for the old per-pixel "TRANSLUCENT, not
        // opaque" alpha-blend check, which no longer applies now that a
        // surviving fragment is fully opaque by construction — see
        // kTrajFrag's own comment: translucency is now spent as COVERAGE,
        // not blend. The IGN-formula match above already verifies coverage
        // is governed by the 0.45 keep-probability, which subsumes it.)
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

    // --- (i) T1 (55): Eye-Dome Lighting darkens a depth discontinuity, and
    //     never touches the pick pass ----------------------------------------
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
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);

        SceneLayers edl_on;
        SceneLayers edl_off;
        edl_off.edl = false;
        std::vector<unsigned char> px_edl_on = capture(scene, cam, cf, edl_on);
        std::vector<unsigned char> px_edl_off =
            capture(scene, cam, cf, edl_off);

        check("T1 GL: EDL visibly changes the render",
              pixels_differ(px_edl_on, px_edl_off) > 0,
              "EDL on/off produced pixel-identical frames");

        // A pixel deep inside a large flat run of background (no depth
        // discontinuity anywhere within the tap radius) must read the SAME
        // under EDL on and off — T1's own fidelity bar: EDL adds a cue only
        // where there is something to cue, never a global tint. The mode
        // colour (not a fixed index) is background for any reasonably-framed
        // scene, since the plane's silhouette covers less area than the
        // space around it.
        unsigned char bg_on[3], bg_off[3];
        mode_color(px_edl_on, bg_on);
        mode_color(px_edl_off, bg_off);
        check("T1 GL: a pure-background pixel is unchanged by EDL",
              bg_on[0] == bg_off[0] && bg_on[1] == bg_off[1] &&
                  bg_on[2] == bg_off[2],
              "the background colour changed with EDL on");

        // The pick pass is sacred (D7): EDL runs on the colour path only.
        std::vector<uint32_t> ids_edl_on, ids_edl_off;
        scene.render_pick_buffer(cam, W, H, ids_edl_on);
        scene.render_pick_buffer(cam, W, H, ids_edl_off);
        check("T1 GL: the pick buffer is byte-identical with EDL on and off",
              ids_edl_on == ids_edl_off,
              "EDL leaked into the colour-ID pick pass");
    }

    // --- (j) T5 (55): MSAA smooths an aliased edge, and never touches the
    //     pick pass -----------------------------------------------------------
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_hotcold()))),
            load(ndjson_hotcold()));
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(space::TrajectorySet{}, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);
        SceneLayers terrain_only;
        terrain_only.access_marks = terrain_only.convergence = false;
        terrain_only.weather = false; // the sky quad covers 100% of the
                                      // background, which would hide the
                                      // clear-colour edge this test reads

        scene.msaa_samples = 0;
        std::vector<unsigned char> px_msaa_off =
            capture(scene, cam, cf, terrain_only);
        scene.msaa_samples = 4;
        std::vector<unsigned char> px_msaa_on =
            capture(scene, cam, cf, terrain_only);

        check("T5 GL: MSAA visibly changes the render",
              pixels_differ(px_msaa_on, px_msaa_off) > 0,
              "MSAA on/off produced pixel-identical frames (no aliased edge "
              "in this fixture, or MSAA silently degraded to off)");

        // The background colour (the mode, not a fixed index or a hardcoded
        // byte value — see mode_color's own comment). Single-sample
        // rasterization has NO partial coverage (a pixel centre is either
        // inside the terrain or it is not), so every background pixel with
        // MSAA off is EXACTLY this colour. MSAA blends the 4 sub-samples of a
        // pixel straddling the terrain's silhouette, so a pixel that
        // rasterized as pure background under single-sampling can become a
        // partial blend once ANY of its sub-samples lands on the terrain —
        // the "intermediate value" the brief's own test asks for, without
        // needing to know in advance which pixel is on the edge.
        unsigned char clear[3];
        mode_color(px_msaa_off, clear);
        auto is_clear = [&clear](const unsigned char *p) {
            return p[0] == clear[0] && p[1] == clear[1] && p[2] == clear[2];
        };
        int revealed = 0;
        for (size_t i = 0; i + 3 < px_msaa_off.size(); i += 4)
            if (is_clear(&px_msaa_off[i]) && !is_clear(&px_msaa_on[i]))
                revealed++;
        check("T5 GL: MSAA reveals partial coverage at an aliased silhouette "
              "edge",
              revealed > 0,
              "no background pixel gained a blended value under MSAA");

        scene.msaa_samples = 0;
        std::vector<uint32_t> ids_msaa_off;
        scene.render_pick_buffer(cam, W, H, ids_msaa_off);
        scene.msaa_samples = 4;
        std::vector<uint32_t> ids_msaa_on;
        scene.render_pick_buffer(cam, W, H, ids_msaa_on);
        check("T5 GL: the pick buffer is byte-identical with MSAA on and off",
              ids_msaa_on == ids_msaa_off,
              "MSAA leaked into the colour-ID pick pass");
        scene.msaa_samples = 0; // restore Scene's own conservative default
    }

    // --- (k) T3 (55): contour band width stays roughly constant in SCREEN
    //     pixels across a large change in camera distance ---------------------
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_hotcold()))),
            load(ndjson_hotcold()));
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(space::TrajectorySet{}, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);

        SceneLayers contours_on;
        contours_on.exact = contours_on.statistical =
            contours_on.access_marks = contours_on.convergence =
                contours_on.vehicle = contours_on.edl = false;
        SceneLayers contours_off = contours_on;
        contours_off.contours = false;

        // Frame the KNOWN hot/cold boundary directly (Camera::frame, not the
        // default plane-centre target) so both distances look at the same
        // gradient regardless of where the Hilbert layout happens to place
        // it — a plane-centre target risks the boundary landing near an
        // edge, out of frame, at one of the two radii.
        float hot_u = 0.0f, hot_v = 0.0f;
        terr.proj.project(kLoopBody, &hot_u, &hot_v);

        // A contour-affected pixel is exactly one where the on/off renders
        // disagree — reusing the SceneLayers toggle rather than re-deriving
        // the shader's own colour math a second time in C++. Reports the
        // pixel-run lengths (in a scanline) of contiguous affected pixels: a
        // fixed WORLD-space band (the pre-55 bug) widens as the camera
        // dollies in, so its run length in pixels scales with distance; a
        // fixed SCREEN-space band (fwidth) does not.
        auto band_run_lengths = [&](float radius) {
            Camera c;
            c.frame(hot_u, hot_v,
                   Camera::clampf(radius, Camera::kMinRadius,
                                  Camera::kMaxRadius));
            std::vector<unsigned char> on = capture(scene, c, cf, contours_on);
            std::vector<unsigned char> off =
                capture(scene, c, cf, contours_off);
            std::vector<int> runs;
            for (int y = 0; y < cf.h; y++) {
                int run = 0;
                for (int x = 0; x < cf.w; x++) {
                    size_t i = (static_cast<size_t>(y) * cf.w +
                               static_cast<size_t>(x)) *
                              4;
                    bool differs = on[i] != off[i] || on[i + 1] != off[i + 1] ||
                                  on[i + 2] != off[i + 2];
                    if (differs) {
                        run++;
                    } else {
                        if (run > 0)
                            runs.push_back(run);
                        run = 0;
                    }
                }
                if (run > 0)
                    runs.push_back(run);
            }
            return runs;
        };
        auto mean_run = [](const std::vector<int> &v) {
            if (v.empty())
                return 0.0;
            double s = 0.0;
            for (int x : v)
                s += x;
            return s / static_cast<double>(v.size());
        };

        std::vector<int> near_runs = band_run_lengths(0.8f);
        std::vector<int> far_runs = band_run_lengths(6.0f);
        check("T3 GL: contour bands are visible at both a close and a far "
              "camera distance",
              !near_runs.empty() && !far_runs.empty(),
              "no contour-affected pixel run found at one of the two "
              "distances — the fixture may need a steeper height gradient");
        if (!near_runs.empty() && !far_runs.empty()) {
            double mn = mean_run(near_runs), mf = mean_run(far_runs);
            double ratio = mn > mf ? mn / mf : mf / mn;
            // A screen-space-constant band's mean run length should be
            // roughly camera-distance-independent; a world-space-fixed band
            // (the pre-55 bug) scales with the ~7.5x radius change used here.
            // 3x is generous headroom over discretization noise while
            // staying well short of what the old bug would produce.
            check("T3 GL: contour band width is roughly constant across a "
                  "7.5x camera-distance change",
                  ratio < 3.0,
                  "band width scaled with camera distance — looks world-"
                  "space-fixed, not screen-space-fixed (fwidth)");
        }
    }

    // --- 51 T1/T2/T3 (scene-focus-and-scale): the SUBJECT filter, in pixels --
    // The pure half of the filter is test_focus.cpp; this is the shader half
    // the brief asks the FBO smoke to carry — the parts only a real render can
    // answer: does the filter actually reach the draw, does GHOSTING keep ink
    // that DROPPING removes, and does a filter leave the plane's GEOMETRY
    // (its silhouette, i.e. its heights) untouched.
    {
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_threads()))),
            load(ndjson_threads()));
        space::TrajectorySet traj =
            space::build_trajectories(load(ndjson_threads()), terr.proj);
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);
        scene.set_zoning(terr.kind_by_cell, terr.w, terr.h);
        // The sky covers the whole frame, so switch it off here: with it off
        // the clear colour IS the background and "did a pixel lose all its
        // ink" is answerable directly.
        SceneLayers layers;
        layers.weather = false;

        scene.focus = scene3d::SceneFocus{}; // nothing focused
        const std::vector<unsigned char> plain = capture(scene, cam, cf, layers);

        // (1) focus_tid = -1 is BYTE-IDENTICAL to today: a default SceneFocus
        // must not perturb a single pixel of the pre-51 render.
        scene.focus = scene3d::SceneFocus{};
        const std::vector<unsigned char> again = capture(scene, cam, cf, layers);
        check("51 T1 GL: an unfocused scene is byte-identical to itself",
              pixels_differ(plain, again) == 0,
              "the default SceneFocus perturbed the render");

        // (2) focusing a real tid reaches the draw.
        scene3d::SceneFocus f;
        f.tid = 1;
        scene.focus = f;
        const std::vector<unsigned char> ghosted =
            capture(scene, cam, cf, layers);
        check("51 T1 GL: focusing a thread changes the render",
              pixels_differ(plain, ghosted) > 0,
              "focus.tid never reached the trajectory draw");

        // (3) GHOST, NEVER HIDE — the load-bearing one. Ghosting and dropping
        // are the same code path with a different alpha, so the honest way to
        // prove a ghost is not a hide is to render the hide and compare: a
        // worldline is drawn OVER the terrain (not over the background), so
        // the right measure is not "how much background is left" but "do the
        // two images differ at all". They must, on the very pixels the ghosted
        // line still occupies and the dropped one has surrendered to the
        // terrain underneath.
        scene3d::SceneFocus d = f;
        d.drop_unfocused = true;
        scene.focus = d;
        const std::vector<unsigned char> dropped =
            capture(scene, cam, cf, layers);
        check("51 T1 GL: a GHOSTED non-subject worldline still marks pixels a "
              "DROPPED one surrenders",
              pixels_differ(ghosted, dropped) > 0,
              "ghosting and dropping rendered identically — a ghost that is "
              "indistinguishable from a hide is a hide");
        check("51 T1 GL: dropping diverges from the unfiltered render at least "
              "as much as ghosting does",
              pixels_differ(plain, dropped) >= pixels_differ(plain, ghosted),
              "removing a line changed less than dimming it");
        scene.focus = scene3d::SceneFocus{};

        // (4) T2/T3: a kind filter is a PRESENTATION channel. Its render must
        // differ in colour but occupy EXACTLY the same silhouette — same
        // covered pixels, because height is the encoded quantity and a filter
        // may not touch it.
        auto silhouette = [&](const std::vector<unsigned char> &px) {
            unsigned char bg[3];
            mode_color(px, bg);
            std::vector<char> on(px.size() / 4, 0);
            for (size_t i = 0, k = 0; i + 3 < px.size(); i += 4, k++)
                on[k] = !(px[i] == bg[0] && px[i + 1] == bg[1] &&
                          px[i + 2] == bg[2]);
            return on;
        };
        scene3d::SceneFocus k;
        k.kind_mask = 0u; // every kind excluded: the strongest filter there is
        scene.focus = k;
        const std::vector<unsigned char> filtered =
            capture(scene, cam, cf, layers);
        check("51 T2 GL: a kind filter changes the plane's colour",
              pixels_differ(plain, filtered) > 0,
              "uKindMask never reached the terrain shader");
        check("51 T2/T3 GL: a kind filter leaves the plane's SILHOUETTE "
              "unchanged (it desaturates, it never re-heights)",
              silhouette(plain) == silhouette(filtered),
              "the filtered plane covers different pixels — a filter changed "
              "geometry, which would change the data");
        scene.focus = scene3d::SceneFocus{};

        // (5) T3: fidelity wins the LAST word. The torn fixture's red gash
        // must still be red with every kind filtered out — a filtered-out
        // torn cell may never read as a plain filtered cell.
        space::TerrainModel tt = space::build_terrain(
            space::build_projection(
                space::regions_from_codeimage(load(ndjson_torn()))),
            load(ndjson_torn()));
        scene.set_terrain(tt.full());
        scene.set_trajectories(space::build_trajectories(load(ndjson_torn())),
                               tt.proj);
        scene.set_zoning(tt.kind_by_cell, tt.w, tt.h);
        scene.focus = k; // every kind excluded
        const std::vector<unsigned char> torn_filtered =
            capture(scene, cam, cf, layers);
        std::vector<uint32_t> tids;
        scene.render_pick_buffer(cam, W, H, tids);
        const uint32_t torn_cell = cell_of(tt.proj, tt.w, 4194304);
        int tp = -1;
        for (size_t i = 0; i < tids.size(); i++)
            if (tids[i] == pick_id_cell(torn_cell)) {
                tp = static_cast<int>(i);
                break;
            }
        check("51 T3 GL: the torn cell is still on screen under a filter",
              tp >= 0, "the filtered plane lost the torn cell entirely");
        if (tp >= 0) {
            const unsigned char *p = &torn_filtered[tp * 4];
            check("51 T3 GL: a FILTERED torn cell is still red — fidelity "
                  "gets the last word on the pixel",
                  p[0] > p[1] + 40 && p[0] > p[2] + 40,
                  "the subject filter laundered a torn cell into a plain "
                  "desaturated one");
        }
        check("51 T3 GL: the pick pass ignores the filter entirely (a "
              "desaturated cell is exactly as clickable)",
              tp >= 0 && tids[static_cast<size_t>(tp)] ==
                             pick_id_cell(torn_cell),
              "a filter reached the pick pass");
        scene.focus = scene3d::SceneFocus{};
    }

    // ---- T1 (59-standalone-scenes): a SECOND SUBSTRATE renders and picks ---
    // The brief's own "done when": a second substrate can be rendered and
    // picked, and the plane path is unchanged. The image check is the round
    // trip — rendering another kind in between and coming back must reproduce
    // the plane's frame byte for byte, which is what proves a kind switch
    // disturbs no plane state (and that the shared program-link/pick-target
    // factoring left the plane's own GL objects alone).
    {
        StandaloneRenderer sr;
        std::string serr;
        const bool sok = sr.init_gl(&serr);
        check("T1 GL (59): the standalone renderer builds its programs", sok,
              serr.c_str());
        if (sok) {
            // A hand-built invocation stack: two slabs, the second missing the
            // middle block (a HOLE — not pickable, because nothing is there).
            InvocationScene inv;
            inv.blocks = {0x10, 0x20, 0x30};
            for (uint32_t k = 1; k <= 2; k++) {
                InvSlab sl;
                sl.number = k;
                sl.closed = k == 1;
                for (size_t j = 0; j < inv.blocks.size(); j++) {
                    InvCell c;
                    c.block = inv.blocks[j];
                    c.state = (k == 2 && j == 1) ? InvCellState::Absent
                                                 : InvCellState::Counted;
                    c.count = 1 + static_cast<uint32_t>(j);
                    if (c.state == InvCellState::Absent)
                        inv.holes++;
                    sl.cells.push_back(c);
                }
                inv.slabs.push_back(std::move(sl));
            }
            StandaloneFrame sf;
            sf.kind = SceneKind::Invocation;
            sf.invocation = &inv;
            sr.upload(sf);

            // The bands report what was REALLY uploaded, in the right band —
            // the ground truth a decode needs, never a re-derivation.
            const PickBands sb = sr.pick_bands();
            check("T1 GL (59): the upload reports its own kind",
                  sb.kind == SceneKind::Invocation,
                  "pick_bands() did not name the uploaded substrate");
            check("T1 GL (59): the upload reports its element count",
                  sb.nelem == invocation_pick_order(inv).size(),
                  "the uploaded element count does not match the pure pick "
                  "order — the GL encode and the pure decode disagree");

            const Camera scam =
                standalone_default_camera(SceneKind::Invocation);
            glBindFramebuffer(GL_FRAMEBUFFER, gcf.fbo);
            glViewport(0, 0, GW, GH);
            glClearColor(0.02f, 0.02f, 0.03f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            sr.render(scam, GW, GH);
            std::vector<unsigned char> spx(static_cast<size_t>(GW) * GH * 4);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glReadPixels(0, 0, GW, GH, GL_RGBA, GL_UNSIGNED_BYTE, spx.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            size_t slit = 0;
            for (size_t i = 0; i < spx.size(); i += 4)
                if (lum(&spx[i]) > 20.0f)
                    slit++;
            check("T1 GL (59): the standalone scene draws geometry", slit > 0,
                  "the second substrate rendered an empty frame");

            // ...and it PICKS: ids appear, and every read-back id decodes
            // inside its OWN band and never as a plane cell.
            std::vector<uint32_t> sids;
            sr.render_pick_buffer(scam, GW, GH, sids);
            size_t shits = 0, sforeign = 0;
            for (uint32_t id : sids) {
                if (id == 0)
                    continue;
                shits++;
                if (!decode_pick_standalone(id, sb))
                    sforeign++;
                if (decode_pick(id, 64).kind != Pick::None)
                    sforeign++;
            }
            check("T1 GL (59): the standalone scene is pickable", shits > 0,
                  "the pick pass wrote no ids");
            check("T1 GL (59): every id decodes inside its own band",
                  sforeign == 0,
                  "a standalone id decoded as another substrate's element");

            // The round trip.
            SceneLayers rl;
            const std::vector<unsigned char> before =
                capture(scene, gcam, gcf, rl);
            glBindFramebuffer(GL_FRAMEBUFFER, gcf.fbo);
            glViewport(0, 0, GW, GH);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            sr.render(scam, GW, GH); // the other substrate, in between
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const std::vector<unsigned char> after =
                capture(scene, gcam, gcf, rl);
            check("T1 GL (59): switching kinds and back reproduces the "
                  "original image",
                  pixels_differ(before, after) == 0,
                  "the plane image changed after a standalone scene rendered "
                  "in between — a kind switch disturbed plane state");
            // The plane's own pick bands are still the PLANE's: a foreign
            // render must not have rewritten them.
            check("T1 GL (59): the plane's bands still name the plane",
                  scene.pick_bands().kind == SceneKind::Plane,
                  "the plane's pick bands were relabelled by a kind switch");
            sr.shutdown();
        }
    }

    // --- (l) T6 (55) step 1: the portable line width ------------------------
    // The colour pass now widens every line in the vertex shader (kLineVert)
    // instead of asking glLineWidth for a width a core profile may refuse.
    // Three things have to hold, and none of them is "it still draws":
    //   1. the pick pass's two routes resolve the SAME overlay ids, so the
    //      quad route a core profile takes cannot change a drill-in;
    //   2. the quad route reproduces the width GL's own wide-line rasterizer
    //      produces for the same request — which is the only independent
    //      reference for "6 pixels" this lane has;
    //   3. the width is SCREEN-space: it does not scale with camera distance,
    //      which is exactly what a world-space expansion would do.
    {
        Recording rec = load(ndjson_threads());
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        space::ConvergenceSet conv =
            space::detect_convergences(traj, terr.proj);
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        // Flatten the terrain for this block ONLY. The arcs bow above the
        // plane, so with the usual relief they are mostly buried in it — and
        // every measurement below is of a rendered WIDTH, which an occluding
        // hill silently truncates. A flat plane leaves the whole arc visible,
        // so what is measured is the expansion and nothing else. Restored at
        // the end of the block.
        const float saved_y_scale = scene.y_scale;
        scene.y_scale = 0.0f;
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(conv, terr.proj);

        const scene3d::PickBands bands = scene.pick_bands();
        const uint32_t arc_id = pick_id_conv(terr.w, bands.npts, /*i=*/0);
        // Which route Auto takes HERE is a fact about this driver, not about
        // the code — print it so a failure on some other lane is readable.
        std::printf("test_scene_fbo: GL_ALIASED_LINE_WIDTH_RANGE = [%g, %g], "
                    "pick route = %s\n",
                    static_cast<double>(scene.aliased_line_width_range[0]),
                    static_cast<double>(scene.aliased_line_width_range[1]),
                    scene.pick_uses_quads() ? "quads" : "wide lines");

        auto pick_ids_via = [&](Scene::PickWidening route, const Camera &c) {
            scene.pick_widening = route;
            std::vector<uint32_t> ids;
            scene.render_pick_buffer(c, W, H, ids);
            scene.pick_widening = Scene::PickWidening::Auto;
            return ids;
        };
        // The width of a rendered band, measured as the mean length of its
        // contiguous runs along a scanline. That is a LOCAL measure — it is
        // the band's thickness divided by the sine of its screen angle — so
        // unlike a raw pixel count it does not move when part of the mark is
        // occluded by the terrain, which for these arcs is most of it.
        auto mean_run = [](const std::vector<bool> &hit) {
            double total = 0.0;
            int runs = 0, run = 0;
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    if (hit[static_cast<size_t>(y) * W + x]) {
                        run++;
                    } else if (run > 0) {
                        total += run;
                        runs++;
                        run = 0;
                    }
                }
                if (run > 0) {
                    total += run;
                    runs++;
                    run = 0;
                }
            }
            return runs > 0 ? total / runs : 0.0;
        };
        auto id_hits = [](const std::vector<uint32_t> &ids, uint32_t want) {
            std::vector<bool> hit(ids.size(), false);
            for (size_t i = 0; i < ids.size(); i++)
                hit[i] = (ids[i] == want);
            return hit;
        };

        std::vector<uint32_t> ids_lines =
            pick_ids_via(Scene::PickWidening::Lines, cam);
        std::vector<uint32_t> ids_quads =
            pick_ids_via(Scene::PickWidening::Quads, cam);

        // (1) the same OVERLAY ids, both ways. Terrain-cell ids at the very
        // margin of a widened overlay legitimately differ between the two
        // rasterizations (a wider mark covers one more cell, a narrower one
        // uncovers it), so the comparison is over the arc/spur bands — the
        // ids this change could actually have broken.
        auto overlay_ids = [&](const std::vector<uint32_t> &ids) {
            std::set<uint32_t> out;
            for (uint32_t id : ids) {
                if (!id)
                    continue;
                Pick d = decode_pick(id, terr.w, bands);
                if (d.kind == Pick::Conv || d.kind == Pick::Spur)
                    out.insert(id);
            }
            return out;
        };
        std::set<uint32_t> ov_lines = overlay_ids(ids_lines);
        std::set<uint32_t> ov_quads = overlay_ids(ids_quads);
        check("T6 GL: the fixture actually puts overlay ids in the pick "
              "buffer (test setup)",
              !ov_lines.empty(), "no arc/spur id was pickable at all");
        check("T6 GL: the quad pick route resolves exactly the same overlay "
              "ids as the wide-line route",
              ov_lines == ov_quads,
              "the quad-expanded click targets gained or lost a pickable "
              "arc/spur — a drill-in would land somewhere else on a core "
              "profile than it does here");

        // (2) and the same click-target WIDTH. GL's own wide-line rasterizer
        // is the only independent reference for "6 pixels" this lane has, so
        // the quad expansion is calibrated against it. The two are not
        // expected to agree exactly: GL offsets a wide line along the MINOR
        // AXIS by w/2, where the quad offsets along the true perpendicular,
        // so a diagonal quad is legitimately up to ~1/cos(45 deg) wider — and
        // the quad also carries square caps GL's line does not. Anything
        // outside this band is a real width error, not a rasterization
        // difference.
        double run_lines = mean_run(id_hits(ids_lines, arc_id));
        double run_quads = mean_run(id_hits(ids_quads, arc_id));
        check("T6 GL: the convergence arc is pickable on BOTH routes",
              run_lines > 0.0 && run_quads > 0.0,
              "one of the two click-target routes drew no arc at all");
        if (run_lines > 0.0 && run_quads > 0.0) {
            const double ratio = run_quads / run_lines;
            check("T6 GL: the quad route reproduces the wide-line route's "
                  "click-target width",
                  ratio > 0.7 && ratio < 2.0,
                  "the quad-expanded 6px arc is a very different width from "
                  "GL's own 6px line — the screen-space width is wrong, not "
                  "merely differently rasterized");
        }

        // ...and the width the code asks for is the width it gets: the SAME
        // arc geometry is drawn by the colour pass at kConvWidthPx (3) and by
        // the pick pass's quad route at kPickConvWidthPx (6), through the same
        // expansion, under the same camera and the same terrain occlusion. Its
        // rendered thickness must double. This is the brief's own "a line
        // drawn at width 3 covers ~3x the pixels of one at width 1", in the
        // form this scene can state without inventing a knob that exists only
        // for the test.
        {
            SceneLayers arcs_only;
            arcs_only.terrain = true; // depth: the arc must occlude the same
            arcs_only.exact = arcs_only.statistical = false;
            arcs_only.access_marks = arcs_only.vehicle = false;
            arcs_only.canopy = arcs_only.mispred = false;
            arcs_only.weather = false; // the sky would fill the background
            arcs_only.edl = false;     // EDL darkens; the hue test is exact
            // T2's depth cue attenuates a far line's width; this measurement
            // is of the NOMINAL width, so it is taken with that off.
            arcs_only.halos = false;
            std::vector<unsigned char> px = capture(scene, cam, cf, arcs_only);
            std::vector<bool> magenta(static_cast<size_t>(W) * H, false);
            for (size_t i = 0; i + 3 < px.size(); i += 4)
                magenta[i / 4] = px[i] > 178 && px[i + 2] > 153 &&
                                 px[i + 1] < 127; // the arc's own colour
            const double run_colour = mean_run(magenta);
            check("T6 GL: the colour pass draws the arc (test setup)",
                  run_colour > 0.0, "no arc pixel in the colour render");
            if (run_colour > 0.0 && run_quads > 0.0) {
                const double ratio = run_quads / run_colour;
                check("T6 GL: doubling the requested width doubles the "
                      "rendered width",
                      ratio > 1.4 && ratio < 2.8,
                      "the same arc drawn at 6px and at 3px did not come out "
                      "twice as thick — the width uniform is not reaching the "
                      "geometry in pixels");
            }
        }

        // The drill-in itself: a pixel that picked the arc still picks the
        // arc, and picks the SAME id, on the other route.
        int arc_px = -1;
        for (size_t i = 0; i < ids_lines.size(); i++)
            if (ids_lines[i] == arc_id) {
                arc_px = static_cast<int>(i);
                break;
            }
        if (arc_px >= 0) {
            const int px_x = arc_px % W;
            const int y_top = H - 1 - arc_px / W;
            scene.pick_widening = Scene::PickWidening::Quads;
            uint32_t got_q = scene.pick(cam, W, H, px_x, y_top);
            scene.pick_widening = Scene::PickWidening::Lines;
            uint32_t got_l = scene.pick(cam, W, H, px_x, y_top);
            scene.pick_widening = Scene::PickWidening::Auto;
            check("T6 GL: a click on the arc resolves to the same id on both "
                  "widening routes",
                  got_l == arc_id && got_q == arc_id,
                  "the same screen pixel resolved to two different ids");
        }

        // (3) screen-space, not world-space. Measure the arc band's scanline
        // run lengths in the PICK buffer (exact per-pixel membership, no
        // colour threshold) at two camera distances 3x apart: a width in
        // PIXELS is distance-invariant, a width baked in world units is not.
        auto run_at = [&](float radius) {
            Camera c;
            c.frame(
                0.5f, 0.5f,
                Camera::clampf(radius, Camera::kMinRadius, Camera::kMaxRadius));
            return mean_run(
                id_hits(pick_ids_via(Scene::PickWidening::Quads, c), arc_id));
        };
        double near_w = run_at(1.2f), far_w = run_at(3.6f);
        check("T6 GL: the arc is on screen at both camera distances (test "
              "setup)",
              near_w > 0.0 && far_w > 0.0,
              "the arc left the frame at one of the two radii");
        if (near_w > 0.0 && far_w > 0.0) {
            double ratio = near_w > far_w ? near_w / far_w : far_w / near_w;
            check("T6 GL: quad-expanded line width is constant in SCREEN "
                  "pixels across a 3x camera-distance change",
                  ratio < 2.0,
                  "the band thickened as the camera dollied in — the width "
                  "is being applied in world units, not pixels");
        }
        scene.y_scale = saved_y_scale;
    }

    // --- (m) T2 (55): depth-dependent halos --------------------------------
    // Everts, Bekker, Roerdink & Isenberg (IEEE TVCG 15(6) 2009). Each line is
    // drawn as a slightly wider quad whose OUTER RING is the background colour,
    // so a nearer line visibly CUTS the ones behind it. What has to hold: the
    // ring exists and is opaque at the line's own depth (that IS the cut), a
    // mark is never eaten by its own halo, the attenuation is bounded, and —
    // the fidelity bar, checked in (n) below — a statistical mark gets no more
    // solid.
    {
        Recording rec = load(ndjson_threads());
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        space::ConvergenceSet conv =
            space::detect_convergences(traj, terr.proj);
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(conv, terr.proj);

        // Confounds off: no sky (it would fill the background the halo is
        // coloured after) and no EDL (a second pass answering the same
        // "what is in front" question, with its own screen-space radius).
        SceneLayers base;
        base.weather = false;
        base.edl = false;
        base.vehicle = false;

        auto frame = [&](bool halos, bool conv_on) {
            SceneLayers l = base;
            l.halos = halos;
            l.convergence = conv_on;
            return capture(scene, cam, cf, l);
        };
        auto arc_only = [&](bool halos) {
            SceneLayers off = base;
            off.halos = halos;
            off.convergence = false;
            return pixels_differ(frame(halos, true),
                                 capture(scene, cam, cf, off));
        };

        // (1) the ring exists, and it is OPAQUE at the mark's own depth — which
        // is precisely what makes a nearer mark cut a farther one, since every
        // pixel of that wider footprint now wins the z-test against anything
        // behind it. Measured as the pixels the convergence layer OWNS (its
        // toggle changes them), which grows by the ring on every side.
        const size_t owned_plain = arc_only(false);
        const size_t owned_halo = arc_only(true);
        check("T2 GL: the convergence arc is on screen at all (test setup)",
              owned_plain > 0, "the arc layer owned no pixel");
        check("T2 GL: a halo widens the footprint a mark occludes with",
              owned_halo > owned_plain,
              "turning halos on did not widen what the arc covers — the ring "
              "is missing, or is not opaque, so nothing behind it is cut");

        // (2) ...and a mark is never eaten by its OWN halo. The ring is the
        // outer part of the SAME quad, so there is no depth relationship to
        // get backwards — but a regression that reintroduced a separate,
        // wider halo primitive would show up here immediately (measured: a
        // 16-segment arc lost 78% of itself to exactly that).
        auto magenta = [](const std::vector<unsigned char> &px) {
            size_t n = 0;
            for (size_t i = 0; i + 3 < px.size(); i += 4)
                if (px[i] > 178 && px[i + 2] > 153 && px[i + 1] < 127)
                    n++;
            return n;
        };
        const size_t arc_plain = magenta(frame(false, true));
        const size_t arc_halo = magenta(frame(true, true));
        check("T2 GL: the halo does not swallow the mark it belongs to",
              arc_halo > 0 && arc_halo * 4 >= arc_plain * 3,
              "the convergence arc lost a quarter or more of itself once halos "
              "turned on — its own halo is cutting it");

        // (3) the layer toggle owns pixels: off means the pass does not run,
        // never that it runs and does nothing.
        check("T2 GL: the halos layer toggle owns pixels",
              pixels_differ(frame(true, true), frame(false, true)) > 0,
              "turning halos off changed nothing at all");

        // (4) the depth-cued width attenuation is BOUNDED. At the far end of
        // the dolly range a line must still be on screen: a mark thinned out
        // of existence is an unknown rendered as an absence (invariant 3), and
        // kMinLineWidthPx is the floor that prevents it.
        {
            Camera far_cam;
            far_cam.frame(0.5f, 0.5f, Camera::kMaxRadius);
            SceneLayers l = base;
            l.halos = true;
            SceneLayers no_lines = l;
            no_lines.exact = no_lines.statistical = no_lines.convergence =
                no_lines.access_marks = false;
            check("T2 GL: a depth-attenuated line never thins away to nothing",
                  pixels_differ(capture(scene, far_cam, cf, l),
                                capture(scene, far_cam, cf, no_lines)) > 0,
                  "at maximum camera distance the line sets drew no pixel at "
                  "all — the attenuation is unbounded below");
        }
    }

    // --- (m2) T2 (55): a worldline past the playhead still dims, with halos --
    // The companion the T1 (49) dim/discard case defers to: that case measures
    // with halos off (the execution-front glyph moves with the playhead, and
    // the chrome it uncovers is background-coloured, which its luminance rule
    // cannot tell from a discarded worldline). This says the property itself
    // survives the halo.
    {
        Recording rec = load(ndjson_hotcold());
        space::TerrainModel terr = space::build_terrain(
            space::build_projection(space::regions_from_codeimage(rec)), rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        scene.nsteps = static_cast<uint32_t>(terr.nsteps);
        scene.set_terrain(terr.full());
        scene.set_trajectories(traj, terr.proj);
        scene.set_convergences(space::ConvergenceSet{}, terr.proj);
        scene.follow_step = 999999; // no vehicle glyph in this frame

        SceneLayers l; // defaults, halos ON
        const uint64_t saved_slice = scene.slice_step;
        scene.slice_step = terr.nsteps;
        std::vector<unsigned char> at_end = capture(scene, cam, cf, l);
        scene.slice_step = terr.nsteps / 2;
        std::vector<unsigned char> at_half = capture(scene, cam, cf, l);
        size_t dimmed = 0;
        for (size_t i = 0; i + 3 < at_end.size(); i += 4) {
            const float le = lum(&at_end[i]), lh = lum(&at_half[i]);
            if (le > 40.0f && lh < le - 5.0f && lh > 8.0f)
                dimmed++;
        }
        check("T2 GL: with halos on, a worldline past the playhead still DIMS "
              "rather than disappearing",
              dimmed > 0,
              "no pixel dimmed and stayed visible — the halo may be burying "
              "the clipped tail instead of ringing it");
        scene.slice_step = saved_slice;
    }

    // --- (n) T2 (55) fidelity: a statistical mark gets no more solid --------
    // A STATISTICAL line's halo is the outer ring of the same quad, and the
    // stipple `discard` is evaluated BEFORE the ring branch on gl_FragCoord —
    // so the ring inherits exactly the line's own screen-space IGN mask. Its
    // gaps stay gaps and still show what is behind them. An opaque ring would
    // instead fill them with background colour and launder a sampled survey
    // into a solid, exact-looking path. This re-runs case (g)'s own
    // IGN-agreement measurement with halos ON: every pixel the statistical
    // layer owns — line AND ring — must still obey the mask.
    {
        SceneModel m = build_scene(
            load_path(ASMTEST_FIXTURE_DIR, "obs-survey-ibs.asmtrace"),
            {survey_window()});
        upload(scene, m, m.terr.stat);

        SceneLayers on;
        on.edl = false; // as in (g): EDL shades neighbours, not the mark
        on.halos = true;
        SceneLayers off = on;
        off.statistical = false;
        std::vector<unsigned char> px_on = capture(scene, gcam, gcf, on);
        std::vector<unsigned char> px_off = capture(scene, gcam, gcf, off);

        std::vector<size_t> marked;
        for (size_t i = 0, p = 0; i < px_on.size(); i += 4, p++)
            if (px_on[i] != px_off[i] || px_on[i + 1] != px_off[i + 1] ||
                px_on[i + 2] != px_off[i + 2])
                marked.push_back(p);

        constexpr float kStatTrajAlpha = 0.45f; // set_trajectories' own literal
        int agree = 0;
        for (size_t p : marked) {
            const float x =
                static_cast<float>(p % static_cast<size_t>(GW)) + 0.5f;
            const float y =
                static_cast<float>(p / static_cast<size_t>(GW)) + 0.5f;
            if (ign_cpu(x, y) <= kStatTrajAlpha)
                agree++;
        }
        const double agree_frac = marked.empty()
                                      ? 0.0
                                      : static_cast<double>(agree) /
                                            static_cast<double>(marked.size());
        check("T2 GL: with halos on, the statistical mark is still governed by "
              "the SAME stipple mask",
              !marked.empty() && agree_frac > 0.9,
              "pixels the statistical layer owns stopped matching the shader's "
              "own IGN discard formula once halos turned on — the ring is "
              "filling the stipple's gaps, which launders a survey into an "
              "exact path");
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
