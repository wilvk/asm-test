// test_coverage_window.cpp — the confidence layer's coverage-window mask
// (56-fidelity-and-module-layers.md T2 step 2). Null harness: links
// space/terrain.o + space/projection.o + views/canvas.o (the terrain's own
// closure, mirroring test_terrain.cpp) plus views/hotedges.o for
// apply_coverage_window itself and obs_hotedges_build/obs_hotedges_for_scene,
// which supply hv.have_window/window_base/window_len from a real recording's
// provenance rather than a hand-built HotEdgeSceneView — the same wiring
// shell.cpp uses.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>

#include "doc/recording.h"
#include "space/projection.h"
#include "space/terrain.h"
#include "views/hotedges.h"

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

int main() {
    // Region A [0x400000, 0x400100): hit twice at A0, once at A1 (in the
    // stated window). Region B [0x500000, 0x500100): hit three times at B0
    // (outside the window entirely).
    static const uint64_t A0 = 0x400000, A1 = 0x400010, B0 = 0x500000;
    Recording rec = mk_rec(
        "{\"asmtrace\":1,\"provenance\":{\"backend\":\"sample\",\"exact\":"
        "false,\"trust\":\"statistical\",\"window\":{\"base\":4194304,"
        "\"len\":256}},\"arch\":\"x86_64\"}\n"
        "{\"k\":\"codeimage\",\"base\":4194304,\"len\":256,\"version\":0,"
        "\"when\":1,\"bytes\":\"90\"}\n"
        "{\"k\":\"codeimage\",\"base\":5242880,\"len\":256,\"version\":0,"
        "\"when\":1,\"bytes\":\"90\"}\n"
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194320}\n"
        "{\"k\":\"trace\",\"basis\":\"abs\",\"off\":4194304}\n"
        "{\"k\":\"survey\",\"sampler\":\"ibs-op\",\"samples\":10,"
        "\"branch_samples\":10,\"lost\":0,\"throttled\":false,\"edges\":[]}\n"
        "{\"k\":\"end\",\"events\":7,\"truncated\":false,"
        "\"drops\":{\"lost\":0,\"throttled\":false}}\n");

    std::vector<Region> regs = regions_from_codeimage(rec);
    check("two code regions", regs.size() == 2,
          "got " + std::to_string(regs.size()));
    Projection p = build_projection(regs);
    TerrainModel m = build_terrain(p, rec);
    Terrain slice = m.full();

    HotEdgeView hv_raw = obs_hotedges_build(rec);
    check("window read from provenance", hv_raw.have_window,
          "the fixture's provenance.window must be parsed");
    check("window base", hv_raw.window_base == 4194304,
          "got " + std::to_string(hv_raw.window_base));
    check("window len", hv_raw.window_len == 256,
          "got " + std::to_string(hv_raw.window_len));
    HotEdgeSceneView hv = obs_hotedges_for_scene(hv_raw);

    bool ok = false;
    uint32_t cA0 = cell_at(m.proj, A0, &ok);
    uint32_t cA1 = cell_at(m.proj, A1, &ok);
    uint32_t cB0 = cell_at(m.proj, B0, &ok);

    // --- no-window: a total no-op, not one flag touched ---------------------
    {
        Terrain untouched = m.full();
        HotEdgeSceneView none; // have_window == false, the default
        apply_coverage_window(untouched, m, none);
        check("no window: A0 flags unchanged", untouched.flags[cA0] == slice.flags[cA0],
              "an absent window must not fabricate a mask");
        check("no window: B0 flags unchanged", untouched.flags[cB0] == slice.flags[cB0],
              "an absent window must not fabricate a mask");
    }

    // --- window present: A is in-window-and-credited, B is out-of-window ----
    apply_coverage_window(slice, m, hv);
    check("A0 (in window, credited) carries neither window bit",
          (slice.flags[cA0] & (TF_INWINDOW_EMPTY | TF_OUTWINDOW)) == 0u,
          "a hit cell in the window must read as a plain mound");
    check("A1 (in window, credited) carries neither window bit",
          (slice.flags[cA1] & (TF_INWINDOW_EMPTY | TF_OUTWINDOW)) == 0u,
          "a hit cell in the window must read as a plain mound");
    check("B0 (outside the window) is flagged OUTWINDOW",
          (slice.flags[cB0] & TF_OUTWINDOW) != 0u,
          "B0 sits at 0x500000, outside [0x400000, 0x400100)");
    check("B0 is never ALSO flagged in-window-empty (mutually exclusive)",
          (slice.flags[cB0] & TF_INWINDOW_EMPTY) == 0u,
          "a cell is either out of the window or (maybe) empty inside it, "
          "never both");

    // --- an in-window cell with zero height reads as below-rate, not cold --
    {
        Terrain empty_in_window = m.full();
        // A synthetic in-domain, in-window cell that never got a hit: pick any
        // in-domain cell that is NOT cA0/cA1 by construction (both got hits) —
        // reuse cA0's own cell after manually zeroing its height, so this
        // proves the FUNCTION's branch rather than hunting for a naturally
        // cold cell in a plane this small.
        empty_in_window.height[cA0] = 0.0f;
        apply_coverage_window(empty_in_window, m, hv);
        check("a zero-height in-window cell is flagged below-rate",
              (empty_in_window.flags[cA0] & TF_INWINDOW_EMPTY) != 0u,
              "in-window-and-empty must read as below-rate, not silently cold");
        check("below-rate is never OUTWINDOW too",
              (empty_in_window.flags[cA0] & TF_OUTWINDOW) == 0u,
              "in-window and out-of-window are mutually exclusive");
    }

    if (failures) {
        std::fprintf(stderr, "%d test_coverage_window check(s) failed\n",
                     failures);
        return 1;
    }
    std::printf("test_coverage_window: all checks passed\n");
    return 0;
}
