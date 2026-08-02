// test_scene2d.cpp — the flat terrain surface's pure model
// (docs/internal/gui/52-flat-terrain-surface.md T1/T2/T3). Null harness, no
// display: links space/terrain.o + space/trajectory.o + space/projection.o +
// scene3d/pick.o (T3's resolve_pick parity proof) + the document model and
// NOTHING else — no ImGui, no GL, no engine (D4), the same engine-free
// closure test_terrain.cpp/test_drillin.cpp already prove for their own
// pieces.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "nav.h"
#include "scene3d/pick.h"
#include "space/converge.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"
#include "views/scene2d.h"

using namespace asmdesk;
using namespace asmdesk::space;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

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

// The plane cell an address projects into — the SAME rounding test_terrain.cpp
// and terrain.cpp itself use, so an expected cell and the builder's agree.
static uint32_t cell_at(const Projection &p, uint64_t addr, bool *ok) {
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

static const uint64_t A0 = 0x400000; // region 1
static const uint64_t A1 = 0x400010; // region 1
static const uint64_t B0 = 0x500000; // region 2

// A small two-region fixture, mirroring test_terrain.cpp's Fixture A, reused
// across the plan/pick tests below so every test shares one known geometry.
static Recording two_region_rec() {
    return mk_rec(
        "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
        "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
        "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
        "\"when\":1,\"bytes\":\"90\"}\n"
        "{\"k\":\"codeimage\",\"base\":5242880,\"len\":256,\"version\":0,"
        "\"when\":1,\"bytes\":\"90\"}\n"
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // A0 step0
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n" // A1 step1
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":5242880}\n" // B0 step2
        "{\"k\":\"end\",\"events\":5,\"truncated\":false,"
        "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
}

int main() {
    // === T1: cell_paint mirrors kTerrainFrag's branch chain =================
    {
        // Every TerrainFlag combination, plus both sentinel kinds (Region::Code
        // as an ordinary in-range kind, kKindByCellNone as the off-domain /
        // unclassified clamp target) — dumped as a table so any future
        // divergence between this function and the shader is a diff, not a
        // discovery.
        struct Case {
            const char *name;
            float height;
            uint32_t flags;
            uint8_t kind;
        };
        auto k = [](Region::Kind kind) { return static_cast<uint8_t>(kind); };
        const Case cases[] = {
            {"exact, mid height, Code", 0.5f, 0u, k(Region::Code)},
            {"exact, zero height, Stack", 0.0f, 0u, k(Region::Stack)},
            {"churn only", 0.5f, TF_CHURN, k(Region::Data)},
            {"stat only", 0.5f, TF_STAT, k(Region::Heap)},
            {"unknown only", 0.0f, TF_UNKNOWN, k(Region::Mmap)},
            {"torn only", 0.8f, TF_TORN, k(Region::Code)},
            {"torn+unknown+stat+churn", 0.9f,
             TF_TORN | TF_UNKNOWN | TF_STAT | TF_CHURN, k(Region::Code)},
            {"off-domain clamp", 0.0f, 0u, kKindByCellNone},
        };
        for (const Case &c : cases) {
            CellPaint p = cell_paint(c.height, c.flags, c.kind);
            check(std::string("cell_paint rgba finite: ") + c.name,
                  p.r >= 0.0f && p.r <= 1.0f && p.g >= 0.0f && p.g <= 1.0f &&
                      p.b >= 0.0f && p.b <= 1.0f,
                  "colour channel out of [0,1]");
            const bool want_hatched = (c.flags & (TF_TORN | TF_UNKNOWN)) != 0u;
            check(std::string("hatched matches torn|unknown: ") + c.name,
                  p.hatched == want_hatched,
                  p.hatched ? "hatched but neither flag set"
                            : "not hatched but torn/unknown set");
            check(std::string("why is never empty: ") + c.name,
                  p.why != nullptr && p.why[0] != '\0', "why is empty");
        }
        // The branch-order bar, stated as pairwise dominance checks (T1's own
        // acceptance: "its branch order matches the shader's"): torn reads
        // as torn even under every other flag; an unknown-but-not-torn cell
        // reads as unknown even under stat+churn.
        CellPaint torn_all = cell_paint(
            0.9f, TF_TORN | TF_UNKNOWN | TF_STAT | TF_CHURN, Region::Code);
        check("torn dominates the combined why()",
              std::string(torn_all.why).find("torn") == 0, torn_all.why);
        CellPaint unknown_stat_churn =
            cell_paint(0.5f, TF_UNKNOWN | TF_STAT | TF_CHURN, Region::Code);
        check("unknown dominates over stat+churn in why()",
              std::string(unknown_stat_churn.why).find("fog-of-war") == 0,
              unknown_stat_churn.why);
        // Height still modulates colour even with no flags at all: a taller
        // exact cell reads brighter toward its kind hue than a flat one.
        CellPaint low = cell_paint(0.0f, 0u, Region::Code);
        CellPaint high = cell_paint(1.0f, 0u, Region::Code);
        check("height brightens the exact cell toward its kind hue",
              (high.r + high.g + high.b) > (low.r + low.g + low.b),
              "a taller cell must not be darker or equal");
    }

    // === T2: down-sampling never drops a fidelity flag, and states its
    //     factor honestly; padding is counted, not silently absorbed. A
    //     hand-built 4x4 plane, so the fold is asserted against KNOWN cells
    //     rather than an opaque real projection. ===============================
    {
        TerrainModel terr;
        terr.w = terr.h = 4;
        terr.kind_by_cell.assign(16, static_cast<uint8_t>(Region::Code));
        terr.kind_by_cell[5] = kKindByCellNone; // cell (1,1): off-domain

        Terrain slice;
        slice.w = slice.h = 4;
        slice.height.assign(16, 0.0f);
        slice.flags.assign(16, 0u);
        slice.height[0] = 1.0f;   // cell (0,0): tall, no flags
        slice.flags[3] = TF_TORN; // cell (3,0): torn, otherwise quiet
        slice.height[3] = 0.2f;

        TrajectorySet traj;
        ConvergenceSet conv;

        // max_blocks_per_edge=1 forces the whole 4x4 plane into ONE block, so
        // the torn cell's flag and the off-domain cell's padding must both
        // survive the fold into that single block.
        Scene2dPlan plan = build_scene2d_plan(terr, slice, traj, conv,
                                              /*playhead_t=*/0,
                                              /*max_blocks_per_edge=*/1);
        check("plan has a plane", plan.has_plane,
              "4x4 terrain must have a plane");
        check("single block at downsample 4",
              plan.grid_w == 1 && plan.grid_h == 1 && plan.downsample == 4,
              "grid " + std::to_string(plan.grid_w) + "x" +
                  std::to_string(plan.grid_h) +
                  " ds=" + std::to_string(plan.downsample));
        check("exactly one block emitted", plan.blocks.size() == 1,
              std::to_string(plan.blocks.size()));
        if (plan.blocks.size() == 1) {
            const Scene2dBlock &b = plan.blocks[0];
            check("down-sampling never drops TORN (hatched)", b.paint.hatched,
                  "the block's OR'd flags lost TF_TORN");
            check("down-sampling never drops TORN (why)",
                  std::string(b.paint.why).find("torn") == 0, b.paint.why);
            check("total_cells covers the whole 4x4 plane", b.total_cells == 16,
                  std::to_string(b.total_cells));
            check("exactly one off-domain source cell counted",
                  b.off_domain_cells == 1, std::to_string(b.off_domain_cells));
            check("a straddling block is not counted as wholly padding",
                  b.off_domain_cells < b.total_cells, "off_domain == total");
        }

        // No down-sampling (a budget that fits the plane exactly): every
        // source cell gets its own block, 1:1.
        Scene2dPlan plan1 = build_scene2d_plan(terr, slice, traj, conv, 0, 4);
        check("downsample 1 when the budget already fits",
              plan1.downsample == 1 && plan1.grid_w == 4 && plan1.grid_h == 4,
              "ds=" + std::to_string(plan1.downsample));
        if (plan1.blocks.size() == 16) {
            check("the off-domain cell is wholly padding at downsample 1",
                  plan1.blocks[5].off_domain_cells == 1 &&
                      plan1.blocks[5].total_cells == 1,
                  "cell 5 must be exactly one wholly-padding block");
        }
    }

    // === T2 steps 2-4: trajectory breaks + playhead grading, against a real
    //     projection so placed points land at the addresses they claim. =======
    {
        Recording rec = two_region_rec();
        std::vector<Region> regs = regions_from_codeimage(rec);
        Projection proj = build_projection(regs);
        TerrainModel terr = build_terrain(proj, rec);
        Terrain slice = terr.full();

        bool ok = false;
        float ua0 = 0, va0 = 0, ua1 = 0, va1 = 0, ub0 = 0, vb0 = 0;
        check("A0 projects", terr.proj.project(A0, &ua0, &va0), "A0");
        check("A1 projects", terr.proj.project(A1, &ua1, &va1), "A1");
        check("B0 projects", terr.proj.project(B0, &ub0, &vb0), "B0");
        (void)ok;

        TrajectorySet traj;
        Trajectory tr;
        tr.tid = 0;
        TrajPoint p0;
        p0.t = 0;
        p0.addr = A0;
        p0.placed = true;
        TrajPoint p1; // unreachable: not placed, must break the strip
        p1.t = 1;
        p1.addr = 0;
        p1.placed = false;
        TrajPoint p2;
        p2.t = 2;
        p2.addr = A1;
        p2.placed = true;
        TrajPoint p3; // past the playhead below (dim, never dropped)
        p3.t = 10;
        p3.addr = B0;
        p3.placed = true;
        tr.points = {p0, p1, p2, p3};
        traj.trajectories = {tr};
        ConvergenceSet conv;

        Scene2dPlan plan =
            build_scene2d_plan(terr, slice, traj, conv, /*playhead_t=*/5, 64);
        check("one path emitted", plan.paths.size() == 1,
              std::to_string(plan.paths.size()));
        if (plan.paths.size() == 1) {
            const Scene2dPath &path = plan.paths[0];
            check("all four points carried (even the unplaced one)",
                  path.points.size() == 4, std::to_string(path.points.size()));
            if (path.points.size() == 4) {
                check("point 0 placed and at A0's projection",
                      path.points[0].placed &&
                          std::fabs(path.points[0].u - ua0) < 1e-4f &&
                          std::fabs(path.points[0].v - va0) < 1e-4f,
                      "point 0 mislocated or unplaced");
                check("point 1 (the unreachable one) is unplaced",
                      !path.points[1].placed, "must break the strip here");
                check("point 2 placed and at A1's projection",
                      path.points[2].placed &&
                          std::fabs(path.points[2].u - ua1) < 1e-4f &&
                          std::fabs(path.points[2].v - va1) < 1e-4f,
                      "point 2 mislocated or unplaced");
                check("point 0 (t=0) is not past the t=5 playhead",
                      !path.points[0].past_playhead,
                      "t=0 must not be past t=5");
                check("point 3 (t=10) is past the t=5 playhead — dimmed, not "
                      "dropped",
                      path.points[3].placed && path.points[3].past_playhead,
                      "t=10 must be placed AND graded past the playhead");
            }
        }
    }

    // === T3: the flat pick inversion agrees with a hand-computed cell, and
    //     that cell resolves through the SAME resolve_pick the 3D pane calls —
    //     "a flat pick and a 3D pick of the same cell produce identical
    //     links." ==============================================================
    {
        Recording rec = two_region_rec();
        std::vector<Region> regs = regions_from_codeimage(rec);
        Projection proj = build_projection(regs);
        TerrainModel terr = build_terrain(proj, rec);
        Terrain slice = terr.full();
        TrajectorySet traj; // no trajectories needed for a Cell pick
        ConvergenceSet conv;

        const uint32_t n = uint32_t{1} << terr.proj.order;
        // A budget that comfortably exceeds the plane's own side: no
        // down-sampling, so a block IS a cell and the inversion is exact.
        Scene2dPlan plan =
            build_scene2d_plan(terr, slice, traj, conv, 0, n * 2);
        check("no down-sampling at a generous budget", plan.downsample == 1,
              std::to_string(plan.downsample));

        bool ok = false;
        const uint32_t expect_cell = cell_at(terr.proj, A0, &ok);
        check("A0 has a cell", ok, "A0 must project");
        if (ok && plan.downsample == 1 && plan.w > 0) {
            const uint32_t x = expect_cell % plan.w, y = expect_cell / plan.w;
            // Click the CENTRE of that cell's on-screen rect.
            const float fx = (x + 0.5f) / static_cast<float>(plan.grid_w);
            const float fy = (y + 0.5f) / static_cast<float>(plan.grid_h);
            uint32_t got_cell = 0;
            check("scene2d_pick_cell inverts the layout back to A0's cell",
                  scene2d_pick_cell(plan, fx, fy, &got_cell) &&
                      got_cell == expect_cell,
                  "got " + std::to_string(got_cell) + " want " +
                      std::to_string(expect_cell));

            scene3d::Pick flat_pick;
            flat_pick.kind = scene3d::Pick::Cell;
            flat_pick.cell = got_cell;
            scene3d::Pick threed_pick; // the "3D style" pick: same cell, built
            threed_pick.kind = scene3d::Pick::Cell; // independently of scene2d
            threed_pick.cell = expect_cell;

            std::optional<dt_link> flat_link = scene3d::resolve_pick(
                terr, traj, "rec.asmtrace", flat_pick, conv);
            std::optional<dt_link> threed_link = scene3d::resolve_pick(
                terr, traj, "rec.asmtrace", threed_pick, conv);
            check("both picks resolve",
                  flat_link.has_value() && threed_link.has_value(),
                  "a code cell must resolve to a link");
            if (flat_link && threed_link) {
                check("identical view", flat_link->view == threed_link->view,
                      "view differs");
                check("identical offset", flat_link->off == threed_link->off,
                      "off differs");
                check("identical recording id",
                      flat_link->rec == threed_link->rec, "rec differs");
            }

            scene3d::PickHint flat_hint =
                scene3d::resolve_pick_hint(terr, traj, conv, flat_pick);
            scene3d::PickHint threed_hint =
                scene3d::resolve_pick_hint(terr, traj, conv, threed_pick);
            check("identical hover hint target",
                  flat_hint.target == threed_hint.target,
                  "hint target differs");
        }

        check(
            "an out-of-range fraction refuses",
            [&] {
                uint32_t c = 0;
                return !scene2d_pick_cell(plan, 1.0f, 0.5f, &c) &&
                       !scene2d_pick_cell(plan, -0.01f, 0.5f, &c);
            }(),
            "a fraction outside [0,1) must not resolve to a cell");
        {
            uint32_t c = 0;
            check("has_plane=false refuses regardless of fraction",
                  !scene2d_pick_cell(Scene2dPlan{}, 0.5f, 0.5f, &c),
                  "an empty plan must never resolve a cell");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_scene2d: all checks passed\n");
    return 0;
}
