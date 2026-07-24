// test_slice_view.cpp — the def-use slice explorer (04-replay-views.md T5).
#include "nav.h"
#include "view_test.h"
#include "views/slice_view.h"

using namespace asmdesk;
using vt::load;

namespace {

// Build a Streams by hand for the doc's fixture graph (6 steps, edges
// 0->2, 1->2, 2->4, 3->4, 4->5). Step 5 is left edge-less on purpose so the
// "a step with no dependence is dimmed" rule has a subject.
Streams fixture() {
    Streams s;
    s.df.nsteps = 6;
    for (uint32_t i = 0; i < 6; i++) {
        s.df.insn_off.push_back(i * 4);
        s.df.disasm.push_back("insn" + std::to_string(i));
        s.df.step_present.push_back(1);
    }
    s.df.edges = {{0, 2}, {1, 2}, {2, 4}, {3, 4}, {4, 5}};
    for (size_t i = 0; i < s.df.edges.size(); i++) {
        ValRec l;
        l.space = "reg";
        l.reg = 35 + static_cast<uint32_t>(i);
        s.df.edge_loc.push_back(l);
    }
    s.id = "fixture";
    return s;
}

dt_cone style_of(const dt_slice_view &v, uint32_t step) {
    for (const dt_slice_node &n : v.nodes)
        if (n.step == step)
            return n.style;
    return dt_cone::none;
}

} // namespace

int main() {
    // --- the doc's hand fixture: cone styles and stable lanes ---------------
    {
        Streams s = fixture();
        dt_slice_view v = dt_slice_view_build(s, 4);
        vt::eq("selection", v.selected_step.value_or(999), 4u);
        for (uint32_t step : {0u, 1u, 2u, 3u})
            vt::check(
                "step " + std::to_string(step) + " is in the back cone",
                style_of(v, step) == dt_cone::back,
                "everything that produced the value at 4 lights backward");
        vt::check("step 5 is in the forward cone",
                  style_of(v, 5) == dt_cone::fwd, "4 -> 5 is a recorded edge");
        vt::check("the selection itself is in both cones",
                  style_of(v, 4) == dt_cone::both, "the origin is in both");

        // Overlapping arcs 0->2 and 1->2 span [0,2] and [1,2] and so must get
        // DIFFERENT lanes — and the same lanes on every run.
        int lane02 = -1, lane12 = -1;
        for (const dt_slice_edge &e : v.edges) {
            if (e.from_step == 0 && e.to_step == 2)
                lane02 = e.lane;
            if (e.from_step == 1 && e.to_step == 2)
                lane12 = e.lane;
        }
        vt::check("both overlapping arcs are laid out",
                  lane02 >= 0 && lane12 >= 0, "an arc was dropped");
        vt::check("overlapping arcs get different lanes", lane02 != lane12,
                  "0->2 and 1->2 both got lane " + std::to_string(lane02));
        vt::eq("the layout is deterministic",
               dt_slice_view_dump(dt_slice_view_build(s, 4)),
               dt_slice_view_dump(v));

        // A step outside every cone is dimmed, not hidden and not lit.
        Streams s2 = fixture();
        s2.df.nsteps = 7;
        s2.df.insn_off.push_back(24);
        s2.df.disasm.push_back("insn6");
        s2.df.step_present.push_back(1);
        dt_slice_view v2 = dt_slice_view_build(s2, 4);
        vt::check("an edge-less step is not a node",
                  style_of(v2, 6) == dt_cone::none,
                  "a step carrying no dependence is not part of the graph");

        // A step with edges but outside the cone IS dimmed.
        Streams s3 = fixture();
        s3.df.edges.push_back({5, 3});
        dt_slice_view v3 = dt_slice_view_build(s3, 0);
        vt::check("a step outside the cone from 0 is dimmed",
                  style_of(v3, 1) == dt_cone::dimmed,
                  "step 1 neither produces nor consumes step 0's value");

        vt::golden("slice-fixture.txt", dt_slice_view_dump(v));
    }

    // --- generation walk: nearest in-cone step, ties toward the lower -------
    {
        Streams s = fixture();
        dt_slice_view v = dt_slice_view_build(s, 4);
        auto back1 = dt_slice_view_walk(v, 4, false);
        vt::eq("one generation back from 4", back1.value_or(999), 3u);
        auto back2 = dt_slice_view_walk(v, 3, false);
        vt::eq("two generations back", back2.value_or(999), 2u);
        auto fwd1 = dt_slice_view_walk(v, 4, true);
        vt::eq("one generation forward from 4", fwd1.value_or(999), 5u);
        vt::check("the walk ends at the edge of the cone",
                  !dt_slice_view_walk(v, 5, true).has_value(),
                  "nothing is forward of 5");
        vt::check("the walk ends at the other edge too",
                  !dt_slice_view_walk(v, 0, false).has_value(),
                  "nothing is backward of 0");
    }

    // --- a real recording: byte-stable, and the router lands on the step ----
    {
        Streams s = load("sum_via_rbx.asmtrace");
        dt_link link;
        std::string err;
        vt::check("the slice deep link parses",
                  dt_nav_parse("asmtrace-link:v=slice&rec=sum_via_rbx.asmtrace"
                               "&step=4",
                               link, err),
                  err);
        vt::check("it names the slice view", link.view == dt_view::slice,
                  "wrong view");
        dt_slice_view v = dt_slice_view_build(s, link.step);
        vt::eq("the link lands on its step", v.selected_step.value_or(999), 4u);
        vt::check("a clean recording carries no cone banner", v.banner.empty(),
                  "got: " + v.banner);
        vt::golden("slice-sum_via_rbx.txt", dt_slice_view_dump(v));
    }

    // --- truncation: the cones are LOWER BOUNDS and say so ------------------
    {
        Streams s = load("views/trunc-dataflow.asmtrace");
        dt_slice_view v = dt_slice_view_build(s, 1);
        vt::check("a truncated recording gets the cone banner",
                  v.banner.find("cones incomplete") != std::string::npos,
                  "banner: " + v.banner);
        vt::check("the banner says LOWER BOUND",
                  v.banner.find("LOWER BOUND") != std::string::npos,
                  "banner: " + v.banner);
        vt::golden("slice-trunc-dataflow.txt", dt_slice_view_dump(v));
    }

    // --- no selection: a graph with no cones, not an empty view -------------
    {
        Streams s = load("sum_via_rbx.asmtrace");
        dt_slice_view v = dt_slice_view_build(s, std::nullopt);
        vt::check("an unselected graph still has nodes", !v.nodes.empty(),
                  "the DAG is visible before anything is picked");
        for (const dt_slice_node &n : v.nodes)
            vt::check("no cone styling without a selection",
                      n.style == dt_cone::none,
                      "step " + std::to_string(n.step));
    }

    return vt::report("test_slice_view");
}
