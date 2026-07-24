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

    if (failures) {
        std::fprintf(stderr, "%d slice check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_slice: all checks passed\n");
    return 0;
}
