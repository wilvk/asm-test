// test_datalayers.cpp — the memory data-cell family
// (58-memory-data-cell-family.md), all six tasks. Null harness, no display and
// no GL: this binary links the pure space/ models (terrain, projection,
// datacell, dataribbon, sediment) plus scene3d/hud.o for T1's chip contract,
// and NOTHING else — the same engine-free closure proof test_terrain.cpp makes
// for the terrain, now for the five layers over its DATA half (D4).
//
// A standalone TU rather than additions to test_terrain.cpp / test_shell.cpp:
// both are worked concurrently by other sessions, and a new file collides with
// nobody (the tree's own convention for a hot shared checkout).
//
// Fixtures are hand-built NDJSON recordings loaded through the real loader, so
// `seq`, basis and truncation behave exactly as a producer's would.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "scene3d/hud.h"
#include "space/datacell.h"
#include "space/dataribbon.h"
#include "space/projection.h"
#include "space/sediment.h"
#include "space/terrain.h"

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

// The SHIPPING projection composition — the exact one ui/shell.cpp performs
// (codeimage code regions + observed_data_spans over them). Every fixture here
// goes through it, so a layer that only works against a hand-built data region
// would fail these tests, not pass them.
static TerrainModel weave(const Recording &r, std::string *span_note = nullptr) {
    std::vector<Region> regs = regions_from_codeimage(r);
    std::string note;
    std::vector<Region> obs = observed_data_spans(r, regs, &note);
    regs.insert(regs.end(), obs.begin(), obs.end());
    Projection proj = build_projection(std::move(regs));
    proj.data_span_note = note;
    if (span_note)
        *span_note = note;
    return build_terrain(std::move(proj), r);
}

static const char kHeader[] =
    "{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":\"t\","
    "\"version\":\"0\"},\"provenance\":{\"backend\":\"t\",\"exact\":true,"
    "\"trust\":\"exact\"},\"arch\":\"x86_64\"}\n"
    "{\"k\":\"codeimage\",\"base\":1048576,\"len\":32,\"version\":0}\n";

// Four trace steps at 0x100000.. so `nsteps` is real and `mem` steps index into
// a covered stream.
static std::string trace_steps(int n) {
    std::string s;
    for (int i = 0; i < n; i++) {
        char b[128];
        std::snprintf(b, sizeof b,
                      "{\"k\":\"trace\",\"basis\":\"abs\",\"kind\":\"insn\","
                      "\"off\":%d}\n",
                      1048576 + (i % 8));
        s += b;
    }
    return s;
}

static std::string mem_ev(uint64_t step, uint64_t ea, uint64_t size,
                          const char *rw) {
    char b[160];
    std::snprintf(b, sizeof b,
                  "{\"k\":\"mem\",\"step\":%llu,\"ea\":%llu,\"size\":%llu,"
                  "\"rw\":\"%s\"}\n",
                  (unsigned long long)step, (unsigned long long)ea,
                  (unsigned long long)size, rw);
    return b;
}

static const char kEnd[] =
    "{\"k\":\"end\",\"events\":99,\"truncated\":false,\"drops\":{\"lost\":0,"
    "\"throttled\":false}}\n";
// A torn tail: no `end` footer at all (Recording::torn).

static bool has_chip(const std::vector<scene3d::PlacementChip> &cs,
                     const std::string &needle) {
    for (const auto &c : cs)
        if (c.text.find(needle) != std::string::npos)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// T1 — the data-cell HUD contract
// ---------------------------------------------------------------------------
static void t1_hud_contract() {
    // A: every access lands inside an observed span -> zero drops.
    {
        std::string nd = kHeader + trace_steps(12) + mem_ev(2, 0x200000, 8, "r") +
                         mem_ev(5, 0x200000, 8, "w") +
                         mem_ev(8, 0x200040, 8, "r") + kEnd;
        Recording r = mk_rec(nd);
        std::string note;
        TerrainModel m = weave(r, &note);
        check("T1 A: mem_present", m.mem_present, "the fixture carries `mem`");
        check("T1 A: every access considered", m.mem_accesses == 3,
              "got " + std::to_string(m.mem_accesses));
        check("T1 A: no access dropped (54 T1's spans cover them)",
              m.mem_dropped == 0, "got " + std::to_string(m.mem_dropped));
        check("T1 A: the data half actually fired from the shipping "
              "composition",
              !m.data.empty(), "no DataCell was built");
        check("T1 A: the span note explains the derivation", !note.empty(),
              "empty data_span_note");

        auto cs = scene3d::placement_chips(m, TrajectorySet{});
        check("T1 A: chip states the rich rung",
              has_chip(cs, "rich: per-access memory"), "no rich chip");
        check("T1 A: chip states the span count + threshold",
              has_chip(cs, "data spans: 1 observed") &&
                  has_chip(cs, "4096-byte gap threshold"),
              "no span chip");
        check("T1 A: chip states every access placed",
              has_chip(cs, "3 of 3 memory accesses placed"),
              "no placement census chip");
    }

    // B: an access outside every span the projection knows about is COUNTED,
    // never silent. Built by handing build_terrain a code-only projection —
    // the pre-54 shape, which is exactly the regression this counter exists to
    // catch if span clustering ever breaks.
    {
        std::string nd = kHeader + trace_steps(12) + mem_ev(2, 0x200000, 8, "r") +
                         mem_ev(5, 0x300000, 8, "w") + kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m =
            build_terrain(build_projection(regions_from_codeimage(r)), r);
        check("T1 B: both accesses considered", m.mem_accesses == 2,
              "got " + std::to_string(m.mem_accesses));
        check("T1 B: both dropped (code-only projection maps neither)",
              m.mem_dropped == 2, "got " + std::to_string(m.mem_dropped));
        auto cs = scene3d::placement_chips(m, TrajectorySet{});
        check("T1 B: a fully off-plane data rung is a REFUSAL chip",
              has_chip(cs, "2 of 2 memory accesses OFF-PLANE"),
              "no off-plane chip");
        bool bad = false;
        for (const auto &c : cs)
            if (c.text.find("OFF-PLANE") != std::string::npos)
                bad = c.sev == scene3d::PlacementChip::Bad;
        check("T1 B: graded Bad when NOTHING placed", bad,
              "an all-dropped data rung must not read as a warning");
    }

    // C: exactly N of M drop when N addresses fall outside the spans.
    {
        std::string nd = kHeader + trace_steps(12) + mem_ev(2, 0x200000, 8, "r") +
                         mem_ev(5, 0x200008, 8, "w") +
                         mem_ev(8, 0x900000, 8, "r") + kEnd;
        Recording r = mk_rec(nd);
        // The spans are derived from ALL observed addresses, so all three
        // place. Drop one deliberately by clipping the projection to the
        // first span only — the honest "the clustering missed this" shape.
        std::vector<Region> regs = regions_from_codeimage(r);
        std::vector<Region> obs = observed_data_spans(r, regs, nullptr);
        check("T1 C: two distinct observed spans (0x200000 and 0x900000)",
              obs.size() == 2, "got " + std::to_string(obs.size()));
        if (obs.size() == 2) {
            regs.push_back(obs[0]); // keep only the low span
            TerrainModel m = build_terrain(build_projection(std::move(regs)), r);
            check("T1 C: exactly one of three dropped", m.mem_dropped == 1,
                  "got " + std::to_string(m.mem_dropped));
            auto cs = scene3d::placement_chips(m, TrajectorySet{});
            check("T1 C: the partial drop is a warning, and says 1 of 3",
                  has_chip(cs, "1 of 3 memory accesses OFF-PLANE"),
                  "no partial off-plane chip");
        }
    }

    // D: a `mem`-less recording shows the coarse chip, in mem_note's own words.
    {
        std::string nd = kHeader + trace_steps(6) + kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m = weave(r);
        check("T1 D: mem absent", !m.mem_present, "unexpected mem");
        check("T1 D: counters stay zero with nothing to place",
              m.mem_accesses == 0 && m.mem_dropped == 0, "non-zero census");
        auto cs = scene3d::placement_chips(m, TrajectorySet{});
        check("T1 D: the coarse chip reuses mem_note verbatim",
              has_chip(cs, "coarse: no per-access memory stream"),
              "the absent case invented new wording");
        check("T1 D: no placement census chip when there was nothing to place",
              !has_chip(cs, "memory accesses"), "a census over zero accesses");
        check("T1 D: no span chip when no address was observed",
              !has_chip(cs, "data spans:"), "a span chip with no spans");
    }
}

// A cell in a built layer, found by the address it was accessed at. Returns
// nullptr when the address placed nowhere (which the caller then asserts on).
template <typename Layer>
static const typename Layer::value_type *
find_at(const Layer &cells, const Projection &p, uint32_t w, uint32_t h,
        uint64_t addr) {
    float u = 0, v = 0;
    if (!p.project(addr, &u, &v))
        return nullptr;
    uint32_t x = static_cast<uint32_t>(u * w), y = static_cast<uint32_t>(v * h);
    if (x >= w)
        x = w - 1;
    if (y >= h)
        y = h - 1;
    const uint32_t cell = y * w + x;
    for (const auto &c : cells)
        if (c.cell == cell)
            return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------
// T2 — read/write twin relief
// ---------------------------------------------------------------------------
static void t2_twin_relief() {
    // The fixture: one read-only cell, one write-only cell, one mixed cell,
    // and one cell whose accesses carry an unrecognised direction token.
    const uint64_t kRO = 0x200000, kWO = 0x200040, kRW = 0x200080,
                   kUD = 0x2000c0;
    std::string nd = kHeader + trace_steps(20) + mem_ev(1, kRO, 8, "r") +
                     mem_ev(2, kRO, 8, "r") + mem_ev(3, kWO, 4, "w") +
                     mem_ev(4, kRW, 8, "r") + mem_ev(5, kRW, 16, "w") +
                     mem_ev(6, kUD, 32, "?") + kEnd;
    Recording r = mk_rec(nd);
    TerrainModel m = weave(r);
    check("T2: the fixture placed four data cells", m.data.size() == 4,
          "got " + std::to_string(m.data.size()));

    DataReliefLayer L = build_data_relief(m, UINT64_MAX);
    check("T2: one relief cell per touched data cell", L.cells.size() == 4,
          "got " + std::to_string(L.cells.size()));

    const ReliefCell *ro = find_at(L.cells, m.proj, m.w, m.h, kRO);
    const ReliefCell *wo = find_at(L.cells, m.proj, m.w, m.h, kWO);
    const ReliefCell *rw = find_at(L.cells, m.proj, m.w, m.h, kRW);
    const ReliefCell *ud = find_at(L.cells, m.proj, m.w, m.h, kUD);
    check("T2: every fixture address placed", ro && wo && rw && ud,
          "one of the four cells did not place");
    if (!(ro && wo && rw && ud))
        return;

    // THE central rule: a read-only fixture produces a peak and NO write
    // surface — the surface is ABSENT, asserted as absent, not as zero.
    check("T2: read-only cell has a read surface", ro->has_read, "no peak");
    check("T2: read-only cell's WRITE surface is ABSENT (not zero)",
          !ro->has_write, "a write surface appeared where none was recorded");
    check("T2: read-only cell's read magnitude is the observed byte sum",
          ro->read_bytes == 16 && ro->write_bytes == 0,
          "got " + std::to_string(ro->read_bytes));
    check("T2: read-only cell's shape", ro->shape() == ReliefShape::ReadOnly,
          "wrong shape");

    check("T2: write-only cell is the mirror",
          wo->has_write && !wo->has_read && wo->write_bytes == 4,
          "the write-only mirror did not hold");
    check("T2: write-only cell's shape", wo->shape() == ReliefShape::WriteOnly,
          "wrong shape");

    check("T2: mixed cell has BOTH surfaces",
          rw->has_read && rw->has_write && rw->read_bytes == 8 &&
              rw->write_bytes == 16,
          "the mixed cell lost a direction");
    check("T2: mixed cell's shape", rw->shape() == ReliefShape::ReadWrite,
          "wrong shape");

    // 54 T2's unknown-direction access contributes to NEITHER surface, and is
    // counted rather than vanishing.
    check("T2: an unknown-direction access feeds NEITHER surface",
          !ud->has_read && !ud->has_write && ud->read_bytes == 0 &&
              ud->write_bytes == 0,
          "an unrecognised rw token was folded into a direction");
    check("T2: its bytes are COUNTED as undirected, not dropped",
          ud->unknown_bytes == 32, "got " + std::to_string(ud->unknown_bytes));
    check("T2: the layer totals the undirected traffic",
          L.unknown_direction_bytes == 32 && L.unknown_direction_cells == 1,
          "the layer lost the undirected traffic");
    check("T2: shape counts add up",
          L.read_only_cells == 1 && L.write_only_cells == 1 &&
              L.read_write_cells == 1 && L.undirected_cells == 1,
          "shape census wrong");

    // The playhead cuts both surfaces. At t=4 the mixed cell has seen its read
    // but not yet its write — so its write surface must still be ABSENT.
    {
        DataReliefLayer at4 = build_data_relief(m, 4);
        const ReliefCell *c = find_at(at4.cells, m.proj, m.w, m.h, kRW);
        check("T2: at t=4 the mixed cell is read-only SO FAR",
              c && c->has_read && !c->has_write,
              "the playhead did not cut the write surface");
        const ReliefCell *unseen = find_at(at4.cells, m.proj, m.w, m.h, kUD);
        check("T2: a cell not yet touched at this slice produces NO entry",
              unseen == nullptr,
              "an untouched-yet cell appeared as a zero-height entry");
    }

    // A torn capture floors BOTH surfaces (never one of them).
    {
        std::string torn_nd = kHeader + trace_steps(8) +
                              mem_ev(1, kRO, 8, "r") + mem_ev(2, kWO, 8, "w");
        // no `end` footer at all => Recording::torn
        Recording tr = mk_rec(torn_nd);
        TerrainModel tm = weave(tr);
        check("T2: the torn fixture is torn", tm.torn, "not torn");
        DataReliefLayer TL = build_data_relief(tm, UINT64_MAX);
        check("T2: TORN floors the layer", TL.torn, "layer not flagged torn");
        bool all = !TL.cells.empty();
        for (const ReliefCell &c : TL.cells)
            if (!c.torn)
                all = false;
        check("T2: TORN floors BOTH surfaces of EVERY cell", all,
              "a torn capture left some surface unflagged");
    }

    // The wording: "observed", and no RMW claimed where one direction only.
    {
        const std::string note = data_relief_note();
        check("T2: the note says OBSERVED",
              note.find("OBSERVED") != std::string::npos ||
                  note.find("observed") != std::string::npos,
              "the layer note does not say observed");
        const std::string ro_lab = relief_shape_label(ReliefShape::ReadOnly);
        check("T2: the read-only label names the ABSENT write surface",
              ro_lab.find("ABSENT") != std::string::npos,
              "the read-only label does not state the absence rule");
        const std::string rw_lab = relief_shape_label(ReliefShape::ReadWrite);
        check("T2: the both-directions label does not INFER read-modify-write",
              rw_lab.find("NOT inferred") != std::string::npos,
              "the mixed label claims an RMW that was not recorded");
    }

    // The legend is exhaustive over ReliefShape and shares the model's words.
    {
        const auto &sw = scene3d::relief_shape_swatches();
        check("T2: legend has one row per ReliefShape", sw.size() == 4,
              "got " + std::to_string(sw.size()));
        bool matches = true;
        for (const auto &row : sw)
            if (row.label != relief_shape_label(row.shape))
                matches = false;
        check("T2: legend text IS the model's own label (one source of truth)",
              matches, "the legend re-spelled a shape's rule");
    }

    // The layer never fires for a recording with no `mem` — and it produces
    // NOTHING rather than a flat plane of zero-height cells.
    {
        std::string coarse = kHeader + trace_steps(6) + kEnd;
        Recording cr = mk_rec(coarse);
        TerrainModel cm = weave(cr);
        DataReliefLayer CL = build_data_relief(cm, UINT64_MAX);
        check("T2: no `mem` => an EMPTY layer, not a zero-height one",
              CL.cells.empty() && !CL.mem_present,
              "the coarse rung produced relief cells");
    }
}

// ---------------------------------------------------------------------------
// T3 — working-set tide
// ---------------------------------------------------------------------------
static void t3_working_set_tide() {
    // kHot is touched late (inside the window at t=40, W=10); kCold only
    // early; kFuture only after t. All three place in the same span.
    const uint64_t kHot = 0x200000, kCold = 0x200040, kFuture = 0x200080;
    std::string nd = kHeader + trace_steps(60) + mem_ev(2, kCold, 8, "r") +
                     mem_ev(3, kCold, 8, "w") + mem_ev(34, kHot, 16, "r") +
                     mem_ev(38, kHot, 8, "w") + mem_ev(50, kFuture, 8, "r") +
                     kEnd;
    Recording r = mk_rec(nd);
    TerrainModel m = weave(r);

    const uint64_t t = 40, W = 10;
    WorkingSetTide T = build_working_set_tide(m, t, W);
    check("T3: the window and its unit ride on the model",
          T.window == W && T.t == t, "window/t not carried");

    const TideCell *hot = find_at(T.cells, m.proj, m.w, m.h, kHot);
    const TideCell *cold = find_at(T.cells, m.proj, m.w, m.h, kCold);
    const TideCell *fut = find_at(T.cells, m.proj, m.w, m.h, kFuture);
    check("T3: the hot and cold cells both produce entries", hot && cold,
          "a touched cell produced no entry");
    check("T3: a cell NOT YET touched at this slice produces NOTHING",
          fut == nullptr,
          "a not-yet-touched cell appeared (it must be indistinguishable from "
          "never-touched at this slice, and BOTH must be absent)");
    if (!(hot && cold))
        return;

    check("T3: the live cell's window mass is the observed byte delta",
          hot->live && hot->live_bytes == 24,
          "got " + std::to_string(hot->live_bytes));
    check("T3: the live cell is not marked cold", !hot->cold, "live and cold");
    check("T3: the live cell's direction split survives the window",
          hot->win_read_bytes == 16 && hot->win_write_bytes == 8,
          "the window lost a direction");

    // THE central rule for this layer: a cell touched only OUTSIDE the window
    // has zero live height and a NON-ZERO watermark. Zero would say it was
    // never touched.
    check("T3: the cold cell has zero live height",
          cold->cold && !cold->live && cold->live_bytes == 0 &&
              cold->live_height == 0.0,
          "a cold cell still carried live mass");
    check("T3: the cold cell's WATERMARK is non-zero (a decay, never a zero)",
          cold->watermark_height > 0.0,
          "a cold cell decayed to zero, which reads as never-touched");
    check("T3: the layer counts the live/cold split",
          T.live_cells == 1 && T.cold_cells == 1,
          "live=" + std::to_string(T.live_cells) + " cold=" +
              std::to_string(T.cold_cells));

    // The windowed delta equals a brute-force sum over the same range, for
    // several (t, W). This is the assertion that the two binary searches are
    // not an approximation of the thing they replace.
    {
        bool all_ok = true;
        for (uint64_t tt : {5ull, 20ull, 40ull, 59ull}) {
            for (uint64_t ww : {1ull, 4ull, 16ull, 1000ull}) {
                WorkingSetTide X = build_working_set_tide(m, tt, ww);
                for (const TerrainModel::DataCell &dc : m.data) {
                    uint64_t brute = 0;
                    for (size_t i = 0; i < dc.steps.size(); i++) {
                        const uint64_t st = dc.steps[i];
                        if (st > tt)
                            continue;
                        if (tt > ww && st <= tt - ww)
                            continue;
                        // the per-access size, recovered from the prefix sum
                        brute += dc.cum_size[i] -
                                 (i > 0 ? dc.cum_size[i - 1] : uint64_t{0});
                    }
                    const TideCell *c = nullptr;
                    for (const TideCell &x : X.cells)
                        if (x.cell == dc.cell)
                            c = &x;
                    const uint64_t got = c ? c->live_bytes : 0;
                    if (got != brute)
                        all_ok = false;
                }
            }
        }
        check("T3: the windowed delta equals a brute-force sum over the same "
              "range, for every (t, W) probed",
              all_ok, "the two binary searches disagree with a linear scan");
    }

    // The degraded path is LABELLED differently from the split path. Built
    // from a fixture whose accesses carry no recognisable direction token at
    // all — the only case in which no ratio exists to draw.
    {
        std::string und = kHeader + trace_steps(20) +
                          mem_ev(2, kHot, 8, "?") + mem_ev(9, kHot, 8, "?") +
                          kEnd;
        Recording ur = mk_rec(und);
        TerrainModel um = weave(ur);
        WorkingSetTide UT = build_working_set_tide(um, 12, 8);
        check("T3: with no recognisable direction the tint DEGRADES",
              UT.tint == TideTint::RwFlagOnly, "the split tint was claimed");
        const std::string deg = tide_tint_label(TideTint::RwFlagOnly);
        const std::string split = tide_tint_label(TideTint::Split);
        check("T3: the degraded label differs from the split label",
              deg != split, "the two tint sources share one label");
        check("T3: the degraded label refuses to call itself a ratio",
              deg.find("no ratio") != std::string::npos &&
                  deg.find("prefix OR") != std::string::npos,
              "the degraded label does not state what it actually is");
        check("T3: the split fixture claims the split tint",
              build_working_set_tide(m, t, W).tint == TideTint::Split,
              "a recording WITH directions was degraded");
    }

    // The note names its axis and its unit — a bare number is the review's own
    // top finding.
    {
        const std::string note = tide_note(T);
        check("T3: the note names the TRACE-TIME axis",
              note.find("TRACE-TIME") != std::string::npos,
              "the note does not name the axis");
        check("T3: the note names the window's unit and value",
              note.find(std::to_string(W)) != std::string::npos &&
                  note.find("steps") != std::string::npos,
              "the note states a bare number");
        check("T3: the note states the watermark rule",
              note.find("WATERMARK") != std::string::npos &&
                  note.find("never to zero") != std::string::npos,
              "the note does not state that cold is a decay, not a zero");
    }

    // Torn floors the window and the flag survives onto every cell.
    {
        std::string tn = kHeader + trace_steps(20) + mem_ev(2, kHot, 8, "r");
        Recording trr = mk_rec(tn); // no `end` footer => torn
        TerrainModel tm = weave(trr);
        WorkingSetTide TT = build_working_set_tide(tm, 10, 8);
        bool all = !TT.cells.empty();
        for (const TideCell &c : TT.cells)
            if (!c.torn)
                all = false;
        check("T3: TORN floors the window on every cell", TT.torn && all,
              "a torn window was presented as a measurement");
        check("T3: the torn note says so",
              tide_note(TT).find("TORN") != std::string::npos,
              "the note hid the truncation");
    }

    // Absent `mem` => an empty layer, never a flat plane of zero cells.
    {
        std::string coarse = kHeader + trace_steps(6) + kEnd;
        Recording cr = mk_rec(coarse);
        TerrainModel cm = weave(cr);
        WorkingSetTide CT = build_working_set_tide(cm, 5, 4);
        check("T3: no `mem` => an EMPTY tide, not a zero-height one",
              CT.cells.empty() && !CT.mem_present,
              "the coarse rung produced tide cells");
    }
}

// ---------------------------------------------------------------------------
// T4 — observed-lifetime pillars
// ---------------------------------------------------------------------------
static void t4_lifetime_pillars() {
    // kSpan is touched at steps 5 and 900 — ONE pillar spanning 5..900, never
    // two. kStub is touched exactly once.
    const uint64_t kSpan = 0x200000, kStub = 0x200040;
    std::string nd = kHeader + trace_steps(950) + mem_ev(5, kSpan, 8, "r") +
                     mem_ev(900, kSpan, 8, "w") + mem_ev(400, kStub, 8, "r") +
                     kEnd;
    Recording r = mk_rec(nd);
    TerrainModel m = weave(r);
    LifetimePillars P = build_lifetime_pillars(m);

    const LifetimePillar *sp = find_at(P.pillars, m.proj, m.w, m.h, kSpan);
    const LifetimePillar *st = find_at(P.pillars, m.proj, m.w, m.h, kStub);
    check("T4: both touched cells produce a pillar", sp && st,
          "a touched cell produced no pillar");
    if (!(sp && st))
        return;

    check("T4: a cell touched at 5 and 900 yields ONE pillar spanning 5..900",
          sp->first_step == 5 && sp->last_step == 900 && sp->touches == 2,
          "got " + std::to_string(sp->first_step) + ".." +
              std::to_string(sp->last_step));
    check("T4: exactly two pillars for two touched cells", P.pillars.size() == 2,
          "got " + std::to_string(P.pillars.size()) + " (a second pillar for "
          "the same cell would mean the interval was split)");
    check("T4: a cell touched once yields a STUB with a STATED zero-length "
          "interval",
          st->zero_length && st->first_step == st->last_step &&
              st->first_step == 400,
          "the stub's interval was silently widened");
    check("T4: the spanning pillar is not zero-length", !sp->zero_length,
          "a real interval was reported as a stub");
    check("T4: dominance comes from the OBSERVED byte split",
          sp->has_read && sp->has_write &&
              sp->dominance == PillarDominance::Balanced,
          "8r/8w should be balanced");

    // A never-touched cell places NOTHING (not a zero-length nub at the
    // origin). Asserted through the shipping composition: an address the
    // recording never touched has no DataCell at all, so no pillar.
    {
        bool any_at_zero = false;
        for (const LifetimePillar &p : P.pillars)
            if (p.touches == 0)
                any_at_zero = true;
        check("T4: no pillar exists for an untouched cell", !any_at_zero,
              "a zero-touch pillar appeared");
    }

    // A torn recording OPEN-TOPS the pillars whose last touch is at the tail —
    // and only those.
    {
        std::string tn = kHeader + trace_steps(50) + mem_ev(2, kStub, 8, "r") +
                         mem_ev(4, kSpan, 8, "r") + mem_ev(40, kSpan, 8, "w");
        Recording trr = mk_rec(tn); // no `end` footer => torn
        TerrainModel tm = weave(trr);
        check("T4: the torn fixture is torn", tm.torn, "not torn");
        LifetimePillars TP = build_lifetime_pillars(tm);
        const LifetimePillar *tail = find_at(TP.pillars, tm.proj, tm.w, tm.h,
                                             kSpan);
        const LifetimePillar *early = find_at(TP.pillars, tm.proj, tm.w, tm.h,
                                              kStub);
        check("T4: the pillar whose last touch is at the tail is OPEN-TOPPED",
              tail && tail->open_top, "the torn tail was capped, not floored");
        check("T4: a pillar that ended BEFORE the tail is NOT open-topped",
              early && !early->open_top,
              "torn-ness was smeared over every pillar");
        check("T4: the layer counts the open-topped pillars",
              TP.open_topped == 1,
              "got " + std::to_string(TP.open_topped));
    }

    // THE load-bearing wording assertion of this whole brief.
    {
        std::string lab = lifetime_pillar_label();
        std::string lower;
        for (char c : lab)
            lower += static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        check("T4: the label contains \"observed\"",
              lower.find("observed") != std::string::npos,
              "the pillar label does not say it measures observed touches");
        check("T4: the label does NOT contain \"allocat\"",
              lower.find("allocat") == std::string::npos ||
                  lower.find("not an allocation") != std::string::npos,
              "the label claims an allocation lifetime nothing recorded");
        // Stronger: the ONLY permitted occurrence is the explicit denial.
        size_t pos = lower.find("allocat");
        bool ok = true;
        while (pos != std::string::npos) {
            const std::string ctx = lower.substr(pos > 8 ? pos - 8 : 0, 24);
            if (ctx.find("not an alloc") == std::string::npos &&
                ctx.find("no producer") == std::string::npos &&
                ctx.find("emits allocat") == std::string::npos)
                ok = false;
            pos = lower.find("allocat", pos + 1);
        }
        check("T4: every mention of allocation is a DENIAL, never a claim", ok,
              "the label asserts something about allocation");
    }

    // Absent `mem` => no pillars at all.
    {
        std::string coarse = kHeader + trace_steps(6) + kEnd;
        Recording cr = mk_rec(coarse);
        TerrainModel cm = weave(cr);
        LifetimePillars CP = build_lifetime_pillars(cm);
        check("T4: no `mem` => no pillars, not zero-height ones",
              CP.pillars.empty() && !CP.mem_present,
              "the coarse rung produced pillars");
    }
}

// ---------------------------------------------------------------------------
// T5 — data-access worldline ribbon
// ---------------------------------------------------------------------------
// The largest Chebyshev plane distance any one segment covers — the "how far
// does one step of this access pattern travel on the plane" measure the shape
// claim rests on.
static uint32_t max_hop(const DataRibbon &R, uint32_t w) {
    uint32_t worst = 0;
    for (const RibbonSegment &sg : R.segs) {
        const uint32_t ca = R.verts[sg.a].cell, cb = R.verts[sg.b].cell;
        const uint32_t ax = ca % w, ay = ca / w, bx = cb % w, by = cb / w;
        const uint32_t dx = ax > bx ? ax - bx : bx - ax;
        const uint32_t dy = ay > by ? ay - by : by - ay;
        worst = std::max(worst, std::max(dx, dy));
    }
    return worst;
}

static void t5_access_ribbon() {
    uint32_t seq_max_hop = 0;
    // A SEQUENTIAL scan: eight consecutive 8-byte reads over one span. The
    // cells must be adjacent-or-equal in the compacted domain, which is the
    // locality property the Hilbert projection exists to preserve.
    {
        std::string nd = kHeader + trace_steps(24);
        for (int i = 0; i < 8; i++)
            nd += mem_ev(static_cast<uint64_t>(i + 1), 0x200000 + 8 * i, 8, "r");
        nd += kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m = weave(r);
        DataRibbon R = build_data_ribbon(r, m.proj);
        check("T5: the ribbon sourced from `mem`", R.source == RibbonSource::Mem,
              "wrong source");
        check("T5: one vertex per recorded access", R.verts.size() == 8,
              "got " + std::to_string(R.verts.size()));
        check("T5: every access placed", R.off_plane == 0,
              std::to_string(R.off_plane) + " off-plane");
        check("T5: seven segments join eight consecutive accesses",
              R.segs.size() == 7 && R.gaps == 0,
              "got " + std::to_string(R.segs.size()) + " segments, " +
                  std::to_string(R.gaps) + " gaps");
        check("T5: vertices are in recorded step order",
              [&] {
                  for (size_t i = 1; i < R.verts.size(); i++)
                      if (R.verts[i].step < R.verts[i - 1].step)
                          return false;
                  return true;
              }(),
              "the ribbon is not ordered by step");
        // A sequential scan hugs adjacent cells: no segment leaps between
        // observed-data spans (there is only one span here).
        check("T5: a sequential scan produces NO cross-span leaps",
              R.cross_region == 0,
              "a scan within one object was reported as non-locality");
        seq_max_hop = max_hop(R, m.w);
    }

    // A STRIDED pattern alternates: the documented zig-zag. Assert the shape
    // is DIFFERENT from the sequential one rather than pinning a Hilbert
    // curve's exact geometry (which is the projection's business, not this
    // layer's).
    {
        std::string nd = kHeader + trace_steps(24);
        for (int i = 0; i < 8; i++)
            nd +=
                mem_ev(static_cast<uint64_t>(i + 1), 0x200000 + 256 * i, 8, "r");
        nd += kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m = weave(r);
        DataRibbon R = build_data_ribbon(r, m.proj);
        check("T5: the strided fixture joins all eight",
              R.verts.size() == 8 && R.segs.size() == 7,
              "the stride lost a segment");
        // THE shape assertion, stated as a COMPARISON rather than as an
        // absolute plane distance: how far apart two consecutive cells land is
        // the Hilbert projection's business (it depends on the compacted
        // domain size and the clamped order), while "a stride travels further
        // than a scan" is THIS layer's claim and is what makes the pattern
        // legible as a shape at all.
        const uint32_t stride_max_hop = max_hop(R, m.w);
        check("T5: a strided pattern travels FURTHER per step than a scan",
              stride_max_hop > seq_max_hop,
              "a 256-byte stride hopped no further than a sequential scan (" +
                  std::to_string(stride_max_hop) + " vs " +
                  std::to_string(seq_max_hop) +
                  ") — the layer would show no shape at all");
    }

    // POINTER-CHASING across two distinct observed-data spans: the leap is
    // labelled GENUINE non-locality so a reader does not blame the compaction.
    {
        std::string nd = kHeader + trace_steps(24) +
                         mem_ev(1, 0x200000, 8, "r") +
                         mem_ev(2, 0x900000, 8, "r") +
                         mem_ev(3, 0x200008, 8, "r") + kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m = weave(r);
        DataRibbon R = build_data_ribbon(r, m.proj);
        check("T5: two distinct observed-data spans exist (test setup)",
              [&] {
                  size_t n = 0;
                  for (const Region &g : m.proj.regions)
                      if (g.label == kObservedDataLabel)
                          n++;
                  return n == 2;
              }(),
              "the fixture did not produce two spans");
        check("T5: a leap across distinct spans is FLAGGED as non-locality",
              R.cross_region == 2 && R.segs.size() == 2,
              "got " + std::to_string(R.cross_region) + " leaps of " +
                  std::to_string(R.segs.size()) + " segments");
        check("T5: the note names the leaps as GENUINE non-locality",
              data_ribbon_note(R).find("GENUINE") != std::string::npos,
              "a reader could blame the address compaction for the leap");
    }

    // A GAP in the step coverage produces a BREAK, never a joined segment. The
    // covered domain here is the trace's 6 steps; the access at step 40 sits
    // outside it, so the pair straddling the boundary is not joined.
    {
        std::string nd = kHeader + trace_steps(6) + mem_ev(1, 0x200000, 8, "r") +
                         mem_ev(2, 0x200008, 8, "r") +
                         mem_ev(40, 0x200010, 8, "r") + kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m = weave(r);
        DataRibbon R = build_data_ribbon(r, m.proj);
        check("T5: the covered step domain is the trace's own extent",
              R.covered_steps == 6,
              "got " + std::to_string(R.covered_steps));
        check("T5: a gap in the step coverage BREAKS the ribbon",
              R.segs.size() == 1 && R.gaps == 1,
              "got " + std::to_string(R.segs.size()) + " segments and " +
                  std::to_string(R.gaps) +
                  " gaps — an interpolated join would draw an access that was "
                  "never recorded");
        check("T5: the out-of-domain access is still COUNTED as a vertex",
              R.verts.size() == 3,
              "the access outside the covered domain was dropped, not gapped");
    }

    // An UNPLACEABLE access breaks the ribbon too, and is counted.
    {
        std::string nd = kHeader + trace_steps(24) +
                         mem_ev(1, 0x200000, 8, "r") +
                         mem_ev(2, 0x900000, 8, "r") +
                         mem_ev(3, 0x200008, 8, "r") + kEnd;
        Recording r = mk_rec(nd);
        // Deliberately build the projection from the CODE regions only, so the
        // middle access places nowhere — the honest "we know it happened, we
        // cannot say where" case.
        TerrainModel m =
            build_terrain(build_projection(regions_from_codeimage(r)), r);
        DataRibbon R = build_data_ribbon(r, m.proj);
        check("T5: unplaceable accesses are COUNTED, never dropped",
              R.verts.size() == 3 && R.off_plane == 3,
              "got " + std::to_string(R.off_plane) + " off-plane of " +
                  std::to_string(R.verts.size()));
        check("T5: no segment is drawn across an unplaceable access",
              R.segs.empty() && R.gaps == 2, "a segment spanned a lost vertex");
        check("T5: the note states the off-plane count",
              data_ribbon_note(R).find("could not be placed") !=
                  std::string::npos,
              "the off-plane accesses vanished silently");
    }

    // The FALLBACK source is labelled DIFFERENTLY from `mem`.
    {
        const std::string ma = ribbon_source_label(RibbonSource::Mem);
        const std::string df = ribbon_source_label(RibbonSource::DataflowAbs);
        check("T5: the two sources carry different labels", ma != df,
              "the fallback source is presented as `mem`");
        check("T5: the fallback label says it is NOT the same population",
              df.find("NOT the same population") != std::string::npos,
              "the fallback label implies the two are interchangeable");
    }

    // Truncation CAPS the ribbon, and the cap is stated.
    {
        std::string nd = kHeader + trace_steps(24) +
                         mem_ev(1, 0x200000, 8, "r") + mem_ev(2, 0x200008, 8, "r");
        Recording r = mk_rec(nd); // no `end` footer => torn
        TerrainModel m = weave(r);
        DataRibbon R = build_data_ribbon(r, m.proj);
        check("T5: a torn recording CAPS the ribbon", R.capped,
              "the tail was presented as an end");
        check("T5: the note states the cap",
              data_ribbon_note(R).find("CAPPED") != std::string::npos,
              "the cap is invisible to the reader");
    }

    // The note names the layer it is NOT.
    {
        std::string nd = kHeader + trace_steps(6) + mem_ev(1, 0x200000, 8, "r") +
                         kEnd;
        Recording r = mk_rec(nd);
        TerrainModel m = weave(r);
        const std::string note = data_ribbon_note(build_data_ribbon(r, m.proj));
        check("T5: the note distinguishes itself from the access spurs",
              note.find("NOT the PC->data access spurs") != std::string::npos,
              "the ribbon and the spurs could read as one layer");
    }

    // No `mem` and no abs dataflow => an empty ribbon that SAYS so.
    {
        std::string coarse = kHeader + trace_steps(6) + kEnd;
        Recording cr = mk_rec(coarse);
        TerrainModel cm = weave(cr);
        DataRibbon CR = build_data_ribbon(cr, cm.proj);
        check("T5: no source => an empty ribbon, labelled",
              CR.verts.empty() && CR.segs.empty() &&
                  CR.source == RibbonSource::None,
              "an empty ribbon claimed a source");
    }
}

// ---------------------------------------------------------------------------
// T6 — residency sediment columns
// ---------------------------------------------------------------------------
static const SedimentColumn *col_at(const SedimentColumns &C,
                                    const Projection &p, uint32_t w, uint32_t h,
                                    uint64_t addr) {
    return find_at(C.exact, p, w, h, addr);
}

static void t6_sediment_columns() {
    // kEarly is hit only in the first decile; kEven throughout. 100 trace
    // steps, so a 10-band split maps one band per decile exactly.
    const uint64_t kEarly = 0x200000, kEven = 0x200040;
    std::string nd = kHeader + trace_steps(100);
    for (int i = 0; i < 5; i++)
        nd += mem_ev(static_cast<uint64_t>(i), kEarly, 8, "r");
    for (int i = 0; i < 10; i++)
        nd += mem_ev(static_cast<uint64_t>(i * 10 + 3), kEven, 8, "r");
    nd += kEnd;
    Recording r = mk_rec(nd);
    TerrainModel m = weave(r);
    SedimentColumns C = build_sediment_columns(m, 10, 0);
    check("T6: the requested band count is honoured with no budget",
          C.bands == 10 && !C.degraded, "got " + std::to_string(C.bands));

    const SedimentColumn *early = col_at(C, m.proj, m.w, m.h, kEarly);
    const SedimentColumn *even = col_at(C, m.proj, m.w, m.h, kEven);
    check("T6: both hit cells produce a column", early && even,
          "a hit cell produced no column");
    if (!(early && even))
        return;

    check("T6: a cell hit only in the first decile produces bands ONLY there",
          early->bands.size() == 1 && early->bands[0].index == 0,
          "got " + std::to_string(early->bands.size()) + " bands");
    check("T6: a uniformly hit cell produces even bands",
          even->bands.size() == 10,
          "got " + std::to_string(even->bands.size()));
    check("T6: each of the even cell's bands carries exactly one hit",
          [&] {
              for (const SedimentBand &b : even->bands)
                  if (b.hits != 1)
                      return false;
              return true;
          }(),
          "the even fixture's bands are not even");

    // THE CONSERVATION ASSERTION: the band counts sum to the cell's total hit
    // count, for EVERY column and for several band counts. This is what proves
    // no binning step silently drops a measurement (D7).
    {
        bool ok = true;
        for (uint32_t bands : {1u, 2u, 3u, 7u, 10u, 16u, 64u}) {
            SedimentColumns X = build_sediment_columns(m, bands, 0);
            for (const SedimentColumn &c : X.exact) {
                uint64_t sum = 0;
                for (const SedimentBand &b : c.bands)
                    sum += b.hits;
                if (sum != c.total_hits)
                    ok = false;
            }
        }
        check("T6: band counts CONSERVE the cell's total hit count, at every "
              "band count probed",
              ok, "a binning step dropped a hit");
    }

    // A never-hit cell places NO column — not a zero nub, which would read as
    // a measurement. Asserted over the whole plane: every emitted column has
    // real hits and real bands.
    {
        bool ok = true;
        for (const SedimentColumn &c : C.exact)
            if (c.total_hits == 0 || c.bands.empty())
                ok = false;
        check("T6: every emitted column has hits and bands (a never-hit cell "
              "places NOTHING)",
              ok, "a zero-hit column appeared");
        check("T6: the plane has far more cells than columns (test setup)",
              static_cast<uint64_t>(m.w) * m.h > C.exact.size(),
              "the fixture does not exercise the absence rule");
    }

    // THE FRAME-BUDGET PATH, wired to the SAME should_degrade budget the 3D
    // scrub uses. Coarsen the BANDS, never the cell set.
    {
        const uint64_t touched = C.exact.size();
        check("T6: the fixture has columns to budget (test setup)", touched > 0,
              "no columns");
        // A budget that admits only ONE band per column.
        SedimentColumns D = build_sediment_columns(m, 16, touched);
        check("T6: an over-budget request COARSENS the band count",
              D.degraded && D.bands < D.bands_requested && D.bands >= 1,
              "got " + std::to_string(D.bands) + " of " +
                  std::to_string(D.bands_requested));
        check("T6: NO CELL IS DROPPED by the degrade — only resolution",
              D.exact.size() == C.exact.size(),
              "the budget deleted measurements instead of merging windows: " +
                  std::to_string(D.exact.size()) + " vs " +
                  std::to_string(C.exact.size()));
        check("T6: the degraded columns still conserve their hit counts",
              [&] {
                  for (const SedimentColumn &c : D.exact) {
                      uint64_t sum = 0;
                      for (const SedimentBand &b : c.bands)
                          sum += b.hits;
                      if (sum != c.total_hits)
                          return false;
                  }
                  return true;
              }(),
              "coarsening lost a hit");
        check("T6: the note STATES the band count and that it was coarsened",
              sediment_note(D).find("COARSENED") != std::string::npos &&
                  sediment_note(D).find(std::to_string(D.bands)) !=
                      std::string::npos,
              "a coarsened column is indistinguishable from a sparse one");
        // A generous budget must NOT degrade.
        SedimentColumns E = build_sediment_columns(m, 16, 1000000);
        check("T6: a generous budget leaves the request intact",
              !E.degraded && E.bands == 16, "degraded under a large budget");
    }

    // A TF_STAT cell's bands land in the STAT buffer, never in the exact one.
    // Built from the committed survey fixture, which carries real `survey`
    // edges and therefore a real TerrainModel::stat layer.
    {
        std::string sv =
            std::string(kHeader) +
            "{\"k\":\"survey\",\"sampler\":\"ibs-op\",\"edges\":[{\"from_"
            "addr\":1048576,\"to_addr\":1048580,\"count\":7,\"mispred\":0,"
            "\"is_return\":0}],\"samples\":100,\"lost\":0}\n" +
            trace_steps(8) + mem_ev(1, 0x200000, 8, "r") + kEnd;
        Recording sr = mk_rec(sv);
        TerrainModel sm = weave(sr);
        check("T6: the survey fixture built a stat layer (test setup)",
              sm.has_stat, "no stat terrain");
        SedimentColumns SC = build_sediment_columns(sm, 8, 0);
        check("T6: a TF_STAT cell's bands land in the STAT buffer",
              !SC.stat.empty(), "the survey produced no statistical column");
        // The isolation invariant is about the two POPULATIONS, not about the
        // cell sets: a cell can legitimately carry both exact residency and
        // survey residency (this fixture's survey edge lands on a traced code
        // cell precisely so the overlap is exercised). What must never happen
        // is the survey magnitude being SUMMED into the exact column — so the
        // check is that every exact column's hit total still equals the real
        // step count of its own CodeCell/DataCell, survey or no survey.
        check("T6: a survey magnitude is NEVER summed into an exact column",
              [&] {
                  for (const SedimentColumn &e : SC.exact) {
                      uint64_t real = 0;
                      if (const auto *cc = sm.code_at(e.cell))
                          real = cc->steps.size();
                      if (const auto *dc = sm.data_at(e.cell))
                          real = dc->steps.size();
                      if (e.total_hits != real)
                          return false;
                  }
                  return true;
              }(),
              "an exact column's hit count was inflated by survey residency");
        check("T6: the survey overlaps a traced cell here (test setup)",
              [&] {
                  for (const SedimentColumn &c : SC.stat)
                      for (const SedimentColumn &e : SC.exact)
                          if (c.cell == e.cell)
                              return true;
                  return false;
              }(),
              "the fixture does not exercise the overlap the invariant guards");
        check("T6: a survey column carries NO phase (one unattributed band)",
              SC.stat_has_no_phase && !SC.stat.empty() &&
                  SC.stat[0].bands.size() == 1,
              "the survey was given a fabricated temporal distribution");
        check("T6: the note states the survey's no-phase caveat",
              sediment_note(SC).find("NO phase") != std::string::npos,
              "a reader could take the survey column as a phase measurement");
    }

    // TORN caps the column and the note says so.
    {
        std::string tn = kHeader + trace_steps(20) + mem_ev(2, kEarly, 8, "r");
        Recording trr = mk_rec(tn); // no `end` footer => torn
        TerrainModel tm = weave(trr);
        SedimentColumns TC = build_sediment_columns(tm, 8, 0);
        bool all = !TC.exact.empty();
        for (const SedimentColumn &c : TC.exact)
            if (!c.torn_capped)
                all = false;
        check("T6: TORN caps every column", TC.torn && all,
              "a torn column was presented as complete");
        check("T6: the note says the top band is a floor",
              sediment_note(TC).find("floor") != std::string::npos,
              "the truncation is invisible");
    }

    // The layer covers CODE cells too, not only data — T6 has no `mem`
    // prerequisite and must work on a coarse recording.
    {
        std::string coarse = kHeader + trace_steps(40) + kEnd;
        Recording cr = mk_rec(coarse);
        TerrainModel cm = weave(cr);
        SedimentColumns CC = build_sediment_columns(cm, 8, 0);
        check("T6: a `mem`-less recording still produces CODE columns",
              !CC.exact.empty(),
              "the layer needs no `mem` and must not be empty without it");
        bool any_code = false;
        for (const SedimentColumn &c : CC.exact)
            if (!c.is_data)
                any_code = true;
        check("T6: those columns are code columns", any_code,
              "no code column was emitted");
    }
}

int main() {
    t1_hud_contract();
    t2_twin_relief();
    t3_working_set_tide();
    t4_lifetime_pillars();
    t5_access_ribbon();
    t6_sediment_columns();

    if (failures) {
        std::fprintf(stderr, "%d data-layer check(s) failed\n", failures);
        return 1;
    }
    std::printf("data-layer checks passed\n");
    return 0;
}
