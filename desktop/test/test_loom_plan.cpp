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

    // --- ImZoomSlider pan window <-> camera mapping (14-quick-wins.md T5) -----
    // The pure half of the pan control: the draw feeds the slider from
    // loom_view_step_window and applies its result via loom_view_set_step_window,
    // finally writing step0 (the field the model carried but nothing ever set).
    {
        loom_view_t v;
        v.px_w = 400;
        v.step0 = 0;
        v.steps_per_px = 1.0;
        double lo = -1, hi = -1;
        loom_view_step_window(v, &lo, &hi);
        check("window reads step0..step0+spp*px_w", lo == 0.0 && hi == 400.0,
              "got [" + std::to_string(lo) + "," + std::to_string(hi) + "]");

        // Pan to [100, 300]: step0 moves to 100, and the 200-step span across
        // 400px is 0.5 steps/px.
        loom_view_set_step_window(v, 100, 300);
        check("pan writes step0 (was never written before)", v.step0 == 100.0,
              "step0=" + std::to_string(v.step0));
        check("zoom writes steps_per_px", v.steps_per_px == 0.5,
              "spp=" + std::to_string(v.steps_per_px));

        // Round-trips: reading the window back gives what we set.
        loom_view_step_window(v, &lo, &hi);
        check("window round-trips", lo == 100.0 && hi == 300.0,
              "got [" + std::to_string(lo) + "," + std::to_string(hi) + "]");

        // Degenerate inputs are clamped, never producing a zero-width window or a
        // negative step0 (which would divide the plan into nonsense).
        loom_view_set_step_window(v, 50, 50);
        check("zero-width window is widened to >=1 step", v.steps_per_px > 0.0,
              "spp=" + std::to_string(v.steps_per_px));
        loom_view_set_step_window(v, -10, 20);
        check("negative lower is clamped to 0", v.step0 == 0.0,
              "step0=" + std::to_string(v.step0));
        loom_view_set_step_window(v, 300, 100); // reversed
        check("reversed window is normalized", v.step0 == 100.0,
              "step0=" + std::to_string(v.step0));
    }

    // --- vertical lane scroll: a deck taller than the viewport must reach every
    // lane (the lane0-never-assigned bug) -----------------------------------
    {
        loom_view_t v;
        v.px_h = 90;      // 5 full rows of 18px fit
        v.lane_h = 18.0f; // -> lanes_full == 5
        check("lanes_full from px_h/lane_h", loom_view_lanes_full(v) == 5,
              "got " + std::to_string(loom_view_lanes_full(v)));

        // A 12-lane deck: the last lane is reachable, so lane0 clamps at 12-5=7.
        check("lane_max exposes the tail", loom_view_lane_max(v, 12) == 7,
              "got " + std::to_string(loom_view_lane_max(v, 12)));

        // Scrolling down advances lane0 and clamps at the max (never past it).
        v.lane0 = 0;
        loom_view_scroll_lanes(v, 12, +3);
        check("scroll down advances lane0", v.lane0 == 3,
              "lane0=" + std::to_string(v.lane0));
        loom_view_scroll_lanes(v, 12, +100);
        check("scroll down clamps at lane_max", v.lane0 == 7,
              "lane0=" + std::to_string(v.lane0));

        // Scrolling up returns to the top and never goes negative.
        loom_view_scroll_lanes(v, 12, -100);
        check("scroll up clamps at 0", v.lane0 == 0,
              "lane0=" + std::to_string(v.lane0));

        // A deck that fits entirely needs no scroll: lane_max is 0.
        check("a deck that fits does not scroll", loom_view_lane_max(v, 4) == 0,
              "got " + std::to_string(loom_view_lane_max(v, 4)));

        // The scrolled lane0 actually reaches the tail lanes in the plan: with a
        // fixture of N lanes, scrolling to lane_max draws a lane_header whose lane
        // index (prim `a`) is the last lane.
        loom_fabric_t big = f;
        while (big.lanes.size() < 12)
            big.lanes.push_back(big.lanes.empty() ? loom_lane_t{}
                                                  : big.lanes.back());
        v.px_w = 400;
        loom_view_scroll_lanes(v, static_cast<int>(big.lanes.size()),
                               loom_view_lane_max(v, 12));
        std::vector<loom_prim_t> tail;
        loom_plan(big, v, &tail);
        uint32_t max_lane = 0;
        for (const loom_prim_t &p : tail)
            if (p.kind == loom_prim::lane_header && p.a > max_lane)
                max_lane = p.a;
        check("scrolled plan reaches the last lane",
              max_lane == big.lanes.size() - 1,
              "max lane header index " + std::to_string(max_lane));
    }

    if (failures) {
        std::fprintf(stderr, "%d loom plan check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_plan: all checks passed\n");
    return 0;
}
