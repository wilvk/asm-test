// test_terrain.cpp — the density height field over time
// (docs/internal/archive/gui/10-spacetime-3d-overview.md T2). Null harness, no display:
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
static const uint64_t D2 = 0x600010; // 54 T2: an access with an unknown `rw`

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

        // 49 T3: max_full_heat is the RAW (pre-log) magnitude a contour band
        // is worth — B0's 3 hits is the brightest cell over the full slice.
        check("max_full_heat over the full recording is B0's 3 hits",
              m.max_full_heat(UINT64_MAX) == 3,
              "got " + std::to_string(m.max_full_heat(UINT64_MAX)));
        check("max_full_heat is 0 for a default-built (empty) model",
              TerrainModel{}.max_full_heat(0) == 0, "an empty model has heat");

        // No survey, no mem, no truncation, no churn — every provenance chip is
        // in its coarse-but-faithful state, never a silent zero.
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
        // T2 (44-faithful-city-phase-a): a "clean" terrain still carries
        // TF_UNKNOWN over its many in-domain-but-untouched cells (the plane
        // packs a 64x64 grid over a two-region domain of a few hundred
        // bytes) — that is the fog-of-war feature working, not a leak. Only
        // TORN/STAT/CHURN/READ/WRITE must stay unset with nothing to justify
        // them; a populated cell must never ALSO read as unknown.
        for (size_t i = 0; i < full.flags.size(); i++) {
            check("no non-UNKNOWN flags on a clean coarse terrain",
                  (full.flags[i] & ~TF_UNKNOWN) == 0u,
                  "a flag bit is set with nothing to justify it");
            if (full.height[i] > 0.0f)
                check("a populated cell is never flagged fog-of-war",
                      (full.flags[i] & TF_UNKNOWN) == 0u,
                      "populated cell " + std::to_string(i) + " reads unknown");
        }
        check("an under-described plane shows fog-of-war",
              [&] {
                  size_t n = 0;
                  for (uint32_t f : full.flags)
                      if (f & TF_UNKNOWN)
                          n++;
                  return n;
              }() > 0,
              "no TF_UNKNOWN cell on a plane with only 3 of ~512 in-domain "
              "cells touched");

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
        check("max_full_heat at t=0 is A0's single hit",
              m.max_full_heat(0) == 1,
              "got " + std::to_string(m.max_full_heat(0)));
        Terrain t2 = m.slice(2); // steps 0,1,2: A0 x2, A1 x1, B0 x0
        check("t=2 accumulates A0 twice and A1 once, B0 not yet",
              near(t2.height[cA0], std::log1p(2.0f)) &&
                  near(t2.height[cA1], std::log1p(1.0f)) &&
                  t2.height[cB0] == 0.0f,
              "the prefix at step 2 is wrong");
        check("max_full_heat at t=2 is A0's two hits (B0 not yet)",
              m.max_full_heat(2) == 2,
              "got " + std::to_string(m.max_full_heat(2)));

        // === T2 (44-faithful-city-phase-a): fog-of-war is per-slice ==========
        // B0's cell is IN-DOMAIN (it is code region 2's cell — hit at steps
        // 3-5) but not yet touched at t=2: it must read TF_UNKNOWN there, and
        // must NOT once it is (t=5, the full slice) — the same cell, two
        // different fidelity states depending on WHEN you look, which is the
        // whole point of a fog-of-war frontier.
        check("T2: an in-domain, not-yet-touched cell is fog-of-war at t=2",
              (t2.flags[cB0] & TF_UNKNOWN) != 0u,
              "B0 has no hit yet at t=2 and must read unknown");
        check("T2: the same cell stops being fog-of-war once it is touched",
              (full.flags[cB0] & TF_UNKNOWN) == 0u,
              "B0 is populated by the full slice and must not read unknown");
        check("T2: a touched cell never carries TF_UNKNOWN even early",
              (t0.flags[cA0] & TF_UNKNOWN) == 0u,
              "A0 is hit at step 0 and must never read unknown");
        // An off-domain cell (kind_by_cell's sentinel) must NEVER be flagged
        // TF_UNKNOWN, at any t — it is void, not fog. Locate one by scanning
        // kind_by_cell (this plane is 64x64 over a two-region, few-hundred-
        // byte domain, so an off-domain cell certainly exists).
        {
            long off_domain = -1;
            for (size_t i = 0; i < m.kind_by_cell.size(); i++)
                if (m.kind_by_cell[i] == kKindByCellNone) {
                    off_domain = static_cast<long>(i);
                    break;
                }
            check("T2: an off-domain cell exists on this plane (test setup)",
                  off_domain >= 0, "every cell claimed a region — check math");
            if (off_domain >= 0)
                check("T2: an off-domain cell is never flagged fog-of-war",
                      (full.flags[static_cast<size_t>(off_domain)] &
                       TF_UNKNOWN) == 0u,
                      "off-domain padding must stay undrawn/dark water, "
                      "never fog-of-war land");
        }

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
            "{\"k\":\"mem\",\"step\":1,\"ea\":6291472,\"size\":2,\"rw\":\"x\"}"
            "\n"
            "{\"k\":\"end\",\"events\":7,\"truncated\":false,"
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

        // 54 T2: the read/write prefix-sum split.
        auto data_cell = [&](uint32_t cell) -> const TerrainModel::DataCell * {
            for (const auto &dc : m.data)
                if (dc.cell == cell)
                    return &dc;
            return nullptr;
        };
        const auto *dcD0 = data_cell(cD0);
        const auto *dcD1 = data_cell(cD1);
        check("D0 split sums exist", dcD0 != nullptr, "no DataCell at D0");
        if (dcD0) {
            uint64_t r = dcD0->cum_read_size.back(), w = dcD0->cum_write_size.back();
            check("D0 read/write split is 8 read + 8 write",
                  r == 8 && w == 8,
                  "got read=" + std::to_string(r) + " write=" + std::to_string(w));
            check("D0 split sums to cum_size at every index",
                  dcD0->cum_read_size.size() == dcD0->cum_size.size() &&
                      [&] {
                          for (size_t i = 0; i < dcD0->cum_size.size(); ++i)
                              if (dcD0->cum_read_size[i] + dcD0->cum_write_size[i] !=
                                  dcD0->cum_size[i])
                                  return false;
                          return true;
                      }(),
                  "read+write must equal cum_size at every index");
        }
        check("D1 split sums exist", dcD1 != nullptr, "no DataCell at D1");
        if (dcD1)
            check("D1 is a pure 4-byte read split",
                  dcD1->cum_read_size.back() == 4 &&
                      dcD1->cum_write_size.back() == 0,
                  "got read=" + std::to_string(dcD1->cum_read_size.back()) +
                      " write=" + std::to_string(dcD1->cum_write_size.back()));

        bool okD2 = false;
        uint32_t cD2 = cell_at(p, D2, &okD2);
        const auto *dcD2 = okD2 ? data_cell(cD2) : nullptr;
        check("D2 (unknown rw) split sums exist", dcD2 != nullptr,
              "no DataCell at D2");
        if (dcD2) {
            check("D2's unknown-direction access counts in cum_size only",
                  dcD2->cum_size.back() == 2 && dcD2->cum_read_size.back() == 0 &&
                      dcD2->cum_write_size.back() == 0,
                  "an unrecognised rw token must land in neither direction");
        }

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
        check("max_full_heat is 0 for a mixed-basis refusal",
              m.max_full_heat(UINT64_MAX) == 0,
              "a refused terrain must report no raw scale either");
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
              m.height_note.find("single-step residency") !=
                      std::string::npos &&
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
        check("G2: no data cells synthesized", m.data.empty(),
              "data not empty");
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

    // === Fixture J: a df recording's churn lands at its TRUE step (37 T3) ====
    // Before 37 the churn walk counted only `trace` offsets, so a df recording's
    // detected churn pinned at step 0. Now its df_step offsets are counted and the
    // churn join keys on the step's own rbase.
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4194304}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":16,\"rbase\":4194304}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":1,"
            "\"when\":5,\"bytes\":\"cc\"}\n" // churn after 2 df_steps
            "{\"k\":\"df_step\",\"step\":2,\"off\":0,\"rbase\":4194304}\n"
            "{\"k\":\"df_step\",\"step\":3,\"off\":16,\"rbase\":4194304}\n"
            "{\"k\":\"end\",\"events\":6,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        auto churn_cells = [](const Terrain &t) {
            size_t n = 0;
            for (uint32_t f : t.flags)
                if (f & TF_CHURN)
                    n++;
            return n;
        };
        check("J: a df recording detects churn", m.churn_present, "no churn");
        check("J: no churn before the bump (t=1)", churn_cells(m.slice(1)) == 0,
              "churn landed too early (the step-0 bug?)");
        check("J: churn appears at the TRUE step (t=2), not step 0",
              churn_cells(m.slice(2)) > 0, "churn did not land at step 2");
        steps_explained("J", m);
    }

    // === Fixture K: two tagged spans ⇒ two DISTINCT, correct churn steps =====
    {
        Recording rec =
            mk_rec("{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-"
                   "dataflow\","
                   "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
                   "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,"
                   "\"version\":0,"
                   "\"when\":1,\"bytes\":\"90\"}\n"
                   "{\"k\":\"codeimage\",\"base\":8388608,\"len\":256,"
                   "\"version\":0,"
                   "\"when\":1,\"bytes\":\"90\"}\n"
                   "{\"k\":\"df_step\",\"step\":0,\"off\":0,\"rbase\":4194304}"
                   "\n" // A
                   "{\"k\":\"df_step\",\"step\":1,\"off\":0,\"rbase\":8388608}"
                   "\n" // B
                   "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,"
                   "\"version\":1," // A churn @2
                   "\"when\":5,\"bytes\":\"cc\"}\n"
                   "{\"k\":\"df_step\",\"step\":2,\"off\":16,\"rbase\":4194304}"
                   "\n" // A
                   "{\"k\":\"df_step\",\"step\":3,\"off\":16,\"rbase\":8388608}"
                   "\n" // B
                   "{\"k\":\"codeimage\",\"base\":8388608,\"len\":256,"
                   "\"version\":1," // B churn @4
                   "\"when\":9,\"bytes\":\"cc\"}\n"
                   "{\"k\":\"df_step\",\"step\":4,\"off\":0,\"rbase\":8388608}"
                   "\n" // B
                   "{\"k\":\"end\",\"events\":9,\"truncated\":false,"
                   "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        bool ok = false;
        uint32_t cA = cell_at(p, 0x400000, &ok); // span A, off 0 (steps 0)
        uint32_t cB = cell_at(p, 0x800000, &ok); // span B, off 0 (steps 1,4)
        auto churned = [&](uint32_t cell, uint64_t t) {
            return (m.slice(t).flags[cell] & TF_CHURN) != 0;
        };
        check("K: span A churns at ITS step (2), not before",
              !churned(cA, 1) && churned(cA, 2), "span A churn step wrong");
        check("K: span B churns at ITS step (4), not span A's (2)",
              !churned(cB, 3) && churned(cB, 4), "span B churn step wrong");
        check("K: the two spans churn at DISTINCT steps (rbase-keyed, not 0)",
              churned(cA, 2) && !churned(cB, 2), "spans share a churn step");
        steps_explained("K", m);
    }

    // === Fixture M: a single-span df capture whose offsets ALL clamp out =====
    // (adversarial-review finding) The df rung must NOT advertise "df residency"
    // over a zero-cell plane: height_source stays unset and anchor_error says why,
    // so steps_explained holds — the same fidelity guard the trace rung already had.
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":16,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n" // a tiny 16-byte span
            "{\"k\":\"df_step\",\"step\":0,\"off\":4096,\"rbase\":4194304}\n"
            "{\"k\":\"df_step\",\"step\":1,\"off\":8192,\"rbase\":4194304}\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        Projection p = build_projection(regions_from_codeimage(rec));
        TerrainModel m = build_terrain(p, rec);
        check("M: nsteps is real (the time axis survives)", m.nsteps == 2,
              "got " + std::to_string(m.nsteps));
        check("M: no cells placed (every offset clamped out)", m.code.empty(),
              "got " + std::to_string(m.code.size()) + " cells");
        check(
            "M: height_source does NOT claim df residency over an empty plane",
            m.height_source != "df_step", "got '" + m.height_source + "'");
        check("M: anchor_error says why (never a silent flat plane)",
              !m.anchor_error.empty(), "silent empty plane over real steps");
        check("M: plane is flat", nonzero(m.full()) == 0,
              "cells on an empty df");
        steps_explained("M", m);
    }

    // === Fixture N: T1 (44-faithful-city-phase-a) — kind_by_cell, 2+ kinds ===
    // A code region (from codeimage) plus a manually-added data region (the
    // shape 07's maps snapshot will add later; test_terrain's own fixture C/D
    // pattern for a non-code region) — kind_by_cell must name each cell's
    // OWNING region's kind correctly, stay byte-stable across slice(t) (it is
    // built once, not re-derived), and carry the sentinel off-domain.
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"end\",\"events\":2,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        std::vector<Region> regs = regions_from_codeimage(rec);
        check("N: one code region from codeimage", regs.size() == 1,
              "got " + std::to_string(regs.size()));
        Region data;
        data.base = D0;
        data.len = 4096;
        data.kind = Region::Data;
        data.label = "heap";
        regs.push_back(data);
        Region stack;
        stack.base = 0x700000;
        stack.len = 4096;
        stack.kind = Region::Stack;
        stack.label = "[stack]";
        regs.push_back(stack);
        Projection p = build_projection(regs);
        TerrainModel m = build_terrain(p, rec);

        check("N: kind_by_cell sized w*h",
              m.kind_by_cell.size() == static_cast<size_t>(m.w) * m.h,
              "got " + std::to_string(m.kind_by_cell.size()) + " want " +
                  std::to_string(static_cast<size_t>(m.w) * m.h));

        bool ok = false;
        uint32_t cCode = cell_at(p, A0, &ok);
        check("N: A0 projects (test setup)", ok, "A0 did not project");
        uint32_t cData = cell_at(p, D0, &ok);
        check("N: D0 projects (test setup)", ok, "D0 did not project");
        uint32_t cStack = cell_at(p, 0x700000, &ok);
        check("N: stack addr projects (test setup)", ok,
              "stack addr did not project");

        check("N: the code cell carries Region::Code",
              m.kind_by_cell[cCode] == static_cast<uint8_t>(Region::Code),
              "got kind " + std::to_string(m.kind_by_cell[cCode]));
        check("N: the data cell carries Region::Data",
              m.kind_by_cell[cData] == static_cast<uint8_t>(Region::Data),
              "got kind " + std::to_string(m.kind_by_cell[cData]));
        check("N: the stack cell carries Region::Stack",
              m.kind_by_cell[cStack] == static_cast<uint8_t>(Region::Stack),
              "got kind " + std::to_string(m.kind_by_cell[cStack]));

        // An off-domain cell carries the sentinel, NEVER Region::Unknown's
        // raw value (5) — the two are different concepts (T1 step 2).
        long off_domain = -1;
        for (size_t i = 0; i < m.kind_by_cell.size(); i++)
            if (m.kind_by_cell[i] == kKindByCellNone) {
                off_domain = static_cast<long>(i);
                break;
            }
        check("N: an off-domain cell exists on this plane (test setup)",
              off_domain >= 0, "every cell claimed a region — check math");
        check("N: kKindByCellNone != Region::Unknown's raw value",
              kKindByCellNone != static_cast<uint8_t>(Region::Unknown),
              "the sentinel must be distinguishable from a real Unknown kind");

        // Byte-stable across slice(t) calls (same weave, different t): T1's
        // Done-when. kind_by_cell is a TerrainModel field, not part of
        // Terrain — slicing at different t must not touch it at all.
        std::vector<uint8_t> before = m.kind_by_cell;
        (void)m.slice(0);
        (void)m.slice(1);
        (void)m.full();
        check("N: kind_by_cell is byte-stable across slice(t) calls",
              m.kind_by_cell == before,
              "slice(t) must never mutate the slice-invariant kind map");
    }

    // === Fixture O: 54 T1 — observed_data_spans places an unmapped access ===
    // A mem access at an address no codeimage/data region covers: without the
    // extension it projects nowhere and no DataCell can ever exist; with the
    // spans appended to the region set before build_projection, the SAME
    // recording places exactly one DataCell (assert the count, per the brief).
    {
        Recording rec = mk_rec(
            "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-dataflow\","
            "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
            "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
            "\"when\":1,\"bytes\":\"90\"}\n"
            "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
            "{\"k\":\"mem\",\"step\":0,\"ea\":104857600,\"size\":8,\"rw\":\"w\"}"
            "\n"
            "{\"k\":\"end\",\"events\":3,\"truncated\":false,"
            "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
        std::vector<Region> regs = regions_from_codeimage(rec);

        {
            Projection p = build_projection(regs);
            TerrainModel m = build_terrain(p, rec);
            check("O: without observed spans, the unmapped access places no "
                  "cell",
                  m.data.empty(),
                  "got " + std::to_string(m.data.size()) + " data cells");
        }
        {
            std::string note;
            std::vector<Region> obs = observed_data_spans(rec, regs, &note);
            check("O: exactly one observed span for one isolated access",
                  obs.size() == 1, "got " + std::to_string(obs.size()));
            check("O: every observed span is labelled Unknown/observed data",
                  !obs.empty() && obs[0].kind == Region::Unknown &&
                      obs[0].label == "observed data",
                  "an observed span must never claim allocation structure");
            check("O: the span note explains the derivation", !note.empty(),
                  "note must be non-empty once a span was derived");

            std::vector<Region> regs2 = regs;
            regs2.insert(regs2.end(), obs.begin(), obs.end());
            Projection p2 = build_projection(regs2);
            TerrainModel m2 = build_terrain(p2, rec);
            check("O: with observed spans, the same access places one cell",
                  m2.data.size() == 1,
                  "got " + std::to_string(m2.data.size()) + " data cells");
        }
        {
            // No mem, no abs df values: byte-identical to no observed spans.
            Recording rec2 = mk_rec(
                "{\"asmtrace\":1,\"provenance\":{\"backend\":\"ptrace-region\","
                "\"exact\":true,\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
                "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,"
                "\"version\":0,\"when\":1,\"bytes\":\"90\"}\n"
                "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
                "{\"k\":\"end\",\"events\":2,\"truncated\":false,"
                "\"drops\":{\"lost\":0,\"throttled\":false}}\n");
            std::vector<Region> r2 = regions_from_codeimage(rec2);
            std::string note2 = "sentinel";
            std::vector<Region> obs2 = observed_data_spans(rec2, r2, &note2);
            check("O: no mem/abs-df events yields no observed spans",
                  obs2.empty(), "got " + std::to_string(obs2.size()));
            check("O: the note is cleared (not left stale) when there is "
                  "nothing to derive",
                  note2.empty(), "got: " + note2);
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d terrain check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_terrain: all checks passed\n");
    return 0;
}
