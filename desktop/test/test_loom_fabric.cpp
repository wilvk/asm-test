// test_loom_fabric.cpp — the fabric model's rules (05-loom-day-one.md T1).
//
// Engine-free by construction: this binary links fabric.o and nothing else, so
// its link line is the proof that the render-only viewer can weave a fabric
// (D4) — the same argument test_slice.cpp makes for the client-side closure.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "loom/fabric.h"
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

int main() {
    loomfx::Fixture fx;
    loom_provenance_t prov;
    prov.producer = "dataflow-emu";
    prov.exact = true;
    prov.isolated_guest = true;
    prov.steps_recorded = 6;
    prov.steps_total = 6;

    loom_fabric_t f;
    std::string err;
    if (!loom_fabric_build(&fx.vt, &fx.g, prov, &f, &err)) {
        fail("build df_chain", err);
        std::fprintf(stderr, "1 loom fabric check(s) failed\n");
        return 1;
    }

    // --- the fixed register deck ------------------------------------------
    // rdi rsi rdx rcx (arg order) ... rax ... rsp rip — only TOUCHED registers
    // materialize, but the order among those that do never changes.
    const char *want[] = {"rdi", "rsi", "rdx", "rcx", "rax", "rsp", "rip"};
    int prev = -1;
    for (const char *n : want) {
        int at = lane_named(f, n);
        check(std::string("deck has ") + n, at >= 0, "no lane named it");
        check(std::string("deck order ") + n, at > prev,
              "lane " + std::to_string(at) + " does not follow " +
                  std::to_string(prev));
        prev = at;
    }
    check("deck excludes untouched registers", lane_named(f, "rbx") < 0,
          "rbx is never touched by df_chain but got a lane");
    check("steps", f.steps == 6, "got " + std::to_string(f.steps));

    // --- exactly one memory band -------------------------------------------
    // The store slot and the return-address slot are 8 bytes apart, well inside
    // the 64-byte coalescing gap: one stack band, not two lanes.
    size_t bands = 0;
    size_t band_idx = 0;
    for (size_t i = 0; i < f.lanes.size(); i++)
        if (f.lanes[i].kind == loom_lane_kind::mem_band) {
            bands++;
            band_idx = i;
        }
    check("exactly one mem band", bands == 1,
          "got " + std::to_string(bands) + " bands");
    if (bands == 1) {
        check("band covers both stack slots",
              f.lanes[band_idx].lo == SLOT && f.lanes[band_idx].hi == SP + 8,
              f.lanes[band_idx].name);
        check("band names its space", f.lanes[band_idx].space == "abs",
              f.lanes[band_idx].space);
        check("mem band follows the register deck",
              band_idx + 1 == f.lanes.size(), "band is not last");
    }

    // --- the step-1 store span carries chip 7 ------------------------------
    bool store_ok = false;
    for (const loom_span_t &s : f.spans)
        if (s.lane == band_idx && s.t_write == 1)
            store_ok = s.value_valid && s.value == 7 && s.lo == SLOT;
    check("step-1 store span carries chip 7", store_ok,
          "no valid value-7 span written at step 1 on the band");

    // The store is never overwritten inside the window, so it is ALIVE at the
    // end — not dead, not freed: the fabric only knows nothing overwrote it.
    for (const loom_span_t &s : f.spans)
        if (s.lane == band_idx && s.t_write == 1)
            check("store span is alive at trace end", s.t_end == kLoomAlive,
                  "t_end = " + std::to_string(s.t_end));

    // rax is written at step 0 and again at step 4: the first span ENDS there.
    int rax = lane_named(f, "rax");
    bool rax0 = false;
    for (const loom_span_t &s : f.spans)
        if (static_cast<int>(s.lane) == rax && s.t_write == 0)
            rax0 = s.t_end == 4 && s.value == 7;
    check("rax span 0 ends at the step-4 rewrite", rax0, "wrong t_end/value");

    // --- hops ---------------------------------------------------------------
    // The load-after-store edge 1 -> 2 must resolve onto the band's worldline.
    bool hop12 = false;
    for (const loom_hop_t &h : f.hops) {
        const asmtest_defuse_edge_t &e = fx.edges[h.edge];
        if (e.from_step == 1 && e.to_step == 2)
            hop12 = f.spans[h.from_span].lane == band_idx &&
                    h.to_span != kLoomNoSpan && f.spans[h.to_span].t_write == 2;
    }
    check("hop mirrors edge 1 -> 2 on the mem band", hop12, "not found");
    check("every edge that resolves became a hop", f.hops.size() == 4,
          "got " + std::to_string(f.hops.size()) + " hops for 4 edges");

    // --- born of untraced state --------------------------------------------
    // rdi/rsi/rsp are read before anything in the window writes them. Their
    // worldlines start at instrumentation, and the entry args carry real chips
    // because the producer's pre-instruction hook captured source state.
    for (const char *n : {"rdi", "rsi", "rsp"}) {
        int lane = lane_named(f, n);
        bool found = false;
        for (const loom_span_t &s : f.spans)
            if (static_cast<int>(s.lane) == lane && s.born_untraced)
                found = true;
        check(std::string(n) + " is born of untraced state", found,
              "no synthetic entry span");
    }
    {
        int lane = lane_named(f, "rdi");
        for (const loom_span_t &s : f.spans)
            if (static_cast<int>(s.lane) == lane && s.born_untraced)
                check("the rdi entry span carries the argument value",
                      s.value_valid && s.value == 7,
                      "value " + std::to_string(s.value));
    }
    check(
        "rax is NOT born untraced",
        [&] {
            for (const loom_span_t &s : f.spans)
                if (static_cast<int>(s.lane) == rax && s.born_untraced)
                    return false;
            return true;
        }(),
        "rax's first record is a write, so it has no entry span");

    // --- the hollow thread --------------------------------------------------
    int rip = lane_named(f, "rip");
    bool hollow = false;
    for (const loom_span_t &s : f.spans)
        if (static_cast<int>(s.lane) == rip && !s.born_untraced)
            hollow = !s.value_valid;
    check("an invalid-value write yields a hollow span", hollow,
          "the deferred rip write should have value_valid = false");

    // --- residency ----------------------------------------------------------
    // The zeroization audit scrubs over exactly this: which span is resident on
    // a lane at time T.
    check("rax resident at 0 is the step-0 span",
          f.spans[f.resident(static_cast<uint32_t>(rax), 0)].value == 7, "");
    check("rax resident at 5 is the step-4 span",
          f.spans[f.resident(static_cast<uint32_t>(rax), 5)].value == 12, "");

    // --- knots --------------------------------------------------------------
    check("every df_chain step is a knot", f.knots.size() == 6,
          "got " + std::to_string(f.knots.size()));

    // --- the statistical-never-woven law ------------------------------------
    {
        loom_provenance_t stat = prov;
        stat.exact = false;
        stat.producer = "ibs";
        loom_fabric_t nope;
        std::string why;
        bool built = loom_fabric_build(&fx.vt, &fx.g, stat, &nope, &why);
        check("a statistical producer is refused", !built, "it wove anyway");
        check("the refusal names the rule",
              why.find("statistical") != std::string::npos &&
                  why.find("ibs") != std::string::npos,
              "err was: " + why);
        check("the refusal leaves NO partial fabric",
              nope.lanes.empty() && nope.spans.empty(), "out was written");
    }

    // --- determinism --------------------------------------------------------
    {
        loom_fabric_t again;
        std::string e2;
        loom_fabric_build(&fx.vt, &fx.g, prov, &again, &e2);
        check("two builds are byte-identical",
              loom_fabric_dump(again) == loom_fabric_dump(f), "dumps differ");
    }

    // --- the register fold --------------------------------------------------
    check("eax folds to rax", loom_fold_reg(19) == RAX, "");
    check("ah folds to rax", loom_fold_reg(1) == RAX, "");
    check("r15d folds to r15", loom_fold_reg(233) == 113, "");
    check("sil folds to rsi", loom_fold_reg(46) == RSI, "");
    check("an unmodelled id folds to itself", loom_fold_reg(9999) == 9999, "");
    check("eflags names itself", loom_reg_name(25) == "eflags",
          loom_reg_name(25));

    if (failures) {
        std::fprintf(stderr, "%d loom fabric check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_fabric: all checks passed\n");
    return 0;
}
