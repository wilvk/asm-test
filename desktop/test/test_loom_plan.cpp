// test_loom_plan.cpp — the draw plan's zoom semantics (05-loom-day-one.md T2).
// Links fabric.o + fabric_plan.o only: the planner reaches for no ImGui, which
// is what keeps every zoom rule assertable without a display.
#include <cstdio>
#include <string>
#include <vector>

#include "loom/fabric_plan.h"
#include "loom_fixture.h"

using namespace asmdesk;
using namespace loomfx;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static size_t count(const std::vector<loom_prim_t> &p, loom_prim k) {
    size_t n = 0;
    for (const loom_prim_t &x : p)
        if (x.kind == k)
            n++;
    return n;
}

int main() {
    Fixture fx;
    loom_fabric_t f = build(fx);
    std::vector<loom_prim_t> prims;

    // --- zoomed out: spans thinner than 3px collapse into a density ribbon --
    {
        loom_view_t v;
        v.steps_per_px = 4.0; // 6 steps in 1.5 px — everything is sub-pixel
        v.px_w = 400;
        v.px_h = 400;
        v.lane_h = 18;
        loom_plan(f, v, &prims);
        check("zoomed out yields at least one density ribbon",
              count(prims, loom_prim::density_ribbon) >= 1, "none");
        check("zoomed out yields no byte rows",
              count(prims, loom_prim::byte_row) == 0,
              "byte rows at 4 steps/px");
        check("zoomed out yields no span rectangles",
              count(prims, loom_prim::span) == 0 &&
                  count(prims, loom_prim::span_hollow) == 0,
              "a collapsed lane still drew rectangles");
        check("lane headers survive the collapse",
              count(prims, loom_prim::lane_header) == f.lanes.size(),
              "got " + std::to_string(count(prims, loom_prim::lane_header)));
        // A ribbon column's intensity is a live-span COUNT, never a guess.
        for (const loom_prim_t &p : prims)
            if (p.kind == loom_prim::density_ribbon)
                check("ribbon intensity is a live count", p.a >= 1,
                      "a ribbon column with 0 live spans was emitted");
    }

    // --- zoomed in on the band: per-byte rows + value chips ----------------
    {
        loom_view_t v;
        v.steps_per_px = 0.01; // 100 px per step
        v.px_w = 800;
        v.px_h = 400;
        v.lane_h = 200; // 16-byte band -> 12.5 px per byte, past the threshold
        v.lane0 = band_lane(f);
        loom_plan(f, v, &prims);
        check("zoomed in on the band explodes into byte rows",
              count(prims, loom_prim::byte_row) > 0, "none");
        check("the band's byte rows cover the written bytes",
              count(prims, loom_prim::byte_row) >= 8,
              "an 8-byte store should own 8 rows");
        check("chips appear when the span fits its text",
              count(prims, loom_prim::value_chip) > 0, "none");
    }

    // A narrower lane keeps the band whole: the explosion is a zoom rule, not a
    // property of memory lanes.
    {
        loom_view_t v;
        v.steps_per_px = 0.01;
        v.px_w = 800;
        v.px_h = 400;
        v.lane_h = 18;
        v.lane0 = band_lane(f);
        loom_plan(f, v, &prims);
        check("a short lane draws the band as one span, not byte rows",
              count(prims, loom_prim::byte_row) == 0,
              "byte rows at 18px lanes");
    }

    // --- chips only when the text fits -------------------------------------
    {
        loom_view_t wide, narrow;
        wide.steps_per_px = 0.02;
        wide.px_w = 800;
        wide.px_h = 400;
        narrow = wide;
        narrow.steps_per_px = 0.4; // ~2.5 px per step: no chip text fits
        std::vector<loom_prim_t> a, b;
        loom_plan(f, wide, &a);
        loom_plan(f, narrow, &b);
        check("a wide camera shows chips", count(a, loom_prim::value_chip) > 0,
              "none");
        check("a narrow camera shows none",
              count(b, loom_prim::value_chip) == 0,
              "a chip was drawn into a span too small for its own text");
    }

    // --- hops need BOTH endpoints visible ----------------------------------
    {
        loom_view_t all;
        all.steps_per_px = 0.05;
        all.px_w = 800;
        all.px_h = 400;
        all.lane_h = 18;
        std::vector<loom_prim_t> a;
        loom_plan(f, all, &a);
        check("all four hops draw when every lane is on screen",
              count(a, loom_prim::hop) == 4,
              "got " + std::to_string(count(a, loom_prim::hop)));

        loom_view_t clipped = all;
        clipped.px_h = 36; // two lanes tall
        std::vector<loom_prim_t> b;
        loom_plan(f, clipped, &b);
        check("a hop with an off-screen endpoint is not drawn",
              count(b, loom_prim::hop) < 4,
              "hops were drawn to lanes outside the viewport");
    }

    // --- determinism: the T7 seam ------------------------------------------
    {
        loom_view_t v;
        v.steps_per_px = 0.05;
        v.px_w = 640;
        v.px_h = 300;
        std::vector<loom_prim_t> a, b;
        loom_plan(f, v, &a);
        loom_plan(f, v, &b);
        check("two identical calls produce identical plans",
              loom_plan_dump(a) == loom_plan_dump(b), "the dumps differ");
        check("the plan is non-empty", !a.empty(), "nothing was planned");
    }

    // --- selection dimming (T3's input to the planner) ---------------------
    {
        loom_view_t v;
        v.steps_per_px = 0.05;
        v.px_w = 640;
        v.px_h = 300;
        v.selected_steps = {0, 1, 2, 3, 4};
        std::vector<loom_prim_t> a;
        loom_plan(f, v, &a);
        bool dimmed_something = false, kept_something = false;
        for (const loom_prim_t &p : a)
            if (p.kind == loom_prim::span || p.kind == loom_prim::span_hollow)
                (p.b ? dimmed_something : kept_something) = true;
        check("a selection dims spans outside it", dimmed_something,
              "the step-5 spans should be dimmed");
        check("a selection keeps spans inside it undimmed", kept_something, "");
    }

    if (failures) {
        std::fprintf(stderr, "%d loom plan check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_plan: all checks passed\n");
    return 0;
}
