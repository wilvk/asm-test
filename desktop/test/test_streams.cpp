// test_streams.cpp — decode_streams' dataflow segmentation by df_invocation
// (docs/internal/gui/40-segment-dataflow-by-invocation.md T1). A continuous
// capture (35 T1) appends many invocation passes, each restarting df_step at 0;
// this asserts build_segmented_dataflow splits them so no pass aliases another,
// that decode_streams resolves Streams::df to the LATEST pass, and that a one-shot
// recording (no marker) stays exactly one pass — byte-identical to the pre-40 flat
// decode.
#include <string>

#include "view_test.h"

#include "doc/streams.h"

using namespace asmdesk;

namespace {

Recording load_rec(const std::string &name) {
    std::string path = std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
    std::string err;
    auto r = load_recording_file(path, err);
    if (!r) {
        vt::fail("load " + name, err);
        return Recording{};
    }
    return *r;
}

// The value of a pass's first step-0 operand. The continuous fixture writes one
// distinguishing write-op per step, so the step-0 value identifies the pass.
uint64_t step0_value(const DataflowStream &df, const char *what) {
    for (const ValRec &v : df.recs)
        if (v.step == 0)
            return v.value;
    vt::fail(what, "pass has no step-0 operand");
    return 0;
}

// Three df_invocation passes, each restarting step at 0. The conflation this brief
// kills would fold all three into one step space (last-write-wins offsets; merged
// ops/edges); segmentation keeps each pass whole and independent.
void test_continuous_segments() {
    Recording r = load_rec("dishonest/continuous-df.asmtrace");
    SegmentedDataflow seg = build_segmented_dataflow(r);

    vt::eq("continuous: three passes", seg.passes.size(), size_t{3});
    vt::eq("continuous: all three present", seg.present_passes(), size_t{3});
    if (seg.passes.size() != 3)
        return;

    const DataflowStream &p0 = seg.passes[0];
    const DataflowStream &p1 = seg.passes[1];
    const DataflowStream &p2 = seg.passes[2];

    // The two clean passes: three steps at offsets {0,3,6}.
    vt::eq("pass0: nsteps", p0.nsteps, uint32_t{3});
    vt::eq("pass1: nsteps", p1.nsteps, uint32_t{3});
    if (p0.insn_off.size() == 3) {
        vt::eq("pass0: off[0]", p0.insn_off[0], uint64_t{0});
        vt::eq("pass0: off[1]", p0.insn_off[1], uint64_t{3});
        vt::eq("pass0: off[2]", p0.insn_off[2], uint64_t{6});
    }
    // Pass 2 is truncated: only two df_step events survived (its marker declares 5).
    vt::eq("pass2: nsteps (truncated to 2)", p2.nsteps, uint32_t{2});

    // Each pass's step-0 op carries ITS OWN value — proof the passes did not
    // conflate onto one shared step 0.
    vt::eq("pass0: step0 value", step0_value(p0, "pass0"), uint64_t{6});
    vt::eq("pass1: step0 value", step0_value(p1, "pass1"), uint64_t{10});
    vt::eq("pass2: step0 value", step0_value(p2, "pass2"), uint64_t{100});

    // decode_streams surfaces the LATEST pass as Streams::df — coherent, not the
    // max(step)+1==3 conflation over all passes, and its operands are pass 2's only.
    Streams s = decode_streams(r);
    vt::eq("Streams::df is latest (nsteps 2)", s.df.nsteps, uint32_t{2});
    vt::eq("Streams::df step0 is pass2's (100)", step0_value(s.df, "Streams::df"),
           uint64_t{100});
    vt::eq("Streams::df recs are pass2's only", s.df.recs.size(), p2.recs.size());
}

// A recording with no df_invocation marker is one pass over the whole df list —
// byte-identical to the pre-40 flat decode. mem-df-chain: 6 df_step + 4 df_edge,
// rbase stated, no marker.
void test_oneshot_is_one_pass() {
    Recording r = load_rec("mem-df-chain.asmtrace");
    SegmentedDataflow seg = build_segmented_dataflow(r);
    vt::eq("oneshot: one pass", seg.passes.size(), size_t{1});
    vt::eq("oneshot: present", seg.present_passes(), size_t{1});
    if (seg.passes.empty())
        return;

    const DataflowStream &only = seg.passes[0];
    vt::eq("oneshot: nsteps 6", only.nsteps, uint32_t{6});
    vt::eq("oneshot: edges 4", only.edges.size(), size_t{4});
    vt::check("oneshot: rbase present", only.rbase_present,
              "mem-df-chain states rbase on every df_step");

    // decode_streams surfaces that single pass unchanged — the counts and the
    // leading offset all agree, which is the single-pass path being the flat decode.
    Streams s = decode_streams(r);
    vt::eq("oneshot: Streams::df nsteps == pass", s.df.nsteps, only.nsteps);
    vt::eq("oneshot: Streams::df recs == pass", s.df.recs.size(), only.recs.size());
    vt::eq("oneshot: Streams::df edges == pass", s.df.edges.size(),
           only.edges.size());
    if (!s.df.insn_off.empty())
        vt::eq("oneshot: Streams::df off[0]", s.df.insn_off[0], uint64_t{0});
}

} // namespace

int main() {
    test_continuous_segments();
    test_oneshot_is_one_pass();
    return vt::report("test_streams");
}
