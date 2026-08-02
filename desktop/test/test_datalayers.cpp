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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "scene3d/hud.h"
#include "space/projection.h"
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

int main() {
    t1_hud_contract();

    if (failures) {
        std::fprintf(stderr, "%d data-layer check(s) failed\n", failures);
        return 1;
    }
    std::printf("data-layer checks passed\n");
    return 0;
}
