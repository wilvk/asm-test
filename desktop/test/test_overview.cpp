// test_overview.cpp — the overview/minimap projection (21-spine-navigation.md
// T3). Model only: the strip is derived from the recording's OWN rows and never
// fabricates structure (docs 04/08 layout ban) — a sparse trace yields a sparse
// strip. No ImGui, no engine: overview.o + the pure timeline builder + the doc
// model, the same engine-free closure test_timeline makes.
#include <cstdio>
#include <string>

#include "doc/recording.h"
#include "doc/streams.h"
#include "loom/fabric.h"
#include "views/overview.h"
#include "views/timeline.h"

using namespace asmdesk;

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}
static std::string gd(const char *name) {
    return std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
}

int main() {
    // --- timeline: exactly the real steps, no invented in-between buckets ------
    {
        dt_timeline t;
        dt_timeline_row r0;
        r0.step = 0;
        r0.n_in = 1;
        r0.n_out = 1;
        t.rows.push_back(r0);
        dt_timeline_row r2;
        r2.step = 2;
        r2.missing = true; // a dropped step: a real index, unknown activity
        t.rows.push_back(r2);
        dt_timeline_row r5;
        r5.step = 5;
        r5.n_in = 2;
        r5.n_out = 0;
        t.rows.push_back(r5);

        OverviewStrip s = overview_from_timeline(t, 0, 6);
        check("sparse: one bucket per real row", s.buckets.size() == 3,
              "the strip must not invent buckets for absent steps (D7)");
        check("sparse: buckets carry the real step indices",
              s.buckets.size() == 3 && s.buckets[0].step == 0 &&
                  s.buckets[1].step == 2 && s.buckets[2].step == 5,
              "steps 0,2,5 — never a padded 0..5");
        check("sparse: a dropped step is weight 0, not fabricated activity",
              s.buckets.size() == 3 && s.buckets[1].weight == 0,
              "unknown, not invented");
        check("extent = last real step + 1", s.nsteps == 6, "nsteps");
        check("viewport stored verbatim", s.lo == 0 && s.hi == 6, "lo/hi");
    }

    // --- fractional click mapping (a minimap click == a typed go-to, D4) -------
    {
        OverviewStrip s;
        s.nsteps = 10;
        check("click mid -> round(0.5*10)=5", overview_click_step(s, 0.5) == 5,
              "");
        check("click start -> 0", overview_click_step(s, 0.0) == 0, "");
        check("click end clamps to last real step",
              overview_click_step(s, 1.0) == 9, "round(10) clamps to 9");
        check("click below range clamps low", overview_click_step(s, -0.5) == 0,
              "");
    }

    // --- fabric: one bucket per recorded step, live-span count ----------------
    {
        loom_fabric_t f;
        f.steps = 4;
        loom_span_t a;
        a.t_write = 0;
        a.t_end = 2; // live at steps 0,1
        f.spans.push_back(a);
        loom_span_t b;
        b.t_write = 1;
        b.t_end = kLoomAlive; // live 1..3 (through the last recorded step)
        f.spans.push_back(b);

        OverviewStrip s = overview_from_fabric(f, 0, 4);
        check("fabric: one bucket per recorded step", s.buckets.size() == 4, "");
        check("fabric: nsteps", s.nsteps == 4, "");
        check("fabric: live-span counts",
              s.buckets.size() == 4 && s.buckets[0].weight == 1 &&
                  s.buckets[1].weight == 2 && s.buckets[2].weight == 1 &&
                  s.buckets[3].weight == 1,
              "density is the fabric's own spans, never fabricated");
    }

    // --- no fabrication over a REAL recording's own rows ----------------------
    {
        std::string err;
        auto rec = load_recording_file(gd("sum_via_rbx.asmtrace"), err);
        check("load real recording", static_cast<bool>(rec), err.c_str());
        if (rec) {
            Streams st = decode_streams(*rec);
            dt_timeline t = dt_timeline_build(st);
            OverviewStrip s = overview_from_timeline(t, 0, st.df.nsteps);
            check("real: one bucket per row, never padded",
                  s.buckets.size() == t.rows.size(),
                  "the strip is a projection of the recording's own rows");
            bool ordered = s.buckets.size() == t.rows.size();
            for (size_t i = 0; ordered && i < t.rows.size(); i++)
                if (s.buckets[i].step != t.rows[i].step)
                    ordered = false;
            check("real: bucket steps == row steps, in order", ordered,
                  "no invented or reordered buckets");
        }
    }

    if (failures) {
        std::fprintf(stderr, "test_overview: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_overview: all checks passed\n");
    return 0;
}
