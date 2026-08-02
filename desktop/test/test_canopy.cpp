// test_canopy.cpp — the per-module residency skyline
// (56-fidelity-and-module-layers.md T3). Null harness: links space/canopy.o +
// space/terrain.o + space/projection.o + the trace canvas builder + the
// document model, mirroring test_terrain.cpp's own closure (D4).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "space/canopy.h"
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
static bool near(double a, double b) { return std::fabs(a - b) < 1e-6; }

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

int main() {
    // Region A [0x400000,+256): A0 hit twice, A1 once (raw_heat = 3). Region B
    // [0x500000,+256): B0 hit three times (raw_heat = 3) — same total as A by
    // construction, so a bug that summed PER-CELL LOG heights instead of raw
    // counts would still (coincidentally) match A's total but not B's, and a
    // bug that dropped the per-cell split (e.g. summed only ONE of A0/A1)
    // would diverge from the known arithmetic sum on the region with two
    // populated cells.
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

    std::vector<Region> regs = regions_from_codeimage(rec);
    Projection p = build_projection(regs);
    TerrainModel m = build_terrain(p, rec);

    std::vector<ModuleCanopy> full = build_module_canopies(m, UINT64_MAX);
    check("one exact canopy per region (no survey)", full.size() == 2,
          "got " + std::to_string(full.size()));

    const ModuleCanopy *ca = nullptr;
    const ModuleCanopy *cb = nullptr;
    for (const ModuleCanopy &mc : full) {
        if (m.proj.regions[mc.region].base == 4194304)
            ca = &mc;
        else if (m.proj.regions[mc.region].base == 5242880)
            cb = &mc;
    }
    check("region A canopy present", ca != nullptr, "");
    check("region B canopy present", cb != nullptr, "");
    if (ca && cb) {
        // --- the anti-regression bar: raw_heat is the ARITHMETIC sum -------
        check("A raw_heat == 3 (2 + 1, arithmetic)", near(ca->raw_heat, 3.0),
              "got " + std::to_string(ca->raw_heat));
        check("A height == log1p(3) (log AFTER the sum)",
              near(ca->height, std::log1p(3.0)),
              "got " + std::to_string(ca->height));
        // The regression this pins: summing the PER-CELL log-scaled heights
        // (log1p(2) + log1p(1)) instead of log-ing the raw sum. The two
        // differ measurably.
        check("A height != sum of per-cell log-scaled heights",
              !near(ca->height, std::log1p(2.0) + std::log1p(1.0)),
              "raw_heat was logged BEFORE summing, not after");
        check("B raw_heat == 3 (arithmetic)", near(cb->raw_heat, 3.0),
              "got " + std::to_string(cb->raw_heat));
        check("B height == log1p(3)", near(cb->height, std::log1p(3.0)),
              "got " + std::to_string(cb->height));

        // --- cells_mapped / cells_hit are geometry vs. activity, distinctly -
        check("A has more mapped cells than hit cells (2 offsets, wider span)",
              ca->cells_mapped >= ca->cells_hit,
              "mapped must never be less than hit");
        check("A cells_hit == 2 (A0 and A1 are distinct cells)",
              ca->cells_hit == 2, "got " + std::to_string(ca->cells_hit));
        check("B cells_hit == 1 (B0 alone)", cb->cells_hit == 1,
              "got " + std::to_string(cb->cells_hit));
        check("neither region is torn (a complete recording)", !ca->torn && !cb->torn,
              "torn without a truncated/dropped recording");
        check("neither exact canopy is flagged statistical",
              !ca->statistical && !cb->statistical, "");

        // --- footprint bounding box is a real, non-degenerate extent --------
        check("A bounding box is valid", ca->min_u <= ca->max_u &&
                                             ca->min_v <= ca->max_v,
              "min must not exceed max for a region with cells_mapped > 0");
    }

    // --- time slicing: t=0 sees only A0 --------------------------------------
    std::vector<ModuleCanopy> t0 = build_module_canopies(m, 0);
    const ModuleCanopy *ca0 = nullptr;
    for (const ModuleCanopy &mc : t0)
        if (m.proj.regions[mc.region].base == 4194304)
            ca0 = &mc;
    check("t=0: region A canopy present (mapped, even before any hit)",
          ca0 != nullptr, "cells_mapped must not depend on t");
    if (ca0) {
        check("t=0: A raw_heat == 1 (only A0's step-0 hit counts)",
              near(ca0->raw_heat, 1.0), "got " + std::to_string(ca0->raw_heat));
        check("t=0: A cells_hit == 1 (A1 not yet touched)", ca0->cells_hit == 1,
              "got " + std::to_string(ca0->cells_hit));
    }
    bool cb_at_t0 = false;
    for (const ModuleCanopy &mc : t0)
        if (m.proj.regions[mc.region].base == 5242880)
            cb_at_t0 = true;
    check("t=0: region B canopy STILL present (mapped-but-cold, not omitted)",
          cb_at_t0, "a footprint with zero hits so far is a wire outline, "
                    "not a dropped entry");
    for (const ModuleCanopy &mc : t0)
        if (m.proj.regions[mc.region].base == 5242880)
            check("t=0: B is the mapped-but-cold wire-outline case",
                  mc.cells_mapped > 0 && mc.cells_hit == 0,
                  "mapped=" + std::to_string(mc.cells_mapped) +
                      " hit=" + std::to_string(mc.cells_hit));

    // --- an empty model produces no canopies, never a crash ------------------
    check("empty model yields no canopies",
          build_module_canopies(TerrainModel{}, 0).empty(),
          "a model with no regions must not synthesise one");

    if (failures) {
        std::fprintf(stderr, "%d test_canopy check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_canopy: all checks passed\n");
    return 0;
}
