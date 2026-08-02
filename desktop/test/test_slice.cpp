// test_slice.cpp — the client-side closure rule (04-replay-views.md T1).
// Engine-free by construction: this binary links slice.o and nothing else, so
// its link line is the proof that the render-only viewer can slice (D4).
#include <cstdio>
#include <string>
#include <vector>

#include "analysis/slice.h"

using namespace asmdesk;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}

static std::string show(const std::vector<uint32_t> &v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? "," : "") + std::to_string(v[i]);
    return s + "}";
}

static void eq(const std::string &what, const dt_slice &got,
               const std::vector<uint32_t> &want) {
    if (got.steps != want)
        fail(what, "got " + show(got.steps) + ", want " + show(want));
}

static std::string show(const std::vector<int32_t> &v) {
    std::string s = "{";
    for (size_t i = 0; i < v.size(); i++)
        s += (i ? "," : "") + std::to_string(v[i]);
    return s + "}";
}

static void eq_u32(const std::string &what, const std::vector<uint32_t> &got,
                   const std::vector<uint32_t> &want) {
    if (got != want)
        fail(what, "got " + show(got) + ", want " + show(want));
}

static void check_true(const std::string &what, bool cond) {
    if (!cond)
        fail(what, "got false, want true");
}

static void check_false(const std::string &what, bool cond) {
    if (cond)
        fail(what, "got true, want false");
}

static void check_eq_int(const std::string &what, int32_t got, int32_t want) {
    if (got != want)
        fail(what, "got " + std::to_string(got) + ", want " +
                       std::to_string(want));
}

int main() {
    // The doc's fixture: 6 steps, edges 0->2, 1->2, 2->4, 3->4, 4->5.
    const std::vector<dt_edge> g = {{0, 2}, {1, 2}, {2, 4}, {3, 4}, {4, 5}};
    const uint32_t n = 6;

    eq("backward(4)", dt_slice_backward(g, n, 4), {0, 1, 2, 3, 4});
    eq("backward(2)", dt_slice_backward(g, n, 2), {0, 1, 2});
    eq("forward(1)", dt_slice_forward(g, n, 1), {1, 2, 4, 5});
    eq("forward(5)", dt_slice_forward(g, n, 5), {5});
    eq("backward(0)", dt_slice_backward(g, n, 0), {0});

    // An out-of-range origin yields the EMPTY slice, not {origin}: the step was
    // never recorded, so nothing is known about it (src/dataflow.c:673).
    eq("forward(6) out of range", dt_slice_forward(g, n, 6), {});
    eq("backward(7) out of range", dt_slice_backward(g, n, 7), {});

    // An edge whose endpoint is past nsteps is ignored, not followed.
    std::vector<dt_edge> oob = g;
    oob.push_back({4, 9});
    eq("forward(1) with a to_step >= nsteps", dt_slice_forward(oob, n, 1),
       {1, 2, 4, 5});
    oob.push_back({9, 3});
    eq("backward(4) with a from_step >= nsteps", dt_slice_backward(oob, n, 4),
       {0, 1, 2, 3, 4});

    // A valid origin with no edges at all is {origin}.
    eq("forward over an empty graph", dt_slice_forward({}, n, 3), {3});
    eq("backward over an empty graph", dt_slice_backward({}, n, 3), {3});
    eq("empty graph, nsteps 0", dt_slice_forward({}, 0, 0), {});

    // A cycle terminates (recorded graphs from a loop body carry them).
    const std::vector<dt_edge> cyc = {{0, 1}, {1, 2}, {2, 1}, {2, 3}};
    eq("forward over a cycle", dt_slice_forward(cyc, 4, 0), {0, 1, 2, 3});
    eq("backward over a cycle", dt_slice_backward(cyc, 4, 3), {0, 1, 2, 3});

    // contains() agrees with membership, on both sides of every boundary.
    dt_slice s = dt_slice_backward(g, n, 4);
    for (uint32_t k = 0; k < n + 2; k++) {
        bool want = k <= 4;
        if (s.contains(k) != want)
            fail("contains(" + std::to_string(k) + ")",
                 std::string("got ") + (s.contains(k) ? "true" : "false"));
    }
    if (dt_slice{}.contains(0))
        fail("contains on an empty slice", "an empty slice contains nothing");

    // --- 54 T5: dt_walk_depth ------------------------------------------------
    // A diamond with one longer alternate path (1->4->3) alongside the two
    // short ones (0->1->3, 0->2->3): node 3 is reachable at hop 2 via 1 or 2,
    // AND at hop 3 via 1->4->3. First-reach BFS must report 2, not 3 — a
    // last-write-wins bug would report whichever of 2's/4's edges is
    // processed last.
    const std::vector<dt_edge> diamond = {{0, 1}, {0, 2}, {1, 3},
                                          {2, 3}, {1, 4}, {4, 3}};
    {
        dt_walk w = dt_walk_depth(diamond, 5, 0, true, 10);
        eq_u32("diamond: steps", w.steps, {0, 1, 2, 3, 4});
        std::vector<int32_t> want_depth = {0, 1, 1, 2, 2};
        if (w.depth != want_depth)
            fail("diamond: first-reach depth (not last-reach)",
                 "got " + show(w.depth) + ", want " + show(want_depth));
        check_eq_int("diamond: depth_min", w.depth_min, 0);
        check_eq_int("diamond: depth_max", w.depth_max, 2);
        check_false("diamond: unbounded walk is not bounded", w.bounded);
    }

    // A cycle terminates (BFS visited-once, same graph test_slice's own cycle
    // test uses) and the unbounded walk's step set matches dt_slice_forward.
    {
        dt_walk w = dt_walk_depth(cyc, 4, 0, true, 10);
        std::vector<int32_t> want_depth = {0, 1, 2, 3};
        if (w.depth != want_depth)
            fail("cycle: depth", "got " + show(w.depth) + ", want " +
                                     show(want_depth));
        check_false("cycle: unbounded walk terminates, not bounded",
                    w.bounded);
    }

    // max_depth truncates the walk AND sets bounded, before the true fixpoint.
    {
        dt_walk w = dt_walk_depth(g, n, 1, true, 1);
        eq_u32("max_depth=1 from 1: steps", w.steps, {1, 2});
        std::vector<int32_t> want_depth = {0, 1};
        if (w.depth != want_depth)
            fail("max_depth=1 from 1: depth",
                 "got " + show(w.depth) + ", want " + show(want_depth));
        check_true("max_depth=1 from 1: bounded (4 was cut off)", w.bounded);
    }

    // A walk that lands exactly on max_depth with NO further edges reached
    // the true fixpoint — bounded must stay false, not follow from
    // depth_max == max_depth alone (5 is g's sink: no outgoing edges).
    {
        dt_walk w = dt_walk_depth(g, n, 5, true, 0);
        eq_u32("max_depth=0 from sink 5: steps", w.steps, {5});
        check_false(
            "max_depth=0 from sink 5: NOT bounded (fixpoint, not a cut)",
            w.bounded);
    }

    // Backward direction stores NEGATIVE depth, so both directions from one
    // origin plot on a single signed axis.
    {
        dt_walk w = dt_walk_depth(g, n, 4, false, 10);
        eq_u32("backward(4): steps", w.steps, {0, 1, 2, 3, 4});
        std::vector<int32_t> want_depth = {-2, -2, -1, -1, 0};
        if (w.depth != want_depth)
            fail("backward(4): negative depth",
                 "got " + show(w.depth) + ", want " + show(want_depth));
        check_eq_int("backward(4): depth_min", w.depth_min, -2);
        check_eq_int("backward(4): depth_max", w.depth_max, 0);
    }

    // Out-of-range origin: an EMPTY walk, never {origin} — same rule as
    // dt_slice.
    {
        dt_walk w = dt_walk_depth(g, n, 6, true, 10);
        eq_u32("out-of-range origin: empty walk", w.steps, {});
        check_false("out-of-range origin: not bounded", w.bounded);
    }

    // An empty edge set yields {origin} at depth 0, exactly like dt_slice.
    {
        dt_walk w = dt_walk_depth({}, n, 3, true, 10);
        eq_u32("empty graph: {origin}", w.steps, {3});
        std::vector<int32_t> want_depth = {0};
        if (w.depth != want_depth)
            fail("empty graph: depth 0",
                 "got " + show(w.depth) + ", want " + show(want_depth));
    }

    // The unbounded walk's step set must not quietly disagree with
    // dt_slice_forward/backward about REACHABILITY, over several origins.
    for (uint32_t origin : {0u, 1u, 2u, 3u, 4u, 5u}) {
        dt_walk fw = dt_walk_depth(g, n, origin, true, 10);
        dt_slice fs = dt_slice_forward(g, n, origin);
        if (fw.steps != fs.steps)
            fail("forward(" + std::to_string(origin) +
                     ") walk vs. slice set-equality",
                 "walk " + show(fw.steps) + ", slice " + show(fs.steps));
        dt_walk bw = dt_walk_depth(g, n, origin, false, 10);
        dt_slice bs = dt_slice_backward(g, n, origin);
        if (bw.steps != bs.steps)
            fail("backward(" + std::to_string(origin) +
                     ") walk vs. slice set-equality",
                 "walk " + show(bw.steps) + ", slice " + show(bs.steps));
    }

    if (failures) {
        std::fprintf(stderr, "%d slice check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_slice: all checks passed\n");
    return 0;
}
