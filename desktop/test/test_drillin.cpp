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
#include "scene3d/goto.h"
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
              radius >= scene3d::Camera::kMinRadius &&
                  radius <= scene3d::Camera::kMaxRadius,
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

    if (failures) {
        std::fprintf(stderr, "%d drill-in check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_drillin: all checks passed\n");
    return 0;
}
