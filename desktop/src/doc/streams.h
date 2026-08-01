// streams.h — the DECODED streams every replay view renders from
// (docs/internal/archive/gui/04-replay-views.md, the "what dt_recording must expose"
// list). 03's Recording keeps each event as raw JSON grouped by kind, which is
// exactly right for a loader; a view wants typed columns. This is that decode,
// done once per recording at open, and it is additive to 03 — recording.h is
// untouched.
//
// Engine-free (D4): only the standard library, nlohmann/json and slice.h's
// dt_edge. Including asmtest_valtrace.h would be legal (it is plain structs),
// but the operand shape here mirrors what the SCHEMA serialises rather than
// what the C struct holds, and the two are not the same thing — the schema
// omits `value` when `value_valid` is false, and a >8-byte operand's bytes ride
// in the `bytes` hex field (28 R1 T3), decoded here into ValRec::bytes, or are
// absent (the view degrades to [wide]). A decoder that invented either would
// show values no producer wrote.
#ifndef ASMDESK_DOC_STREAMS_H
#define ASMDESK_DOC_STREAMS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "analysis/slice.h"
#include "doc/recording.h"

namespace asmdesk {

// One operand record: schema `df_step.ops[]` and `df_edge.loc`, which share a
// shape (asmtest_valtrace.h's at_val_rec_t as serialised).
struct ValRec {
    uint32_t step = 0; // owning step; 0 and meaningless for an edge's loc
    std::string space; // "reg" | "abs" | "off" (the AT_LOC_* enum as a token)
    uint32_t reg = 0;  // Capstone register id, for space == "reg"
    uint64_t addr = 0; // effective address, for the memory spaces
    uint32_t size = 0;
    bool write = false;
    bool value_valid = false; // false => the value was NOT captured
    bool wide = false;        // > 8 bytes: inline `value` is absent
    uint64_t value = 0;
    // The wide operand's bytes (28 R1 T3), decoded from the `bytes` hex field in
    // memory order; empty when the producer did not serialize them (the reader
    // then degrades to "[wide]"). A view shows these instead of a placeholder.
    std::vector<uint8_t> bytes;
};

// The exact execution stream: `trace` + `coverage` events.
struct TraceStream {
    std::vector<uint64_t> insns;            // ordered, one per `trace` event
    std::vector<uint64_t> blocks;           // de-duplicated, ascending
    std::map<uint64_t, std::string> disasm; // D10: recorded text, per offset

    // `basis` is NEVER defaulted (schema rule). The first one seen wins;
    // a second, different one sets basis_error and the views refuse to render
    // rather than mis-attributing every row.
    std::string basis;
    std::string basis_error;

    uint64_t blocks_total = 0; // totals SEEN, which count past the buffer caps
    uint64_t insns_total = 0;  // (blocks_total > blocks.size() == truncation)
    bool truncated = false;    // a coverage event declared it

    bool present() const { return !insns.empty() || !blocks.empty(); }
};

// The L0/L1 dataflow stream: `df_step` + `df_edge` events.
struct DataflowStream {
    std::vector<uint64_t> insn_off; // per step
    // 37: per step, the region base `off` is relative to (df_step.rbase); 0 where
    // the wire did not state one (a pre-37 recording, or a producer that omitted
    // it). One DataflowStream now holds exactly ONE invocation pass (40): a
    // continuous capture's passes are split by build_segmented_dataflow before they
    // reach here, so the per-pass `step` restart (35 T1) no longer aliases offsets
    // OR bases across passes — every index in this vector belongs to one pass.
    std::vector<uint64_t> insn_rbase;
    bool rbase_present = false; // any df_step of THIS pass stated an rbase
    // 37 T4: per step, the codeimage `when` in force when this step's invocation
    // STARTED (df_step.when); 0 where the wire did not state one (only the serve
    // path ever does — the headless sink and both corpus recorders hold no
    // codeimage timeline). Constant across every step of one pass by
    // construction (the producer samples it once before the pass), but carried
    // per-step so a reader never has to special-case a pass boundary. Keyed
    // together with insn_rbase[step] — `when` alone is not distinguishing (it
    // restarts at each re-armed span in a candidate walk).
    std::vector<uint64_t> insn_when;
    bool when_present = false;       // any df_step of THIS pass stated a when
    std::vector<std::string> disasm; // per step; "" where the producer had none
    std::vector<ValRec> recs;        // ALL steps' operands, ascending by step
    std::vector<dt_edge> edges;      // def-use endpoints (slice input)
    std::vector<ValRec> edge_loc; // parallel to `edges`: the carried location
    uint32_t nsteps = 0;

    // asmspy_df_annotate's cursor contract requires records grouped and in
    // ascending step order. The decoder sorts to guarantee it and records the
    // fact so a view can say so rather than silently mis-annotating.
    bool recs_grouped = true;
    uint32_t steps_missing = 0;     // step indices no df_step event covered
    std::vector<char> step_present; // per step: did a df_step event cover it?

    // A step index the recording never described. Its offset is NOT 0 — it is
    // unknown, and a view must render it as unknown rather than as an
    // instruction at the start of the routine.
    bool has_step(uint32_t step) const {
        return step < step_present.size() && step_present[step] != 0;
    }

    bool present() const { return nsteps > 0; }
};

// The dataflow stream segmented by df_invocation (35 T1): one DataflowStream per
// continuous-capture pass, oldest first. A one-shot recording (no marker) is
// exactly ONE pass over the whole df_step/df_edge list — byte-identical to the
// pre-40 flat decode. Each pass restarts step at 0, so passes never alias into
// one another (the KNOWN-ALIASING hazard the DataflowStream comment above flagged
// is closed here, not deferred). Mirrors analysis/stepindex.h's SegmentedStepIndex
// exactly: build_segmented_dataflow : DataflowStream :: build_segmented_step_index
// : StepIndex, and decode_streams resolves Streams::df to passes[latest()] the way
// build_step_index resolves the register ring — latest is the live default.
struct SegmentedDataflow {
    std::vector<DataflowStream> passes; // one per pass, oldest first
    // The latest (newest) pass — the live default. 0 when passes is empty.
    size_t latest() const { return passes.empty() ? 0 : passes.size() - 1; }
    // Any pass carries a df_step block (an edges-only or empty pass does not).
    bool present() const {
        for (const DataflowStream &p : passes)
            if (p.present())
                return true;
        return false;
    }
    // How many passes carry a df_step block.
    size_t present_passes() const {
        size_t n = 0;
        for (const DataflowStream &p : passes)
            if (p.present())
                n++;
        return n;
    }
};

// One statistical hot edge (`survey`). STATISTICAL: never merged into exact
// heat, always labelled where it is shown (schema Provenance rule).
struct SurveyEdge {
    uint64_t from = 0, to = 0;
    uint64_t count = 0, mispred = 0;
};

// One `blame` backward-attribution (33 R6 T1): the value at `step`/`off`
// (identified by the optional `loc`) and its ascending backward def-use cone —
// the producing steps, INCLUDING the sink. `born_untraced` is the fidelity
// verdict: the value has no traced producer (the cone is the sink alone), so its
// provenance starts at instrumentation — never presented as an empty cone.
struct BlameAttr {
    uint32_t step = 0;
    uint64_t off = 0;
    bool has_loc = false;
    ValRec loc;
    std::vector<uint32_t> cone; // ascending producing steps (sink included)
    bool born_untraced = false;
};

// One `statediff` step-to-step delta (33 R6 T2): the registers whose value
// changed from the previous held step (name -> NEW value), and whether the delta
// was `computed` from a known predecessor. `computed == false` (the first held
// step / across an evicted boundary) carries an EMPTY `changed` and must never be
// read as "nothing changed".
struct StateDelta {
    uint32_t step = 0; // absolute step (past any evicted prefix)
    std::map<std::string, uint64_t> changed; // reg name -> new value
    bool computed = false;
};

// Everything the replay views need, decoded once.
struct Streams {
    TraceStream trace;
    DataflowStream df;
    std::vector<SurveyEdge> survey;
    uint64_t survey_samples = 0;
    uint64_t survey_lost = 0;
    std::vector<BlameAttr> blame;      // `blame` events (33 R6 T1)
    std::vector<StateDelta> statediff; // `statediff` events (33 R6 T2)

    // The Recording's fidelity facts, lifted so a builder needs only this.
    bool truncated = false; // footer truncation OR torn OR a truncated coverage
    bool torn = false;
    uint64_t lost = 0;
    bool throttled = false;

    // The footer's `steps_total` (28 R1 T2): the total step count the producer
    // saw. has_steps_total is false when the footer omitted it, and the Loom
    // keeps its trace-derived fallback for the M.
    bool has_steps_total = false;
    uint64_t steps_total = 0;
    bool statistical = false; // provenance.exact == false
    bool redacted = false;

    std::string arch;
    std::string backend;
    std::string id; // the recording's basename — the deep-link `rec` key

    // The header's `code` routine identity (28 R1 T1), lifted so dt_diff_build
    // can refuse a wrong-routine pair. code_present is false when the producer
    // omitted it, and the diff then keeps its faithful "identity not checked"
    // caveat rather than asserting sameness.
    bool code_present = false;
    std::string code_sha;  // lowercase-hex SHA-256 of the routine bytes
    std::string code_name; // the routine name (informational)
    uint64_t code_len = 0;
};

// Decode one loaded Recording. Total: an event this does not understand is
// skipped, exactly as the loader's forward-compat rule requires. Streams::df is
// the LATEST invocation pass (the live default) — see build_segmented_dataflow.
Streams decode_streams(const Recording &r);

// The dataflow stream, one DataflowStream per continuous-capture pass (35 T1),
// bucketed by the `df_invocation` markers' stream position (Event::seq) exactly as
// build_segmented_step_index buckets the register ring. A one-shot recording (no
// marker) yields exactly ONE pass over the whole df_step/df_edge list —
// byte-identical to the pre-40 flat decode. Use this to address a pass other than
// the latest; decode_streams already exposes the latest as Streams::df.
SegmentedDataflow build_segmented_dataflow(const Recording &r);

// The deep-link id of a recording: its path's basename. Recordings are opened
// by path and the schema carries no recording id, so the basename is the only
// stable, user-visible handle there is — and a link that names one is
// reproducible from a shell.
std::string recording_id(const std::string &path);

} // namespace asmdesk
#endif // ASMDESK_DOC_STREAMS_H
