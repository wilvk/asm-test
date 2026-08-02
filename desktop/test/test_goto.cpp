// test_goto.cpp — the "go there" resolvers of 48-scene-navigation-and-goto.md
// (scene3d/goto.h): scene_recentre_target (T2), scene_goto_addr / scene_goto_region
// (T3), scene_home_target (T4). Null harness, no display: this binary links
// goto.o + pick.o (for Pick/decode_pick — the id space only, no GL) + the pure
// space/ models + the doc model, and NOTHING else — the same engine-free closure
// argument test_projection/test_drillin make, now for the camera-navigation
// resolvers.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "scene3d/camera.h"
#include "scene3d/goto.h"
#include "scene3d/pick.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"

using namespace asmdesk;
using namespace asmdesk::scene3d;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

// Build a Recording from an in-memory NDJSON string, through the real loader —
// the same pattern test_terrain.cpp/test_drillin.cpp's mk_rec uses.
static Recording mk_rec(const std::string &ndjson) {
    std::istringstream in(ndjson);
    std::string err;
    auto rec = load_recording(in, err);
    if (!rec) {
        fail("load recording", err);
        return Recording{};
    }
    return *rec;
}

// The plane cell an address projects into — the SAME rounding terrain.cpp uses.
static uint32_t cell_at(const space::Projection &p, uint64_t addr, bool *ok) {
    float u = 0, v = 0;
    if (!p.project(addr, &u, &v)) {
        *ok = false;
        return 0;
    }
    uint32_t n = uint32_t{1} << p.order;
    uint32_t x = static_cast<uint32_t>(u * n);
    uint32_t y = static_cast<uint32_t>(v * n);
    if (x >= n)
        x = n - 1;
    if (y >= n)
        y = n - 1;
    *ok = true;
    return y * n + x;
}

static Pick cell_pick(uint32_t cell, uint32_t n) {
    return decode_pick(pick_id_cell(cell), n);
}
static Pick vertex_pick(uint64_t vindex, uint32_t n) {
    return decode_pick(pick_id_vertex(n, vindex), n);
}

static space::Region region(uint64_t base, uint64_t len,
                           space::Region::Kind k) {
    space::Region r;
    r.base = base;
    r.len = len;
    r.kind = k;
    return r;
}

static const char *kHdrExact =
    "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\",\"exact\":"
    "true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n";

int main() {
    // === 48 T2/T3: scene_recentre_target / scene_goto_addr =====================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;

        // --- Cell pick: matches the SAME cell-centre rounding resolve_pick uses.
        bool ok = false;
        uint32_t c = cell_at(terr.proj, 4194304, &ok);
        float u = -1.0f, v = -1.0f;
        check("recentre/cell: resolves",
              scene_recentre_target(terr, traj, cell_pick(c, n), &u, &v),
              "cell recentre failed");
        {
            uint32_t x = c % n, y = c / n;
            float eu = (x + 0.5f) / static_cast<float>(n);
            float ev = (y + 0.5f) / static_cast<float>(n);
            check("recentre/cell: matches cell-centre rounding",
                  u == eu && v == ev, "u/v mismatch");
        }

        // --- Padding cell: the domain (256B) is a sliver of the 64x64 plane
        // (4096 cells), so the plane's LAST Hilbert cell is unmapped padding.
        u = -1.0f;
        v = -1.0f;
        check("recentre/padding: refuses",
              !scene_recentre_target(terr, traj, cell_pick(n * n - 1, n), &u,
                                     &v),
              "recentre must refuse on padding, never jump to (0,0)");

        // --- Vertex pick: recentres on the PLACED ADDRESS's cell, matching
        // Projection::project directly (never the vertex's world-Y).
        u = -1.0f;
        v = -1.0f;
        check("recentre/vertex: resolves",
              scene_recentre_target(terr, traj, vertex_pick(0, n), &u, &v),
              "vertex recentre failed");
        {
            float pu = 0.0f, pv = 0.0f;
            check("recentre/vertex: matches the vertex's projected address",
                  terr.proj.project(4194304, &pu, &pv) && u == pu && v == pv,
                  "vertex recentre landed on the wrong cell");
        }

        // --- Pick::None: refuses.
        Pick none;
        u = -1.0f;
        v = -1.0f;
        check("recentre/none: refuses",
              !scene_recentre_target(terr, traj, none, &u, &v),
              "Pick::None must refuse");

        // --- scene_goto_addr: a mapped address matches Projection::project;
        // an unmapped one refuses and writes neither u nor v.
        u = -2.0f;
        v = -2.0f;
        check("goto_addr: a mapped address resolves",
              scene_goto_addr(terr.proj, 4194304, &u, &v), "goto_addr failed");
        {
            float pu = 0.0f, pv = 0.0f;
            check("goto_addr: matches Projection::project",
                  terr.proj.project(4194304, &pu, &pv) && u == pu && v == pv,
                  "goto_addr disagreed with project()");
        }
        u = -3.0f;
        v = -3.0f;
        check("goto_addr: an unmapped address refuses",
              !scene_goto_addr(terr.proj, 0xDEADBEEF00ull, &u, &v),
              "goto_addr must refuse an address no region maps");
        check("goto_addr: refusal writes no u/v", u == -3.0f && v == -3.0f,
              "an unmapped goto must never clamp to the nearest cell");
    }

    // === 48 T3: scene_goto_region frames a region's OWN footprint ==============
    {
        space::Projection p = space::build_projection(
            {region(0x400000, 0x1000, space::Region::Code),
             region(0x800000, 0x2000, space::Region::Data)});
        float u = 0.0f, v = 0.0f, radius = 0.0f;
        check("goto_region: region 0 resolves",
              scene_goto_region(p, 0, &u, &v, &radius), "region 0 failed");
        float u1 = 0.0f, v1 = 0.0f, radius1 = 0.0f;
        check("goto_region: region 1 resolves",
              scene_goto_region(p, 1, &u1, &v1, &radius1), "region 1 failed");
        check("goto_region: distinct regions frame distinct centres",
              u != u1 || v != v1,
              "two disjoint regions framed the same centre");
        check("goto_region: radius stays within the camera's working range",
              radius >= Camera::kMinRadius && radius <= Camera::kMaxRadius,
              "radius heuristic escaped Camera's own clamp range");
        check("goto_region: an out-of-range index refuses",
              !scene_goto_region(p, 99, &u, &v, &radius),
              "must refuse an out-of-range region_index");

        space::Projection pz = space::build_projection(
            {region(0x400000, 0, space::Region::Code)});
        check("goto_region: a zero-length region refuses",
              !scene_goto_region(pz, 0, &u, &v, &radius),
              "a zero-length region has nothing to frame");
    }

    // === 48 T4: scene_home_target — the code-district centroid =================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        float hu = -1.0f, hv = -1.0f;
        check("home: a code-only recording resolves",
              scene_home_target(terr, &hu, &hv), "scene_home_target failed");
        check("home: the centroid sits inside the plane [0,1]^2",
              hu >= 0.0f && hu <= 1.0f && hv >= 0.0f && hv <= 1.0f,
              "home target escaped the plane");

        space::TerrainModel empty_terr; // default: no regions at all
        check("home: no regions refuses (caller keeps its default camera)",
              !scene_home_target(empty_terr, &hu, &hv),
              "an empty projection must refuse, not fabricate a centroid");

        // Byte-stable across two build_terrain calls on the same recording.
        space::Projection p2 =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr2 = space::build_terrain(p2, rec);
        float hu2 = 0.0f, hv2 = 0.0f;
        check("home: resolves the same way a second time",
              scene_home_target(terr2, &hu2, &hv2), "second call failed");
        check("home: byte-stable across two build_terrain calls",
              hu == hu2 && hv == hv2,
              "scene_home_target must be deterministic for the same recording");
    }

    // === 50 T4 (two-way-brushing): scene_on_screen ============================
    {
        Camera cam; // reset() default: three-quarter view of the plane centre
        std::string edge;

        // The camera's own target is, by construction, on-screen at its centre.
        check("on_screen/target: the camera's own target is on-screen",
              scene_on_screen(cam, cam.target[0], cam.target[2], 1.6f, &edge),
              "edge=" + edge);

        // A point far outside the plane's [0,1]^2 domain, well past the
        // camera's frustum at this radius, must read as off-screen with a
        // named edge — never silently "on" nor a crash.
        edge.clear();
        check("on_screen/far: a distant point is off-screen",
              !scene_on_screen(cam, 50.0f, 50.0f, 1.6f, &edge),
              "a point far outside the frustum was reported on-screen");
        check("on_screen/far: names a real edge",
              edge == "left" || edge == "right" || edge == "top" ||
                  edge == "bottom" || edge == "behind",
              "got: '" + edge + "'");

        // A degenerate/zero aspect must not crash (Camera::proj's own guard
        // substitutes 1.0); reaching the next check is the assertion.
        scene_on_screen(cam, cam.target[0], cam.target[2], 0.0f);
    }

    if (failures) {
        std::fprintf(stderr, "%d goto check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_goto: all checks passed\n");
    return 0;
}
