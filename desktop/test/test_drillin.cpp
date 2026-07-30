// test_drillin.cpp — the 3D-overview drill-in router + the two fidelity invariants
// (docs/internal/gui/10-spacetime-3d-overview.md T6). Null harness, no display:
// this binary drives the pick-id RESOLUTION path (scene3d/pick.h) with no GL at
// all — the same closure argument test_projection makes for the plane — so "every
// pick reaches the right 2D view, and statistical/torn survive the drill" is
// asserted without a GL context. It links pick.o + the pure space/ models +
// nav.o + the trace canvas (for the truncation banner it reads) + the doc model,
// and NOTHING else (D4).
//
// "3D to find, 2D to read": every pickable kind routes through 04's router to the
// flat view that reads it. The two fidelity invariants ride along:
//   1. TRUNCATION SURVIVES THE DRILL-IN — a TORN cell's 2D target carries 04/08's
//      truncation banner (the 3D tear is never the only signal).
//   2. STATISTICAL IS NEVER EXACT — a survey-only recording yields no exact tube,
//      the HUD's statistical provenance is set, and a statistical pick opens the
//      hot-edge view (08-T4), NEVER the exact slice explorer.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/streams.h"
#include "scene3d/pick.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "space/trajectory.h"
#include "views/canvas.h"

#ifndef ASMTEST_FIXTURE_DIR
#error "ASMTEST_FIXTURE_DIR must be defined by the build (mk/desktop.mk)"
#endif

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

// Build a Recording from an in-memory NDJSON string, through the real loader.
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

// Load one of the committed fixtures (truncated.asmtrace, obs-survey-*).
static Recording load_fixture(const std::string &name) {
    std::string path = std::string(ASMTEST_FIXTURE_DIR) + "/" + name;
    std::string err;
    auto rec = load_recording_file(path, err);
    if (!rec) {
        fail("load fixture " + name, err);
        return Recording{};
    }
    return *rec;
}

// The plane cell an address projects into — the SAME rounding terrain.cpp uses,
// so an expected cell and the terrain's agree.
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

// The pick a click on a cell centre produces (id encode -> decode round trip).
static Pick cell_pick(uint32_t cell, uint32_t n) {
    return decode_pick(pick_id_cell(cell), n);
}
static Pick vertex_pick(uint64_t vindex, uint32_t n) {
    return decode_pick(pick_id_vertex(n, vindex), n);
}

// A code region, hand-declared where a fixture carries no codeimage (a rel-basis
// or survey stream: the caller supplies the region a maps snapshot / window would).
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
    // === 1. an exact code cell -> the trace CANVAS at the code offset ==========
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,\"drops\":{"
            "\"lost\":"
            "0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;

        bool ok = false;
        uint32_t c = cell_at(terr.proj, 4194304, &ok);
        auto link = resolve_pick(terr, traj, "rec.asmtrace", cell_pick(c, n));
        check("code cell resolves to a link", link.has_value(), "no link");
        if (link) {
            check("code cell opens the trace canvas",
                  link->view == dt_view::canvas, "wrong view");
            check("code cell carries the code offset (0)",
                  link->off.has_value() && *link->off == 0,
                  "off wrong / absent");
            check("code cell names the recording", link->rec == "rec.asmtrace",
                  "rec wrong");
            check("a plain code cell is NOT the disasm pane",
                  link->view != dt_view::disasm, "churned by mistake");
        }
    }

    // === 2. a CHURNED code cell -> the codeimage-versioned DISASM pane =========
    // A codeimage version bump within the recording marks the region churned; the
    // exact bytes at an offset then differ by trace time, so the pick opens the
    // version-aware disasm pane (08-T7), not the plain canvas.
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":1,"
            "\"when\":5,\"bytes\":\"cc\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"end\",\"events\":5,\"truncated\":false,\"drops\":{"
            "\"lost\":"
            "0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;
        check("the recording churned", terr.churn_present, "no churn");

        bool ok = false;
        uint32_t c = cell_at(terr.proj, 4194304, &ok);
        auto link = resolve_pick(terr, traj, "rec.asmtrace", cell_pick(c, n));
        check("churned cell resolves to a link", link.has_value(), "no link");
        if (link) {
            check("churned cell opens the disasm pane",
                  link->view == dt_view::disasm, "wrong view");
            check("churned cell carries the code offset (0)",
                  link->off.has_value() && *link->off == 0, "off wrong");
            check("a churned cell is NEVER the plain canvas",
                  link->view != dt_view::canvas,
                  "a churned region on the canvas would show the wrong bytes");
        }
    }

    // === 3. a DATA cell (rich `mem` rung) -> the SLICE explorer at the step ====
    // The mem kind has no v1 producer (inert on real recordings); a hand-authored
    // fixture exercises the path. Two accesses hit one data cell — the drill opens
    // the slice explorer at the MOST RECENT access step.
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194312}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194316}\n"
            "{\"k\":\"mem\",\"step\":1,\"ea\":6291456,\"size\":8,\"rw\":\"r\"}"
            "\n"
            "{\"k\":\"mem\",\"step\":3,\"ea\":6291456,\"size\":8,\"rw\":\"w\"}"
            "\n"
            "{\"k\":\"end\",\"events\":7,\"truncated\":false,\"drops\":{"
            "\"lost\":"
            "0,\"throttled\":false}}\n");
        std::vector<space::Region> regs = space::regions_from_codeimage(rec);
        regs.push_back(region(6291456, 4096, space::Region::Data)); // 0x600000
        space::Projection p = space::build_projection(regs);
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;
        check("the mem rung is present", terr.mem_present, "no mem");

        bool ok = false;
        uint32_t c = cell_at(terr.proj, 6291456, &ok);
        auto link = resolve_pick(terr, traj, "rec.asmtrace", cell_pick(c, n));
        check("data cell resolves to a link", link.has_value(), "no link");
        if (link) {
            check("data cell opens the slice explorer",
                  link->view == dt_view::slice, "wrong view");
            check("data cell opens at the most-recent access step (3)",
                  link->step.has_value() && *link->step == 3,
                  "step wrong / absent");
        }
    }

    // === 4. an exact PC vertex -> the operand TIMELINE at that step ============
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194312}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,\"drops\":{"
            "\"lost\":"
            "0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;

        std::vector<PickVertex> order = pick_vertex_order(traj);
        check("three PC vertices are pickable", order.size() == 3,
              "got " + std::to_string(order.size()));
        auto link = resolve_pick(terr, traj, "rec.asmtrace", vertex_pick(2, n));
        check("exact vertex resolves to a link", link.has_value(), "no link");
        if (link) {
            check("exact vertex opens the operand timeline",
                  link->view == dt_view::timeline, "wrong view");
            check("exact vertex opens at its step (2)",
                  link->step.has_value() && *link->step == 2, "step wrong");
            check("an exact vertex is NEVER the hot-edge view",
                  link->view != dt_view::hotedges, "exact routed to hotedges");
        }
    }

    // === 5. None / padding pick -> no link ====================================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":"
            "0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;

        check("the background id -> None -> no link",
              !resolve_pick(terr, traj, "r", decode_pick(0, n)).has_value(),
              "background produced a link");
        // The last plane cell (n*n-1) is padding beyond the 256-byte domain.
        Pick padding = cell_pick(n * n - 1, n);
        check("a padding cell resolves to no link",
              !resolve_pick(terr, traj, "r", padding).has_value(),
              "padding produced a link");
        // A vertex id past the geometry resolves to nothing.
        Pick past = vertex_pick(999, n);
        check("a vertex id past the geometry -> no link",
              !resolve_pick(terr, traj, "r", past).has_value(),
              "an out-of-range vertex produced a link");
    }

    // === FIDELITY INVARIANT 1: truncation survives the drill-in ================
    // truncated.asmtrace is a rel-basis, torn recording with no codeimage — the
    // caller supplies the code region a window/maps snapshot would. Its TORN cell
    // drills to the trace canvas, and that 2D view carries the truncation banner:
    // the 3D tear is never the only signal.
    {
        Recording rec = load_fixture("truncated.asmtrace");
        // rel basis: the trace offset (0) IS the address, so the region base is 0.
        space::Projection p =
            space::build_projection({region(0, 0x1000, space::Region::Code)});
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;
        check("the truncated fixture is torn", terr.torn, "not torn");

        // The one trace hit (off 0) is a TORN cell on the full slice.
        space::Terrain full = terr.full();
        bool ok = false;
        uint32_t c = cell_at(terr.proj, 0, &ok);
        check("the picked cell is flagged TORN",
              ok && c < full.flags.size() &&
                  (full.flags[c] & space::TF_TORN) != 0u,
              "the cell the drill opens is not marked torn");

        auto link =
            resolve_pick(terr, traj, "truncated.asmtrace", cell_pick(c, n));
        check("the torn cell drills to a 2D view", link.has_value(), "no link");
        if (link) {
            check("the torn cell opens the trace canvas",
                  link->view == dt_view::canvas, "wrong view");
            // The invariant: the 2D view the pick opens carries the banner. The
            // canvas is built from the SAME recording the drill names.
            Streams s = decode_streams(rec);
            dt_canvas canvas = dt_canvas_build(s);
            check("the drilled-in canvas reports truncation", canvas.truncated,
                  "the 2D view lost the truncation the 3D tear showed");
            check("the drilled-in canvas carries a truncation banner",
                  !canvas.banner.empty(),
                  "no banner: the loud-truncation signal did not survive");
        }
    }

    // === FIDELITY INVARIANT 2: statistical is never exact ======================
    // A survey-only recording (ibs-op / sw-clock) is exact:false BY CONSTRUCTION.
    // It produces NO exact trajectory tube, the statistical provenance is set, and
    // every statistical pick (a TF_STAT cell, a TRAJ_STATISTICAL vertex) opens the
    // hot-edge view (08-T4) — never the exact slice explorer.
    for (const std::string &name : {std::string("obs-survey-ibs.asmtrace"),
                                    std::string("obs-survey-sw.asmtrace")}) {
        Recording rec = load_fixture(name);

        // No exact tube: build_trajectories yields ONLY statistical residency.
        space::TrajectorySet traj = space::build_trajectories(rec);
        check(name + ": not refused", !traj.refused(), traj.diagnostic);
        check(name + ": a residency layer exists", !traj.trajectories.empty(),
              "no trajectories");
        size_t exact_tubes = 0, stat_layers = 0;
        for (const space::Trajectory &tr : traj.trajectories) {
            if ((tr.flags & space::TRAJ_STATISTICAL) != 0)
                stat_layers++;
            else
                exact_tubes++;
            for (const space::TrajPoint &pt : tr.points)
                if (pt.fidelity == space::TrajPoint::Exact)
                    exact_tubes += 1000; // an exact point in a stat layer: loud
        }
        check(name + ": NO exact trajectory tube", exact_tubes == 0,
              "an exact path leaked out of a survey");
        check(name + ": a statistical residency layer is present",
              stat_layers >= 1, "no statistical layer");

        // The HUD's statistical provenance chip source is the recording's own
        // exact:false flag — set here, so the chip is carried.
        check(name + ": the statistical provenance flag is set",
              rec.statistical(), "a survey must be exact:false");

        // The survey targets fall in the sampler's window (the caller supplies the
        // data region a maps snapshot would); the residency lands on the STAT layer.
        space::Projection p = space::build_projection(
            {region(4198400, 4096, space::Region::Data)}); // window base/len
        space::TerrainModel terr = space::build_terrain(p, rec);
        const uint32_t n = terr.w;
        check(name + ": a separate statistical terrain layer exists",
              terr.has_stat, "no STAT layer");
        check(name + ": the exact terrain is empty (isolation)",
              terr.code.empty(),
              "an exact code cell in a survey-only recording");

        // A statistical CELL (a TF_STAT residency cell) -> the hot-edge view.
        bool ok = false;
        uint32_t c = cell_at(terr.proj, 4198900, &ok); // a survey `to` target
        check(name + ": the target cell is flagged STAT",
              ok && terr.has_stat && c < terr.stat.flags.size() &&
                  (terr.stat.flags[c] & space::TF_STAT) != 0u,
              "the survey target did not land on the STAT layer");
        auto clink = resolve_pick(terr, traj, name, cell_pick(c, n));
        check(name + ": a statistical cell resolves to a link",
              clink.has_value(), "no link");
        if (clink) {
            check(name + ": a statistical cell opens the hot-edge view",
                  clink->view == dt_view::hotedges, "wrong view");
            check(name + ": a statistical cell NEVER opens the slice explorer",
                  clink->view != dt_view::slice,
                  "a sampled residency opened an EXACT reader");
            check(name + ": a statistical cell NEVER opens the trace canvas",
                  clink->view != dt_view::canvas, "sampled -> exact canvas");
        }

        // A statistical VERTEX (a TRAJ_STATISTICAL residency point) -> hot-edges.
        std::vector<PickVertex> order = pick_vertex_order(traj);
        check(name + ": statistical residency vertices are pickable",
              !order.empty(), "no pickable vertices");
        if (!order.empty()) {
            check(name + ": the pickable vertex is flagged statistical",
                  order[0].statistical, "a survey vertex not flagged");
            auto vlink = resolve_pick(terr, traj, name, vertex_pick(0, n));
            check(name + ": a statistical vertex resolves to a link",
                  vlink.has_value(), "no link");
            if (vlink) {
                check(name + ": a statistical vertex opens the hot-edge view",
                      vlink->view == dt_view::hotedges, "wrong view");
                check(
                    name +
                        ": a statistical vertex NEVER opens the slice explorer",
                    vlink->view != dt_view::slice, "sampled -> exact slice");
                check(name + ": a statistical vertex NEVER opens the timeline",
                      vlink->view != dt_view::timeline,
                      "sampled -> exact timeline");
            }
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d drill-in check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_drillin: all checks passed\n");
    return 0;
}
