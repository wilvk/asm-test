// test_terrain.cpp — the density height field over time
// (docs/internal/gui/10-spacetime-3d-overview.md T2). Null harness, no display:
// this binary links space/terrain.o + space/projection.o + the trace canvas
// builder + the document model and NOTHING else — the same engine-free closure
// proof test_projection.cpp makes for the plane, now for the terrain over it
// (D4). Fixtures are hand-built NDJSON recordings loaded through the real loader,
// so `seq`/basis/truncation behave exactly as a producer's would.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/streams.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "views/canvas.h"

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

// The plane cell an address projects into — the SAME rounding terrain.cpp uses,
// so an expected cell and the terrain's agree.
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

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// Count non-zero cells in a height field.
static size_t nonzero(const Terrain &t) {
    size_t n = 0;
    for (float h : t.height)
        if (h > 0.0f)
            n++;
    return n;
}

// 36 T3 regression bar: STEPS WITHOUT CELLS MUST BE EXPLAINED. A terrain with a
// real time axis (nsteps > 0) but no placed code cells must carry a stated reason
// — basis_error (mixed bases) or anchor_error (no resolvable span) — never a
// silent flat plane over real steps.
static void steps_explained(const std::string &what, const TerrainModel &m) {
    if (m.nsteps > 0 && m.code.empty())
        check(what + ": steps without cells are explained",
              !m.basis_error.empty() || !m.anchor_error.empty(),
              "nsteps=" + std::to_string(m.nsteps) +
                  " but no cells and no stated reason");
}

// -------------------------------------------------------------------------
// The addresses the fixtures use, named for readability.
static const uint64_t A0 = 0x400000; // region 1, hit twice
static const uint64_t A1 = 0x400010; // region 1, hit once
static const uint64_t B0 = 0x500000; // region 2, hit three times
static const uint64_t D0 = 0x600000; // a data address (mem / survey)
static const uint64_t D1 = 0x600008;

int main() {
    // === Fixture A: coarse rung — 3 offsets in 2 regions ===================
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"codeimage\",\"base\":5242880,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // A0 step0
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n" // A1 step1
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // A0 step2
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":5242880}\n" // B0 step3
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":5242880}\n" // B0 step4
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":5242880}\n" // B0 step5
            "{\"k\":\"end\",\"events\":8,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");

        // The projection places the code regions the codeimage events declared.
        std::vector<Region> regs = regions_from_codeimage(rec);
        check("codeimage yields two code regions", regs.size() == 2,
              "got " + std::to_string(regs.size()));
        check("regions labelled + versioned",
              !regs.empty() && regs[0].kind == Region::Code &&
                  !regs[0].label.empty(),
              "a code region must carry a kind and label");
        Projection p = build_projection(regs);
        TerrainModel m = build_terrain(p, rec);

        bool ok = false;
        uint32_t cA0 = cell_at(p, A0, &ok);
        uint32_t cA1 = cell_at(p, A1, &ok);
        uint32_t cB0 = cell_at(p, B0, &ok);

        Terrain full = m.full();
        check("A0 height is log1p(2)", near(full.height[cA0], std::log1p(2.0f)),
              "got " + std::to_string(full.height[cA0]));
        check("A1 height is log1p(1)", near(full.height[cA1], std::log1p(1.0f)),
              "got " + std::to_string(full.height[cA1]));
        check("B0 height is log1p(3)", near(full.height[cB0], std::log1p(3.0f)),
              "got " + std::to_string(full.height[cB0]));
        check("exactly three non-zero code cells", nonzero(full) == 3,
              "got " + std::to_string(nonzero(full)));

        // No survey, no mem, no truncation, no churn — every provenance chip is
        // in its coarse-but-honest state, never a silent zero.
        check("no statistical layer without survey", !m.has_stat,
              "a coarse recording has no STAT terrain");
        check("mem gate inert without the stream", !m.mem_present,
              "no mem kind => the rich rung stays off");
        check("mem note names the coarse provenance",
              m.mem_note == "coarse: no per-access memory stream", m.mem_note);
        check("no data cells without mem", m.data.empty(),
              "data must be empty");
        check("not torn", !m.torn, "a complete recording is not torn");
        check("no churn without a version change", !m.churn_present, "churn?");
        for (uint32_t f : full.flags)
            check("no flags set on a clean coarse terrain", f == 0u,
                  "a flag bit is set with nothing to justify it");

        // --- REUSE PROOF: slice(full) reproduces the canvas per-offset heat ---
        // The coarse height source is 04-T3's per-offset count; re-project every
        // canvas row and the summed heat must equal the terrain's full slice.
        Streams s = decode_streams(rec);
        dt_canvas canvas = dt_canvas_build(s);
        std::map<uint32_t, double> want;
        for (const dt_canvas_row &row : canvas.rows) {
            bool k = false;
            uint32_t c = cell_at(p, row.off, &k);
            if (k)
                want[c] += row.heat;
        }
        for (const auto &kv : want)
            check("terrain height == reused canvas heat at cell " +
                      std::to_string(kv.first),
                  near(full.height[kv.first], std::log1p((float)kv.second)),
                  "got " + std::to_string(full.height[kv.first]) + " want " +
                      std::to_string(std::log1p((float)kv.second)));

        // --- TIME SLICING: an inclusive prefix [0, t] ------------------------
        Terrain t0 = m.slice(0); // only step 0 (A0)
        check("t=0 shows only the first step",
              near(t0.height[cA0], std::log1p(1.0f)) &&
                  t0.height[cA1] == 0.0f && t0.height[cB0] == 0.0f,
              "the playhead at 0 must show one hit at A0 and nothing else");
        Terrain t2 = m.slice(2); // steps 0,1,2: A0 x2, A1 x1, B0 x0
        check("t=2 accumulates A0 twice and A1 once, B0 not yet",
              near(t2.height[cA0], std::log1p(2.0f)) &&
                  near(t2.height[cA1], std::log1p(1.0f)) &&
                  t2.height[cB0] == 0.0f,
              "the prefix at step 2 is wrong");

        // Scrubbing the whole range is cheap (the prefix is a per-cell binary
        // search, not an event re-scan). A generous ceiling catches an O(n^2)
        // regression without being a flaky wall-clock assertion.
        std::clock_t c0 = std::clock();
        volatile size_t sink = 0;
        for (int rep = 0; rep < 200; rep++)
            for (uint64_t t = 0; t <= m.nsteps; t++)
                sink += nonzero(m.slice(t));
        double ms = 1000.0 * (std::clock() - c0) / CLOCKS_PER_SEC;
        check("scrubbing the terrain is well under budget", ms < 500.0,
              "1200 slices took " + std::to_string(ms) + " ms");
        (void)sink;

        // --- the 3D-scrub degrade target (23 T4): coarse_slice() ------------
        // A flat plane the size of the projection, no per-cell work — what the
        // scrub renders while a full slice would exceed the frame budget. It is
        // the same dims as a real slice, all heights zero (a labelled coarse
        // plane, NOT a measured zero — the pane draws scrub_degrade_note beside
        // it), and carries no TORN flag for this complete recording.
        Terrain coarse = m.coarse_slice();
        Terrain full2 = m.full();
        check("coarse/same-dims",
              coarse.w == full2.w && coarse.h == full2.h &&
                  coarse.height.size() == full2.height.size(),
              "the coarse plane is the same size as a real slice");
        check("coarse/flat", nonzero(coarse) == 0,
              "the coarse plane is flat — the full detail loads next frame");
        bool any_flag = false;
        for (uint32_t f : coarse.flags)
            if (f != 0u)
                any_flag = true;
        check("coarse/no-torn-when-clean", !any_flag,
              "a complete recording's coarse plane carries no TORN flag");
        steps_explained("A", m);
    }

    // === Fixture B: a truncated recording sets TORN =======================
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"
            "{\"k\":\"coverage\",\"basis\":\"abs\",\"blocks\":[4194304],"
            "\"blocks_total\":5,\"insns_total\":50,\"truncated\":true}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":true,"
            "\"drops\":{\"lost\":3,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        check("truncated recording is torn", m.torn, "end truncated => torn");
        Terrain full = m.full();
        size_t torn_cells = 0, populated = nonzero(full);
        for (size_t i = 0; i < full.flags.size(); i++)
            if (full.flags[i] & TF_TORN) {
                torn_cells++;
                check("TORN only where there is height", full.height[i] > 0.0f,
                      "a TORN flag on an empty cell claims a torn density that "
                      "was never there");
            }
        check("every populated cell is flagged TORN", torn_cells == populated,
              "torn=" + std::to_string(torn_cells) +
                  " populated=" + std::to_string(populated));
        check("a torn terrain still has height", populated > 0,
              "truncation floors the heights, it does not erase them");
        steps_explained("B", m);
    }

    // === Fixture C: survey-only — a STAT layer, an EMPTY exact layer =======
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ibs-op\","
            "\"exact\":false,\"trust\":\"statistical\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"survey\",\"sampler\":\"ibs-op\",\"edges\":["
            "{\"from_addr\":6291456,\"to_addr\":6291712,\"count\":100},"
            "{\"from_addr\":6291712,\"to_addr\":6291456,\"count\":40}],"
            "\"samples\":1000,\"lost\":0}\n"
            "{\"k\":\"end\",\"events\":1,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        // Survey addresses come from no codeimage; the caller supplies the data
        // region (a maps snapshot would, 07). Cover 0x600000..0x600100.
        Region data;
        data.base = D0;
        data.len = 4096;
        data.kind = Region::Data;
        data.label = "heap";
        Projection p = build_projection({data});
        TerrainModel m = build_terrain(p, rec);

        check("survey builds a statistical layer", m.has_stat,
              "an ibs survey with residency must produce a STAT terrain");
        size_t stat_cells = 0;
        for (size_t i = 0; i < m.stat.flags.size(); i++)
            if (m.stat.height[i] > 0.0f) {
                stat_cells++;
                check(
                    "every populated STAT cell carries the STAT flag",
                    (m.stat.flags[i] & TF_STAT) != 0u,
                    "a statistical cell that is not flagged statistical would "
                    "read as exact");
            }
        check("both survey targets land on the STAT plane", stat_cells == 2,
              "got " + std::to_string(stat_cells) + " residency cells");

        // The isolation invariant: the EXACT terrain is empty and never carries
        // a STAT bit — the two layers are distinct, never merged.
        Terrain full = m.full();
        check("the exact terrain is empty for a survey-only recording",
              nonzero(full) == 0,
              "a sampled residency must not appear on the "
              "exact terrain");
        for (uint32_t f : full.flags)
            check("no STAT bit ever leaks onto the exact terrain",
                  (f & TF_STAT) == 0u, "statistical flag on the exact layer");
        steps_explained("C", m);
    }

    // === Fixture D: the rich `mem` rung, behind the runtime gate ===========
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n"
            "{\"k\":\"mem\",\"step\":0,\"ea\":6291456,\"size\":8,\"rw\":\"w\"}"
            "\n"
            "{\"k\":\"mem\",\"step\":1,\"ea\":6291456,\"size\":8,\"rw\":\"r\"}"
            "\n"
            "{\"k\":\"mem\",\"step\":1,\"ea\":6291464,\"size\":4,\"rw\":\"r\"}"
            "\n"
            "{\"k\":\"end\",\"events\":6,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        std::vector<Region> regs = regions_from_codeimage(rec);
        Region data;
        data.base = D0;
        data.len = 4096;
        data.kind = Region::Data;
        data.label = "heap";
        regs.push_back(data);
        Projection p = build_projection(regs);
        TerrainModel m = build_terrain(p, rec);

        check("mem gate opens when the stream is present", m.mem_present,
              "a recording carrying mem events feeds the rich rung");
        check("mem note is cleared when the rung is fed", m.mem_note.empty(),
              "got: " + m.mem_note);

        bool ok = false;
        uint32_t cD0 = cell_at(p, D0, &ok);
        uint32_t cD1 = cell_at(p, D1, &ok);
        Terrain full = m.full();
        check("data cell D0 sums both access sizes (8+8)",
              near(full.height[cD0], std::log1p(16.0f)),
              "got " + std::to_string(full.height[cD0]));
        check("data cell D0 carries both a write and a read tint",
              (full.flags[cD0] & TF_WRITE) && (full.flags[cD0] & TF_READ),
              "rw channel wrong at D0");
        check("data cell D1 is a lone 4-byte read",
              near(full.height[cD1], std::log1p(4.0f)) &&
                  (full.flags[cD1] & TF_READ) && !(full.flags[cD1] & TF_WRITE),
              "D1 rich-rung state wrong");

        // Rich rung time-slices too: at t=0 only the first (write) access is in.
        Terrain t0 = m.slice(0);
        check("mem prefix at t=0 has only the first access",
              near(t0.height[cD0], std::log1p(8.0f)) &&
                  (t0.flags[cD0] & TF_WRITE) && !(t0.flags[cD0] & TF_READ) &&
                  t0.height[cD1] == 0.0f,
              "the mem playhead at 0 is wrong");
        steps_explained("D", m);
    }

    // === Fixture E: JIT churn — a version change within [0,t] sets CHURN ====
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // step0
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n" // step1
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194312}\n" // step2
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":1,"
            "\"when\":5,\"bytes\":\"cc\"}\n"                        // churn @ 3
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n" // step3
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194308}\n" // step4
            "{\"k\":\"end\",\"events\":7,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        check("a re-versioned codeimage is churn", m.churn_present,
              "two versions of one base is JIT churn");

        auto churn_cells = [](const Terrain &t) {
            size_t n = 0;
            for (uint32_t f : t.flags)
                if (f & TF_CHURN)
                    n++;
            return n;
        };
        check("no churn before the version changed (t=2)",
              churn_cells(m.slice(2)) == 0,
              "the churn is at step 3; a slice ending at 2 has not reached it");
        check("churn appears once the slice reaches step 3",
              churn_cells(m.slice(3)) > 0,
              "the version changed within [0,3] and the cells must say so");
        check("churn is set on the full slice", churn_cells(m.full()) > 0,
              "the whole recording churned");
        steps_explained("E", m);
    }

    // === Fixture F: mixed bases — the exact terrain refuses ================
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":4}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        check("mixed bases refuse the exact terrain", !m.basis_error.empty(),
              "a stream mixing rel and abs cannot be placed on one plane");
        check("a refused terrain is flat, not wrong", nonzero(m.full()) == 0,
              "no density may be drawn over an unresolvable basis");
        steps_explained("F", m);
    }

    // === Fixture G / G2: the df_step height rung (36 T3) ===================
    // A live serve dataflow/auto capture: one codeimage span + df_step offsets,
    // NO trace. The coarse rung is single-step residency, anchored to the span —
    // and it never claims block coverage (G2).
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n"  // base+0  = A0
            "{\"k\":\"df_step\",\"step\":1,\"off\":16}\n" // base+16 = A1
            "{\"k\":\"df_step\",\"step\":2,\"off\":0}\n"  // base+0  = A0 again
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);

        check("G: df rung sets height_source df_step",
              m.height_source == "df_step", "got '" + m.height_source + "'");
        check("G: label is single-step residency, not coverage",
              m.height_note.find("single-step residency") != std::string::npos &&
                  m.height_note.find("no block coverage") != std::string::npos,
              "note: " + m.height_note);
        check("G: nsteps is the df step count (real time axis)", m.nsteps == 3,
              "got " + std::to_string(m.nsteps));
        bool ok = false;
        uint32_t cA0 = cell_at(p, A0, &ok); // base+0
        uint32_t cA1 = cell_at(p, A1, &ok); // base+16
        Terrain full = m.full();
        check("G: cell at base+0 has two df hits (log1p 2)",
              near(full.height[cA0], std::log1p(2.0f)),
              "got " + std::to_string(full.height[cA0]));
        check("G: cell at base+16 has one df hit (log1p 1)",
              near(full.height[cA1], std::log1p(1.0f)),
              "got " + std::to_string(full.height[cA1]));
        check("G: exactly two non-zero df cells", nonzero(full) == 2,
              "got " + std::to_string(nonzero(full)));
        Terrain t0 = m.slice(0);
        check("G: slice(0) shows only the first hit",
              near(t0.height[cA0], std::log1p(1.0f)) && t0.height[cA1] == 0.0f,
              "the df playhead at 0 must show one hit at base+0 only");

        // G2: a df height NEVER claims block coverage.
        for (uint32_t f : full.flags)
            check("G2: a df height carries no STAT bit (exact, not sampled)",
                  (f & TF_STAT) == 0u, "df residency flagged statistical");
        check("G2: no separate statistical layer", !m.has_stat, "has_stat set");
        check("G2: no data cells synthesized", m.data.empty(), "data not empty");
        steps_explained("G", m);
    }

    // === Fixture H: two codeimage spans ⇒ no cells, anchor_error, real nsteps =
    // A flat plane that SAYS WHY (distinct from the mixed-basis basis_error).
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"codeimage\",\"base\":5242880,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":0}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":16}\n"
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        check("H: two spans ⇒ no code cells placed", m.code.empty(),
              "got " + std::to_string(m.code.size()) + " cells");
        check("H: anchor_error is set (says why)", !m.anchor_error.empty(),
              "no anchor_error on an ambiguous df recording");
        check("H: anchor_error names both bases",
              m.anchor_error.find("0x400000") != std::string::npos &&
                  m.anchor_error.find("0x500000") != std::string::npos,
              "anchor_error: " + m.anchor_error);
        check("H: NOT basis_error (that is reserved for mixed bases)",
              m.basis_error.empty(), "basis_error misused for ambiguity");
        check("H: nsteps is still real (the time axis survives)", m.nsteps == 2,
              "got " + std::to_string(m.nsteps));
        check("H: the plane is flat", nonzero(m.full()) == 0,
              "cells placed against an unresolvable anchor");
        steps_explained("H", m);
    }

    // === Fixture I: a rel-basis `trace` anchors to its single span (36 T3) ==
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":0}\n"  // base+0
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":16}\n" // base+16
            "{\"k\":\"trace\",\"basis\":\"rel\",\"off\":0}\n"  // base+0
            "{\"k\":\"end\",\"events\":4,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        check("I: rel trace basis recorded", m.basis == "rel", m.basis);
        check("I: rel trace with one span places cells (no anchor_error)",
              m.anchor_error.empty(), "anchor_error: " + m.anchor_error);
        check("I: height_source is trace", m.height_source == "trace",
              "got '" + m.height_source + "'");
        bool ok = false;
        uint32_t cA0 = cell_at(p, A0, &ok);
        uint32_t cA1 = cell_at(p, A1, &ok);
        Terrain full = m.full();
        check("I: cell at base+0 has two hits",
              near(full.height[cA0], std::log1p(2.0f)),
              "got " + std::to_string(full.height[cA0]));
        check("I: cell at base+16 has one hit",
              near(full.height[cA1], std::log1p(1.0f)),
              "got " + std::to_string(full.height[cA1]));
        check("I: exactly two non-zero cells (anchored rel trace)",
              nonzero(full) == 2, "got " + std::to_string(nonzero(full)));
        steps_explained("I", m);
    }

    if (failures) {
        std::fprintf(stderr, "%d terrain check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_terrain: all checks passed\n");
    return 0;
}
