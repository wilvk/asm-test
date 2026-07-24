// test_slice_diff.cpp — differential parity: the viewer's closure vs the C
// slicer it replaces (04-replay-views.md T1).
//
// THE ONE EXCEPTION to this doc's engine-free rule: this binary links
// build/dataflow.o so it can call asmtest_slice_forward_seed / _backward_seed
// and compare. It is a test-half binary only; nothing in desktop/src/ links it,
// which is why `asmtest-viewer` stays permissively distributable (D4).
//
// Two agreement arguments run here. The hand fixture pins the doc's example.
// The 200 pseudo-random graphs are the real gate: a deterministic LCG (no
// <random>, whose engines are implementation-defined across libstdc++/libc++)
// generates graphs up to 64 steps and 256 edges — including out-of-range
// endpoints and self-edges — and EVERY step of EVERY graph is compared in both
// directions. A GUI that showed a different slice than the TUI would be worse
// than no slice at all, so the divergence has to fail the build.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "analysis/slice.h"

extern "C" {
#include "asmtest_valtrace.h"
}

using namespace asmdesk;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}

// Compare one closure, in one direction, against the C slicer.
static void compare(const std::string &what, const std::vector<dt_edge> &edges,
                    uint32_t nsteps, uint32_t origin, bool forward) {
    std::vector<asmtest_defuse_edge_t> ce(edges.size());
    for (size_t i = 0; i < edges.size(); i++) {
        ce[i] = asmtest_defuse_edge_t{};
        ce[i].from_step = edges[i].from_step;
        ce[i].to_step = edges[i].to_step;
    }
    asmtest_defuse_t g{};
    g.edges = ce.empty() ? nullptr : ce.data();
    g.n = ce.size();
    g.nsteps = nsteps;

    at_val_rec_t seed{};
    seed.step = origin;
    asmtest_slice_t *cs = forward ? asmtest_slice_forward_seed(&g, &seed)
                                  : asmtest_slice_backward_seed(&g, &seed);
    if (cs == nullptr) {
        fail(what, "the C slicer returned NULL");
        return;
    }
    dt_slice ours = forward ? dt_slice_forward(edges, nsteps, origin)
                            : dt_slice_backward(edges, nsteps, origin);

    bool same = ours.steps.size() == cs->n;
    for (size_t i = 0; same && i < cs->n; i++)
        same = ours.steps[i] == cs->steps[i];
    if (!same) {
        std::string a = "{", b = "{";
        for (size_t i = 0; i < ours.steps.size(); i++)
            a += (i ? "," : "") + std::to_string(ours.steps[i]);
        for (size_t i = 0; i < cs->n; i++)
            b += (i ? "," : "") + std::to_string(cs->steps[i]);
        fail(what, "viewer " + a + "} != engine " + b + "}");
    }
    // contains() must agree too — the timeline's row emphasis calls it per row.
    for (uint32_t k = 0; k < nsteps; k++) {
        if (ours.contains(k) != (asmtest_slice_contains(cs, k) != 0)) {
            fail(what, "contains(" + std::to_string(k) + ") disagrees");
            break;
        }
    }
    asmtest_slice_free(cs);
}

// A 64-bit LCG: deterministic across every platform and standard library, which
// <random>'s distributions are not.
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(uint32_t bound) {
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return bound ? static_cast<uint32_t>((rng_state >> 33) % bound) : 0;
}

int main() {
    const std::vector<dt_edge> fixture = {
        {0, 2}, {1, 2}, {2, 4}, {3, 4}, {4, 5}};
    for (uint32_t s = 0; s < 8; s++) {
        compare("fixture forward(" + std::to_string(s) + ")", fixture, 6, s,
                true);
        compare("fixture backward(" + std::to_string(s) + ")", fixture, 6, s,
                false);
    }

    int graphs = 0;
    for (int gi = 0; gi < 200 && failures == 0; gi++) {
        uint32_t nsteps = 1 + rnd(64);
        uint32_t nedges = rnd(257);
        std::vector<dt_edge> edges(nedges);
        for (uint32_t e = 0; e < nedges; e++) {
            // 1-in-16 endpoints land past nsteps on purpose: an out-of-range
            // endpoint is a real recording shape (a truncated dataflow stream),
            // and both slicers must ignore it identically.
            edges[e].from_step = rnd(16) == 0 ? nsteps + rnd(8) : rnd(nsteps);
            edges[e].to_step = rnd(16) == 0 ? nsteps + rnd(8) : rnd(nsteps);
        }
        for (uint32_t s = 0; s < nsteps && failures == 0; s++) {
            compare("graph " + std::to_string(gi) + " forward(" +
                        std::to_string(s) + ")",
                    edges, nsteps, s, true);
            compare("graph " + std::to_string(gi) + " backward(" +
                        std::to_string(s) + ")",
                    edges, nsteps, s, false);
        }
        graphs++;
    }

    if (failures) {
        std::fprintf(stderr, "%d slice-parity check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_slice_diff: viewer closure == src/dataflow.c slicer over "
                "the fixture + %d random graphs\n",
                graphs);
    return 0;
}
