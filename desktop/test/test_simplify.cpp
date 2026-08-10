// test_simplify.cpp — the 3D simplified posture's pure rules (2026-08-10
// 3d-simplify spec): the layer gate clears exactly the five spike layers and
// NEVER turns one on; the placard speaks only when something was withheld,
// and its words are pinned. Null harness, no display.
#include <cstdio>
#include <string>

#include "scene3d/simplify.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main() {
    // detail = true: the identity
    {
        scene3d::SceneLayers l;
        l.access_marks = true;
        l.data_relief = true;
        scene3d::SceneLayers out = scene3d::simplify_apply(l, true);
        check("detail is identity",
              out.access_marks && out.data_relief && out.terrain,
              "detail must not touch a single layer");
    }
    // simplified: exactly the five spike layers clear; aggregates survive
    {
        scene3d::SceneLayers l; // defaults: access_marks ON, relief family OFF
        l.data_relief = true;
        l.working_set = true;
        l.lifetime = true;
        l.sediment = true;
        scene3d::SceneLayers out = scene3d::simplify_apply(l, false);
        check("spike layers withheld",
              !out.access_marks && !out.data_relief && !out.working_set &&
                  !out.lifetime && !out.sediment,
              "the five per-event encodings");
        check("aggregates survive",
              out.terrain && out.canopy && out.ridge && out.convergence &&
                  out.crossings && out.exact,
              "terrain/canopy/ridge/convergence/crossings are the point");
        // never turns a layer ON
        scene3d::SceneLayers all_off{};
        all_off.terrain = false;
        all_off.exact = false;
        all_off.statistical = false;
        all_off.access_marks = false;
        all_off.convergence = false;
        all_off.zoning = false;
        all_off.weather = false;
        all_off.ghost_fog = false;
        all_off.vehicle = false;
        all_off.contours = false;
        all_off.edl = false;
        all_off.canopy = false;
        all_off.mispred = false;
        all_off.crossings = false;
        all_off.taint = false;
        all_off.blame = false;
        all_off.ridge = false;
        all_off.halos = false;
        scene3d::SceneLayers off_out = scene3d::simplify_apply(all_off, false);
        check("never turns a layer on",
              !off_out.terrain && !off_out.access_marks && !off_out.canopy &&
                  !off_out.crossings,
              "lod_apply's rule, kept");
    }
    // the placard: silent unless something was withheld; words pinned
    {
        space::SimplifyNote none;
        check("silent in detail",
              scene3d::simplify_note(true, none, true).empty(),
              "detail has no placard");
        check("silent when nothing withheld",
              scene3d::simplify_note(false, none, false).empty(),
              "the under-threshold no-op stays silent");
        space::SimplifyNote n;
        n.hidden_threads = 190;
        n.hidden_points = 12345;
        check("placard counts the fold",
              scene3d::simplify_note(false, n, true) ==
                  "simplified — top 8 worldlines (+190 folded, 12345 points); "
                  "access-mark spurs withheld — detail restores",
              "the claim, verbatim — a screenshot can never pass a "
              "simplified scene off as the whole");
        check("placard without spurs",
              scene3d::simplify_note(false, n, false) ==
                  "simplified — top 8 worldlines (+190 folded, 12345 points)"
                  " — detail restores",
              "");
        check("placard spurs-only",
              scene3d::simplify_note(false, none, true) ==
                  "simplified — access-mark spurs withheld — detail restores",
              "");
    }
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
