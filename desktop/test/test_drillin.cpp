// test_drillin.cpp — the 3D-overview drill-in router + the two fidelity invariants
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T6). Null harness, no display:
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

        // === T2 (47-scene-inspect-and-pickable-overlays): resolve_pick_hint ===
        // for the SAME pick — the anti-drift assertion (target must equal
        // dt_view_name(resolve_pick(...)->view)) plus the golden text.
        space::ConvergenceSet conv;
        PickHint hint = resolve_pick_hint(terr, traj, conv, cell_pick(c, n));
        check("hint/exact code cell: not empty", !hint.empty, "empty hint");
        check("hint/exact code cell: what == code cell",
              hint.what == "code cell", hint.what);
        check("hint/exact code cell: quantity names its unit",
              hint.quantity.find("hits") != std::string::npos, hint.quantity);
        check("hint/exact code cell: exact => no fidelity note",
              hint.fidelity.empty(), hint.fidelity);
        check("hint/exact code cell: target agrees with resolve_pick",
              link.has_value() && hint.target == dt_view_name(link->view),
              "target/view disagreement: '" + hint.target + "'");
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

        // === T2: the hint for a CHURNED code cell must say so (not "exact") ===
        space::ConvergenceSet conv;
        PickHint hint = resolve_pick_hint(terr, traj, conv, cell_pick(c, n));
        check("hint/churned code cell: not empty", !hint.empty, "empty hint");
        check("hint/churned code cell: what == code cell",
              hint.what == "code cell", hint.what);
        check("hint/churned code cell: fidelity names the churn",
              hint.fidelity.find("churn") != std::string::npos, hint.fidelity);
        check("hint/churned code cell: target agrees with resolve_pick "
              "(disasm)",
              link.has_value() && hint.target == dt_view_name(link->view),
              "target/view disagreement: '" + hint.target + "'");
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

        // === T1 (47-scene-inspect-and-pickable-overlays): the cell->content ===
        // index (terr.code_at/data_at, O(log n)) must return exactly what a
        // linear scan of terr.code/terr.data would, for every touched cell AND
        // for a cell with neither — the anti-drift bar T1 exists for (a hover
        // pick and a click share this one lookup path).
        auto linear_code_at =
            [&](uint32_t cell) -> const space::TerrainModel::CodeCell * {
            for (const auto &cc : terr.code)
                if (cc.cell == cell)
                    return &cc;
            return nullptr;
        };
        auto linear_data_at =
            [&](uint32_t cell) -> const space::TerrainModel::DataCell * {
            for (const auto &dc : terr.data)
                if (dc.cell == cell)
                    return &dc;
            return nullptr;
        };
        // The touched code cell, the touched data cell, and a cell that is
        // neither (padding beyond the tiny domain this fixture describes).
        uint32_t cCode = cell_at(terr.proj, 4194304, &ok);
        uint32_t cData = c; // the data cell resolved just above
        uint32_t cNeither = n * n - 1;
        for (uint32_t cell : {cCode, cData, cNeither}) {
            check("T1 index/code_at matches the linear scan at cell " +
                      std::to_string(cell),
                  terr.code_at(cell) == linear_code_at(cell),
                  "the O(log n) index disagreed with the linear scan");
            check("T1 index/data_at matches the linear scan at cell " +
                      std::to_string(cell),
                  terr.data_at(cell) == linear_data_at(cell),
                  "the O(log n) index disagreed with the linear scan");
        }
        check("T1: the neither-cell truly has neither",
              terr.code_at(cNeither) == nullptr &&
                  terr.data_at(cNeither) == nullptr,
              "test setup: cNeither must hold no content");

        // === T2: the hint for a DATA cell names its bytes-accessed magnitude ===
        space::ConvergenceSet conv;
        PickHint hint =
            resolve_pick_hint(terr, traj, conv, cell_pick(cData, n));
        check("hint/data cell: not empty", !hint.empty, "empty hint");
        check("hint/data cell: what == data cell", hint.what == "data cell",
              hint.what);
        check("hint/data cell: quantity names its unit (bytes)",
              hint.quantity.find("bytes") != std::string::npos, hint.quantity);
        check("hint/data cell: target agrees with resolve_pick (slice)",
              link.has_value() && hint.target == dt_view_name(link->view),
              "target/view disagreement: '" + hint.target + "'");
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

        // === T2: the hint for an exact PC vertex ===============================
        space::ConvergenceSet conv;
        PickHint hint = resolve_pick_hint(terr, traj, conv, vertex_pick(2, n));
        check("hint/exact vertex: not empty", !hint.empty, "empty hint");
        check("hint/exact vertex: what == PC vertex", hint.what == "PC vertex",
              hint.what);
        check("hint/exact vertex: quantity names the step (2)",
              hint.quantity.find("2") != std::string::npos, hint.quantity);
        check("hint/exact vertex: exact => no fidelity note",
              hint.fidelity.empty(), hint.fidelity);
        check("hint/exact vertex: target agrees with resolve_pick (timeline)",
              link.has_value() && hint.target == dt_view_name(link->view),
              "target/view disagreement: '" + hint.target + "'");
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

        // === T2: hints for background / padding / out-of-range-vertex =========
        space::ConvergenceSet conv;
        PickHint bg_hint =
            resolve_pick_hint(terr, traj, conv, decode_pick(0, n));
        check("hint/background: empty", bg_hint.empty, "background not empty");
        check("hint/background: no click target", bg_hint.target.empty(),
              bg_hint.target);

        PickHint pad_hint = resolve_pick_hint(terr, traj, conv, padding);
        check("hint/padding: what == padding", pad_hint.what == "padding",
              pad_hint.what);
        check("hint/padding: fidelity says outside the compacted domain",
              pad_hint.fidelity.find("outside the compacted domain") !=
                  std::string::npos,
              pad_hint.fidelity);
        check("hint/padding: no click target (a click here does nothing)",
              pad_hint.target.empty(), pad_hint.target);
        check("hint/padding: target empty exactly when resolve_pick is nullopt",
              !resolve_pick(terr, traj, "r", padding).has_value() &&
                  pad_hint.target.empty(),
              "anti-drift: padding's hint disagreed with resolve_pick");

        PickHint past_hint = resolve_pick_hint(terr, traj, conv, past);
        check("hint/vertex past geometry: empty", past_hint.empty,
              "past-geometry vertex hint not empty");
        check("hint/vertex past geometry: no click target",
              past_hint.target.empty(), past_hint.target);
    }

    // === 5b. T2: a bare, in-domain region cell -> "never described" ============
    // The same fixture as #5: a 256-byte code region with only ONE offset ever
    // traced (4194304). Most of the region's other cells hold no CodeCell at
    // all — in-domain (they belong to a real region) but never described by
    // any event — the fog-of-war fact a hint must name honestly rather than
    // inventing "0 hits" (D7).
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

        bool ok = false;
        // An address well inside the 256-byte region but never traced.
        uint32_t cUnknown = cell_at(terr.proj, 4194304 + 128, &ok);
        check("5b: the never-touched address projects (test setup)", ok,
              "4194304+128 did not project");
        check("5b: the cell truly holds no CodeCell (test setup)",
              terr.code_at(cUnknown) == nullptr,
              "the fixture must leave this cell undescribed");

        auto link =
            resolve_pick(terr, traj, "rec.asmtrace", cell_pick(cUnknown, n));
        check("5b: a bare in-domain cell still opens the canvas",
              link.has_value() && link->view == dt_view::canvas,
              "an in-domain, undescribed cell should still open the canvas");

        space::ConvergenceSet conv;
        PickHint hint =
            resolve_pick_hint(terr, traj, conv, cell_pick(cUnknown, n));
        check("hint/TF_UNKNOWN cell: not empty", !hint.empty, "empty hint");
        check("hint/TF_UNKNOWN cell: quantity is NOT a fabricated zero count",
              hint.quantity.find("0 hits") == std::string::npos &&
                  hint.quantity.find("0 bytes") == std::string::npos,
              hint.quantity);
        check("hint/TF_UNKNOWN cell: fidelity names it never described",
              hint.fidelity.find("never described") != std::string::npos,
              hint.fidelity);
        check("hint/TF_UNKNOWN cell: target agrees with resolve_pick",
              hint.target == dt_view_name(link->view),
              "target/view disagreement: '" + hint.target + "'");
    }

    // === 5c. T2: a data region with no `mem` stream -> the coarse note, not a
    // fabricated "0 bytes" =======================================================
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,\"drops\":{"
            "\"lost\":"
            "0,\"throttled\":false}}\n");
        std::vector<space::Region> regs = space::regions_from_codeimage(rec);
        regs.push_back(region(6291456, 4096, space::Region::Data)); // 0x600000
        space::Projection p = space::build_projection(regs);
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        const uint32_t n = terr.w;
        check("5c: no mem stream (test setup)", !terr.mem_present,
              "mem present");
        check("5c: mem_note is the coarse chip",
              terr.mem_note == "coarse: no per-access memory stream",
              terr.mem_note);

        bool ok = false;
        uint32_t cData = cell_at(terr.proj, 6291456, &ok);
        space::ConvergenceSet conv;
        PickHint hint =
            resolve_pick_hint(terr, traj, conv, cell_pick(cData, n));
        check("hint/coarse data cell: not empty", !hint.empty, "empty hint");
        check("hint/coarse data cell: what == data cell",
              hint.what == "data cell", hint.what);
        check("hint/coarse data cell: quantity reuses TerrainModel::mem_note "
              "verbatim",
              hint.quantity == terr.mem_note, hint.quantity);
        check("hint/coarse data cell: never a fabricated zero count",
              hint.quantity.find("0 bytes") == std::string::npos,
              hint.quantity);
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

        // === T2: the hint for a statistical-only CELL ==========================
        {
            space::ConvergenceSet conv;
            PickHint hint =
                resolve_pick_hint(terr, traj, conv, cell_pick(c, n));
            check(name + ": hint/stat cell: not empty", !hint.empty,
                  "empty hint");
            check(name + ": hint/stat cell: what == survey cell",
                  hint.what == "survey cell", hint.what);
            check(name + ": hint/stat cell: fidelity says statistical",
                  hint.fidelity.find("statistical") != std::string::npos,
                  hint.fidelity);
            check(name + ": hint/stat cell: never claims exactness",
                  hint.fidelity.find("never exact") != std::string::npos,
                  hint.fidelity);
            check(name + ": hint/stat cell: quantity names its unit",
                  hint.quantity.find("samples") != std::string::npos,
                  hint.quantity);
            check(name + ": hint/stat cell: target agrees with resolve_pick "
                         "(hotedges)",
                  clink.has_value() && hint.target == dt_view_name(clink->view),
                  "target/view disagreement: '" + hint.target + "'");
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

            // === T2: the hint for a statistical VERTEX =========================
            space::ConvergenceSet conv;
            PickHint vhint =
                resolve_pick_hint(terr, traj, conv, vertex_pick(0, n));
            check(name + ": hint/stat vertex: not empty", !vhint.empty,
                  "empty hint");
            check(name + ": hint/stat vertex: what == PC vertex",
                  vhint.what == "PC vertex", vhint.what);
            check(name + ": hint/stat vertex: fidelity says statistical",
                  vhint.fidelity.find("statistical") != std::string::npos,
                  vhint.fidelity);
            check(name + ": hint/stat vertex: target agrees with resolve_pick "
                         "(hotedges)",
                  vlink.has_value() &&
                      vhint.target == dt_view_name(vlink->view),
                  "target/view disagreement: '" + vhint.target + "'");
        }
    }

    // === T3 (47-scene-inspect-and-pickable-overlays): the id bands ===========
    // round-trip cleanly, with no aliasing between the vertex/conv/spur bands,
    // across several (n, npts, nconv, nspur) combinations including empty
    // bands. This is the pure decode-side proof; test_scene_fbo.cpp's GL
    // smoke proves the SAME bands come back from a real render_pick_buffer.
    {
        struct Combo {
            uint32_t n;
            uint64_t npts, nconv, nspur;
        };
        const Combo combos[] = {
            {8, 0, 0, 0},   // every band empty
            {8, 5, 0, 0},   // vertices only
            {8, 0, 3, 0},   // convergences only (npts=0 is a legal shape)
            {8, 0, 0, 4},   // spurs only
            {8, 5, 3, 4},   // all three populated
            {64, 1, 1, 1},  // a realistic plane, one of each
        };
        for (const Combo &c : combos) {
            PickBands bands;
            bands.npts = c.npts;
            bands.nconv = c.nconv;
            bands.nspur = c.nspur;
            const std::string tag = "n=" + std::to_string(c.n) +
                                    " npts=" + std::to_string(c.npts) +
                                    " nconv=" + std::to_string(c.nconv) +
                                    " nspur=" + std::to_string(c.nspur);

            // Cell band: unaffected by npts/nconv/nspur.
            Pick c0 = decode_pick(pick_id_cell(0), c.n, bands);
            check("T3 bands " + tag + ": cell 0 round-trips",
                  c0.kind == Pick::Cell && c0.cell == 0, "cell 0 wrong");
            Pick cLast =
                decode_pick(pick_id_cell(c.n * c.n - 1), c.n, bands);
            check("T3 bands " + tag + ": last cell round-trips",
                  cLast.kind == Pick::Cell && cLast.cell == c.n * c.n - 1,
                  "last cell wrong");

            if (c.npts > 0) {
                Pick v0 = decode_pick(pick_id_vertex(c.n, 0), c.n, bands);
                check("T3 bands " + tag + ": vertex 0 round-trips",
                      v0.kind == Pick::Vertex && v0.vertex == 0,
                      "vertex 0 wrong");
                Pick vLast = decode_pick(pick_id_vertex(c.n, c.npts - 1),
                                        c.n, bands);
                check("T3 bands " + tag + ": last vertex round-trips",
                      vLast.kind == Pick::Vertex &&
                          vLast.vertex == c.npts - 1,
                      "last vertex wrong");
            }
            if (c.nconv > 0) {
                Pick a0 = decode_pick(pick_id_conv(c.n, c.npts, 0), c.n,
                                      bands);
                check("T3 bands " + tag + ": conv 0 round-trips",
                      a0.kind == Pick::Conv && a0.conv == 0, "conv 0 wrong");
                Pick aLast = decode_pick(
                    pick_id_conv(c.n, c.npts, c.nconv - 1), c.n, bands);
                check("T3 bands " + tag + ": last conv round-trips",
                      aLast.kind == Pick::Conv && aLast.conv == c.nconv - 1,
                      "last conv wrong");
            }
            if (c.nspur > 0) {
                Pick s0 = decode_pick(pick_id_spur(c.n, c.npts, c.nconv, 0),
                                      c.n, bands);
                check("T3 bands " + tag + ": spur 0 round-trips",
                      s0.kind == Pick::Spur && s0.spur == 0, "spur 0 wrong");
                Pick sLast = decode_pick(
                    pick_id_spur(c.n, c.npts, c.nconv, c.nspur - 1), c.n,
                    bands);
                check("T3 bands " + tag + ": last spur round-trips",
                      sLast.kind == Pick::Spur && sLast.spur == c.nspur - 1,
                      "last spur wrong");
            }
            // An id one past the LAST populated band decodes to None — the
            // "no aliasing" bar: it must never fall back into an adjacent
            // band by miscounting a boundary.
            const uint64_t past =
                static_cast<uint64_t>(c.n) * c.n + 1u + c.npts + c.nconv +
                c.nspur;
            Pick none = decode_pick(static_cast<uint32_t>(past), c.n, bands);
            check("T3 bands " + tag + ": one past every band -> None",
                  none.kind == Pick::None,
                  "an id past every band must decode to nothing, not alias");
        }

        // The legacy default (no bands argument) is UNCHANGED: an unbounded
        // vertex band, exactly pre-T3 behaviour — every existing call site
        // that never learned about PickBands must decode identically.
        Pick legacy = decode_pick(pick_id_vertex(8, 12345), 8);
        check("T3 bands: the no-bands default stays legacy-unbounded",
              legacy.kind == Pick::Vertex && legacy.vertex == 12345,
              "the default PickBands changed pre-T3 decode behaviour");
    }

    // === T3: a convergence arc resolves to whichever tid is nearer follow_step
    {
        // Two threads (tid 1, tid 2) both touch the same cell — one PC vertex
        // each — so detect_convergences yields exactly one mark.
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":4096,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304,\"tid\":1}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304,\"tid\":2}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec, p);
        space::ConvergenceSet conv = space::detect_convergences(traj, p);
        check("T3 conv: one cross-thread mark", conv.marks.size() == 1,
              "got " + std::to_string(conv.marks.size()));

        if (conv.marks.size() == 1) {
            const space::ConvergenceMark &m = conv.marks[0];
            Pick pk;
            pk.kind = Pick::Conv;
            pk.conv = 0;

            // follow_step nearer tid_a's t: resolves to tid_a's step.
            auto link_a =
                resolve_pick(terr, traj, "rec.asmtrace", pk, conv, m.t_a);
            check("T3 conv: resolves to a link", link_a.has_value(),
                  "no link");
            if (link_a) {
                check("T3 conv: opens the operand timeline",
                      link_a->view == dt_view::timeline, "wrong view");
                check("T3 conv: step names one of the two tids' t",
                      link_a->step.has_value() &&
                          (*link_a->step == m.t_a || *link_a->step == m.t_b),
                      "step names neither tid");
                check("T3 conv: follow_step==t_a resolves to t_a",
                      *link_a->step == m.t_a, "did not pick the nearer side");
            }
            // follow_step nearer tid_b's t (if the two differ; else both
            // sides tie and 'a' wins by the documented tie rule).
            if (m.t_a != m.t_b) {
                auto link_b = resolve_pick(terr, traj, "rec.asmtrace", pk,
                                           conv, m.t_b);
                check("T3 conv: follow_step==t_b resolves to t_b",
                      link_b.has_value() && *link_b->step == m.t_b,
                      "did not pick the nearer side");
            }

            // The hint must say WHICH side was chosen, carry gap verbatim,
            // and never upgrade the co-locality hint into a stronger claim.
            PickHint hint =
                resolve_pick_hint(terr, traj, conv, pk, m.t_a);
            check("T3 conv: hint not empty", !hint.empty, "empty hint");
            check("T3 conv: hint/what == convergence",
                  hint.what == "convergence", hint.what);
            check("T3 conv: hint names the chosen tid",
                  hint.fidelity.find(std::to_string(m.tid_a)) !=
                      std::string::npos,
                  hint.fidelity);
            check("T3 conv: hint carries the hint wording verbatim",
                  hint.fidelity.find("not a proven race or order") !=
                      std::string::npos,
                  hint.fidelity);
            check("T3 conv: hint quantity states the gap with its unit",
                  hint.quantity.find("gap") != std::string::npos &&
                      hint.quantity.find("step") != std::string::npos,
                  hint.quantity);
            check("T3 conv: hint target agrees with resolve_pick",
                  link_a.has_value() &&
                      hint.target == dt_view_name(link_a->view),
                  "target/view disagreement: '" + hint.target + "'");

            // An out-of-range conv index resolves to nothing on both sides.
            Pick past;
            past.kind = Pick::Conv;
            past.conv = 99;
            check("T3 conv: an out-of-range conv index -> no link",
                  !resolve_pick(terr, traj, "r", past, conv, 0).has_value(),
                  "an out-of-range conv index produced a link");
            PickHint past_hint =
                resolve_pick_hint(terr, traj, conv, past, 0);
            check("T3 conv: an out-of-range conv index -> empty hint",
                  past_hint.empty, "not empty");
        }
    }

    // === T3: an access spur resolves to its owning PC vertex's step ==========
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"
            "{\"k\":\"mem\",\"step\":1,\"ea\":6291456,\"size\":8,\"rw\":\"r\"}"
            "\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        std::vector<space::Region> regs = space::regions_from_codeimage(rec);
        regs.push_back(region(6291456, 4096, space::Region::Data));
        space::Projection p = space::build_projection(regs);
        space::TrajectorySet traj = space::build_trajectories(rec, p);
        std::vector<PickSpur> sorder = pick_spur_order(traj, p);
        check("T3 spur: one access spur is pickable", sorder.size() == 1,
              "got " + std::to_string(sorder.size()));
        if (sorder.size() == 1) {
            check("T3 spur: the spur names its owning vertex's step (1)",
                  sorder[0].t == 1, "got " + std::to_string(sorder[0].t));

            space::TerrainModel terr = space::build_terrain(p, rec);
            space::ConvergenceSet conv;
            Pick pk;
            pk.kind = Pick::Spur;
            pk.spur = 0;
            auto link = resolve_pick(terr, traj, "rec.asmtrace", pk, conv, 0);
            check("T3 spur: resolves to a link", link.has_value(), "no link");
            if (link) {
                check("T3 spur: opens the operand timeline at its step",
                      link->view == dt_view::timeline &&
                          link->step.has_value() && *link->step == 1,
                      "wrong view/step");
            }
            PickHint hint = resolve_pick_hint(terr, traj, conv, pk, 0);
            check("T3 spur: hint not empty", !hint.empty, "empty hint");
            check("T3 spur: hint/what == access spur",
                  hint.what == "access spur", hint.what);
            check("T3 spur: hint target agrees with resolve_pick",
                  link.has_value() && hint.target == dt_view_name(link->view),
                  "target/view disagreement: '" + hint.target + "'");

            Pick past;
            past.kind = Pick::Spur;
            past.spur = 99;
            check("T3 spur: an out-of-range spur index -> no link",
                  !resolve_pick(terr, traj, "r", past, conv, 0).has_value(),
                  "an out-of-range spur index produced a link");
        }
    }

    // === T3 (50-two-way-brushing): the reverse-direction unguarded ordinal ====
    // A two-tid trace groups into TWO trajectories (by_tid keyed on tid), each
    // with its OWN per-tid ordinal `t` starting at 0 — so tid 1's and tid 2's
    // vertices collide onto the SAME `t` values by construction. The single-tid
    // fixture at "an exact PC vertex -> the operand TIMELINE at that step"
    // (above) already pins the no-regression case: `traj.trajectories.size()
    // == 1` there, so the ordinal fallback still fires and its expected step
    // (2) is unchanged.
    {
        Recording rec = mk_rec(
            std::string(kHdrExact) +
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":1,\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":1,\"off\":4194308}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":1,\"off\":4194312}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":2,\"off\":4194320}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":2,\"off\":4194324}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":2,\"off\":4194328}\n"
            // A dataflow pass over the SAME six PCs, global step order (df_step
            // carries no tid — a single linear value-flow trace, per 38's own
            // correction that a tid-on-df_step convergence brief is dead).
            "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":2,\"off\":8,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":3,\"off\":16,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":4,\"off\":20,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"df_step\",\"step\":5,\"off\":24,\"rbase\":4194304,"
            "\"ops\":[]}\n"
            "{\"k\":\"end\",\"events\":13,\"truncated\":false,\"drops\":{"
            "\"lost\":0,\"throttled\":false}}\n");
        space::Projection p =
            space::build_projection(space::regions_from_codeimage(rec));
        space::TerrainModel terr = space::build_terrain(p, rec);
        space::TrajectorySet traj = space::build_trajectories(rec);
        check("T3 reverse/setup: two trajectories", traj.trajectories.size() == 2,
              "got " + std::to_string(traj.trajectories.size()));
        const uint32_t n = terr.w;

        std::vector<PickVertex> order = pick_vertex_order(traj);
        check("T3 reverse/setup: six pickable vertices", order.size() == 6,
              "got " + std::to_string(order.size()));
        // trajectory-then-point order, by_tid keyed ascending: tid 1's three
        // vertices (index 0-2), then tid 2's three (index 3-5). Index 5 is
        // tid 2's THIRD vertex — its own per-tid ordinal `t` is 2, the SAME
        // ordinal tid 1's third vertex (index 2) also carries. The old bug:
        // `link.step = pv.t` would open step 2 for BOTH, silently colliding.
        check("T3 reverse/setup: index 5 is tid 2's third vertex (t==2)",
              order[5].tid == 2 && order[5].t == 2,
              "fixture assumption broke");
        check("T3 reverse/setup: index 5's address is tid 2's third PC",
              order[5].addr == 4194328,
              "got 0x" + std::to_string(order[5].addr));

        Streams s = decode_streams(rec);
        check("T3 reverse/setup: df decoded", s.df.insn_off.size() == 6,
              "fixture must decode six df_step entries");

        auto link =
            resolve_pick(terr, traj, "rec.asmtrace", vertex_pick(5, n),
                        space::ConvergenceSet{}, 0, &s.df);
        check("T3 reverse: resolves to a link", link.has_value(), "no link");
        if (link) {
            check("T3 reverse: opens the operand timeline",
                  link->view == dt_view::timeline, "wrong view");
            check("T3 reverse: opens at the ADDRESS-matched step (5), not "
                  "the per-tid ordinal (2)",
                  link->step.has_value() && *link->step == 5,
                  "step wrong: got " +
                      (link->step ? std::to_string(*link->step) : "none"));
        }

        // Same two-tid pick, but with NO df supplied at all: the ordinal is
        // unsound (two trajectories) and there is no address route to fall
        // back on, so this must land on the CANVAS at the vertex's own
        // offset — never a fabricated step.
        auto no_df_link =
            resolve_pick(terr, traj, "rec.asmtrace", vertex_pick(5, n));
        check("T3 reverse/no-df: resolves to a link", no_df_link.has_value(),
              "no link");
        if (no_df_link) {
            check("T3 reverse/no-df: falls back to the canvas, not a "
                  "fabricated timeline step",
                  no_df_link->view == dt_view::canvas, "wrong view");
            check("T3 reverse/no-df: canvas offset is addr - base",
                  no_df_link->off.has_value() &&
                      *no_df_link->off == 4194328 - 4194304,
                  "offset wrong");
        }

        // A df that covers only tid 1's PCs (steps 0-2): tid 2's vertex 5 has
        // no matching step ANYWHERE in df, so the address route finds
        // nothing, the ordinal is unsound (two trajectories), and this must
        // still fall back to the canvas rather than open step 2 (which would
        // silently be tid 1's own third step).
        {
            Recording partial = mk_rec(
                std::string(kHdrExact) +
                "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,"
                "\"version\":0,\"when\":1,\"bytes\":\"90\"}\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":1,\"off\":4194304}"
                "\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":1,\"off\":4194308}"
                "\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":1,\"off\":4194312}"
                "\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":2,\"off\":4194320}"
                "\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":2,\"off\":4194324}"
                "\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"tid\":2,\"off\":4194328}"
                "\n"
                "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4194304,"
                "\"ops\":[]}\n"
                "{\"k\":\"df_step\",\"step\":1,\"off\":4,\"rbase\":4194304,"
                "\"ops\":[]}\n"
                "{\"k\":\"df_step\",\"step\":2,\"off\":8,\"rbase\":4194304,"
                "\"ops\":[]}\n"
                "{\"k\":\"end\",\"events\":10,\"truncated\":false,\"drops\":{"
                "\"lost\":0,\"throttled\":false}}\n");
            space::Projection pp =
                space::build_projection(space::regions_from_codeimage(partial));
            space::TerrainModel tterr = space::build_terrain(pp, partial);
            space::TrajectorySet ttraj = space::build_trajectories(partial);
            Streams ss = decode_streams(partial);
            check("T3 reverse/no-match: setup: df has only 3 steps",
                  ss.df.insn_off.size() == 3,
                  "got " + std::to_string(ss.df.insn_off.size()));

            auto miss_link = resolve_pick(
                tterr, ttraj, "rec.asmtrace", vertex_pick(5, tterr.w),
                space::ConvergenceSet{}, 0, &ss.df);
            check("T3 reverse/no-match: resolves to a link",
                  miss_link.has_value(), "no link");
            if (miss_link) {
                check("T3 reverse/no-match: falls back to the canvas, not a "
                      "fabricated step",
                      miss_link->view == dt_view::canvas, "wrong view");
                check("T3 reverse/no-match: canvas offset is addr - base",
                      miss_link->off.has_value() &&
                          *miss_link->off == 4194328 - 4194304,
                      "offset wrong");
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
