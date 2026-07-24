// streams.h — the DECODED streams every replay view renders from
// (docs/internal/gui/04-replay-views.md, the "what dt_recording must expose"
// list). 03's Recording keeps each event as raw JSON grouped by kind, which is
// exactly right for a loader; a view wants typed columns. This is that decode,
// done once per recording at open, and it is additive to 03 — recording.h is
// untouched.
//
// Engine-free (D4): only the standard library, nlohmann/json and slice.h's
// dt_edge. Including asmtest_valtrace.h would be legal (it is plain structs),
// but the operand shape here mirrors what the SCHEMA serialises rather than
// what the C struct holds, and the two are not the same thing — the schema
// omits `value` when `value_valid` is false, and the >8-byte side buffer is a
// documented v1 non-field. A decoder that pretended otherwise would invent
// values no producer wrote.
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
    bool wide = false;        // > 8 bytes: `value` is absent, by v1 design
    uint64_t value = 0;
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
    std::vector<uint64_t> insn_off;  // per step
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

// One statistical hot edge (`survey`). STATISTICAL: never merged into exact
// heat, always labelled where it is shown (schema Provenance rule).
struct SurveyEdge {
    uint64_t from = 0, to = 0;
    uint64_t count = 0, mispred = 0;
};

// Everything the replay views need, decoded once.
struct Streams {
    TraceStream trace;
    DataflowStream df;
    std::vector<SurveyEdge> survey;
    uint64_t survey_samples = 0;
    uint64_t survey_lost = 0;

    // The Recording's honesty facts, lifted so a builder needs only this.
    bool truncated = false; // footer truncation OR torn OR a truncated coverage
    bool torn = false;
    uint64_t lost = 0;
    bool throttled = false;
    bool statistical = false; // provenance.exact == false
    bool redacted = false;

    std::string arch;
    std::string backend;
    std::string id; // the recording's basename — the deep-link `rec` key
};

// Decode one loaded Recording. Total: an event this does not understand is
// skipped, exactly as the loader's forward-compat rule requires.
Streams decode_streams(const Recording &r);

// The deep-link id of a recording: its path's basename. Recordings are opened
// by path and the schema carries no recording id, so the basename is the only
// stable, user-visible handle there is — and a link that names one is
// reproducible from a shell.
std::string recording_id(const std::string &path);

} // namespace asmdesk
#endif // ASMDESK_DOC_STREAMS_H
