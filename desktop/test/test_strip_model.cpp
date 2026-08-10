// test_strip_model.cpp — the session strip's pure closure: camera math,
// layout, build, plan. No ImGui, no GL, no engine (test_scene2d.cpp idiom).
#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "views/strip.h"

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static void camera_math() {
    strip_view_t v;
    v.px_w = 800;
    v.seq0 = 100;
    v.seq_per_px = 2.0;
    double lo = 0, hi = 0;
    strip_view_window(v, &lo, &hi);
    check("window: lo", lo == 100.0, "lo is seq0");
    check("window: hi", hi == 100.0 + 2.0 * 800, "hi is seq0 + seq_per_px*px_w");
    strip_view_set_window(v, 50, 850);
    strip_view_window(v, &lo, &hi);
    check("set_window round-trips", lo == 50.0 && hi == 850.0,
          "set then get must return the same window");
    check("set_window zoom", v.seq_per_px == (850.0 - 50.0) / 800.0, "");
    strip_view_set_window(v, -10, 790);
    strip_view_window(v, &lo, &hi);
    check("set_window clamps lo to 0", lo == 0.0, "no negative stream position");

    v.seq_per_px = 1.0;
    strip_view_follow(v, 10000);
    strip_view_window(v, &lo, &hi);
    check("follow pins tail", hi == 10000.0,
          "follow puts seq_end at the right edge");
    strip_view_follow(v, 10); // shorter than a window
    check("follow clamps at 0", v.seq0 == 0.0, "never a negative origin");

    strip_view_t d;
    d.lane_h = 18;
    check("lanes_full", strip_view_lanes_full(d, 90.0f) == 5, "90/18");
    check("lane_max small deck", strip_view_lane_max(d, 3, 90.0f) == 0,
          "3 lanes fit in 5 rows: no scroll");
    check("lane_max big deck", strip_view_lane_max(d, 12, 90.0f) == 7,
          "12 lanes, 5 visible: max lane0 is 7");
    d.lane0 = 0;
    strip_view_scroll_lanes(d, 12, 90.0f, 100);
    check("scroll clamps high", d.lane0 == 7, "");
    strip_view_scroll_lanes(d, 12, 90.0f, -100);
    check("scroll clamps low", d.lane0 == 0, "");
}

static void layout_sums() {
    StripModel m;
    m.deck_enabled = true;
    m.rail_enabled = true;
    m.bands_enabled = true;
    m.lanes.resize(3);
    m.bands.resize(2);
    strip_view_t v;
    v.px_h = 400;
    v.lane_h = 18;
    StripLayout L = strip_layout(m, v);
    check("layout: channels sum to px_h",
          L.deck_h + L.rail_h + L.bands_h + L.ribbon_h == v.px_h,
          "no unowned pixels");
    check("layout: order", L.deck_y0 == 0 && L.rail_y0 == L.deck_h &&
                               L.bands_y0 == L.rail_y0 + L.rail_h &&
                               L.ribbon_y0 == L.bands_y0 + L.bands_h,
          "deck, rail, bands, ribbon — top to bottom");
    check("layout: equal band heights", L.band_h == L.bands_h / 2.0f,
          "a band's height encodes nothing");
    check("layout: deck fits its lanes", L.deck_h == 3 * v.lane_h,
          "3 lanes need no cap at 400px");

    StripModel none;
    none.deck_enabled = false;
    none.deck_reason = "x";
    none.rail_enabled = false;
    none.rail_reason = "x";
    none.bands_enabled = false;
    none.bands_reason = "x";
    StripLayout L2 = strip_layout(none, v);
    check("layout: disabled channels still sum",
          L2.deck_h + L2.rail_h + L2.bands_h + L2.ribbon_h == v.px_h,
          "absent channels shrink to note rows; the sum invariant holds");
}

static void pinned_strings() {
    check("axis label pinned",
          std::string(StripModel::axis_label()) == "stream order — not time",
          "the axis's own honesty claim, verbatim");
    check("mem tid note pinned",
          std::string(StripModel::mem_tid_note()) ==
              "mem carries no tid — access marks are r/w-hued, never "
              "thread-hued",
          "the legend's no-inference claim, verbatim");
}

int main() {
    camera_math();
    layout_sums();
    pinned_strings();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
