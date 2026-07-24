// test_loom_parity.cpp — the Loom's generation walk vs the C slicer
// (05-loom-day-one.md T3, "Done when: the full-build cross-check equals
// asmtest_slice_backward").
//
// THE ONE EXCEPTION to this doc's engine-free rule, exactly as
// test_slice_diff.cpp is 04's: this binary links build/dataflow.o so it can
// call asmtest_slice_forward_seed / _backward_seed and compare. It is a
// test-half binary only; nothing in desktop/src/ links it, which is why
// `asmtest-viewer` stays permissively distributable (D4).
//
// The claim under test is narrow and load-bearing: the BFS in lineage.cpp adds
// generation DEPTH, and adding depth must not change WHICH steps are reached.
// If it ever did, the Loom would light a different set of steps than the slice
// explorer beside it, over the same recording — and one of them would be wrong
// with no way for the user to tell which.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "loom/lineage.h"

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

// The C slicer's answer for one direction, as an ascending vector.
static std::vector<uint32_t>
c_slice(const std::vector<asmtest_defuse_edge_t> &e, uint32_t nsteps,
        uint32_t origin, bool forward) {
    asmtest_defuse_t g{};
    g.edges =
        const_cast<asmtest_defuse_edge_t *>(e.empty() ? nullptr : e.data());
    g.n = e.size();
    g.nsteps = nsteps;
    at_val_rec_t seed{};
    seed.step = origin;
    asmtest_slice_t *s = forward ? asmtest_slice_forward_seed(&g, &seed)
                                 : asmtest_slice_backward_seed(&g, &seed);
    std::vector<uint32_t> v;
    if (s != nullptr) {
        v.assign(s->steps, s->steps + s->n);
        asmtest_slice_free(s);
    }
    std::sort(v.begin(), v.end());
    return v;
}

// A one-register fabric over `nsteps`: every step writes the register, so every
// step owns a span and loom_select can seed on any of them. That isolates the
// closure relation from the lane model, which is what this test is about.
static loom_fabric_t line_fabric(uint32_t nsteps,
                                 const std::vector<asmtest_defuse_edge_t> &e,
                                 std::vector<at_val_rec_t> &recs,
                                 std::vector<uint64_t> &off,
                                 asmtest_valtrace_t &vt, asmtest_defuse_t &g) {
    recs.clear();
    off.assign(nsteps, 0);
    for (uint32_t s = 0; s < nsteps; s++) {
        at_val_rec_t r{};
        r.kind = AT_LOC_REG;
        r.reg = 35; // rax
        r.size = 8;
        r.is_write = true;
        r.value_valid = true;
        r.value = s;
        r.step = s;
        recs.push_back(r);
    }
    vt = asmtest_valtrace_t{};
    vt.insn_off = off.empty() ? nullptr : off.data();
    vt.steps_cap = vt.steps_len = off.size();
    vt.recs = recs.empty() ? nullptr : recs.data();
    vt.recs_cap = vt.recs_len = recs.size();
    vt.mem_space = AT_LOC_MEM_ABS;
    g = asmtest_defuse_t{};
    g.edges =
        const_cast<asmtest_defuse_edge_t *>(e.empty() ? nullptr : e.data());
    g.n = e.size();
    g.nsteps = nsteps;

    loom_provenance_t p;
    p.producer = "parity";
    p.exact = true;
    loom_fabric_t f;
    std::string err;
    if (!loom_fabric_build(&vt, &g, p, &f, &err))
        fail("parity fabric build", err);
    return f;
}

int main() {
    // A deterministic LCG — no <random>, whose engines are
    // implementation-defined across libstdc++/libc++ (test_slice_diff.cpp's
    // argument, and the same reason applies here).
    uint64_t seed = 0x5eed1234u;
    auto rnd = [&](uint32_t n) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return n == 0 ? 0u : static_cast<uint32_t>((seed >> 33) % n);
    };

    int graphs = 0, origins = 0;
    for (int t = 0; t < 200; t++) {
        uint32_t nsteps = 1 + rnd(32);
        uint32_t nedges = rnd(64);
        std::vector<asmtest_defuse_edge_t> e(nedges);
        for (uint32_t i = 0; i < nedges; i++) {
            e[i] = asmtest_defuse_edge_t{};
            // Endpoints past nsteps are deliberate: both closures must ignore
            // them rather than follow them, and disagreeing about THAT is the
            // most likely way the two implementations drift.
            e[i].from_step = rnd(nsteps + 2);
            e[i].to_step = rnd(nsteps + 2);
            e[i].loc.kind = AT_LOC_REG;
            e[i].loc.reg = 35;
            e[i].loc.size = 8;
        }
        std::vector<at_val_rec_t> recs;
        std::vector<uint64_t> off;
        asmtest_valtrace_t vt{};
        asmtest_defuse_t g{};
        loom_fabric_t f = line_fabric(nsteps, e, recs, off, vt, g);
        if (f.lanes.empty())
            continue;
        graphs++;

        for (uint32_t s = 0; s < nsteps; s++) {
            loom_selection_t sel;
            if (!loom_select(f, e.data(), e.size(), 0, s, &sel)) {
                fail("select",
                     "step " + std::to_string(s) + " has no resident");
                continue;
            }
            origins++;
            std::vector<uint32_t> want = c_slice(e, nsteps, s, false);
            std::vector<uint32_t> fwd = c_slice(e, nsteps, s, true);
            want.insert(want.end(), fwd.begin(), fwd.end());
            std::sort(want.begin(), want.end());
            want.erase(std::unique(want.begin(), want.end()), want.end());
            if (sel.steps != want) {
                fail("closure parity at step " + std::to_string(s),
                     "loom " + show(sel.steps) + " vs C slicer " + show(want));
                return 1; // one divergence is the whole story; stop shouting
            }
            // The generation of a step must also be reachable: a step in the
            // closure with no generation would break the [ / ] walk silently.
            if (sel.generation.size() != sel.steps.size()) {
                fail("generation parallelism", "sizes differ");
                return 1;
            }
            // Ancestors are negative, descendants positive, origin exactly 0.
            for (size_t i = 0; i < sel.steps.size(); i++)
                if (sel.steps[i] == sel.origin_step && sel.generation[i] != 0)
                    fail("origin generation",
                         "origin is at generation " +
                             std::to_string(sel.generation[i]));
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d loom parity check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_parity: loom closure == src/dataflow.c slicer over "
                "%d graphs, %d origins\n",
                graphs, origins);
    return 0;
}
