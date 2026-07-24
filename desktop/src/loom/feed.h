// feed.h — the Loom's two feeders (05-loom-day-one.md T1).
//
// One build path, two ways in:
//   replay — a recording's `df_step` / `df_edge` events, decoded by 02's reader
//            into a `DataflowStream`, materialized back into the C structs the
//            fabric builder consumes;
//   live/fork — T5 hands producer output (an `asmtest_valtrace_t` +
//            `asmtest_defuse_t`) straight to `loom_fabric_build`, unchanged.
//
// DOC CORRECTION (2026-07-24). 05-T1 says "02's reader materializes a
// recording's dataflow-step + defuse-edge events back into an
// asmtest_valtrace_t + asmtest_defuse_t". 02 shipped a decoded `DataflowStream`
// instead (doc/streams.h), which is a better reader output — it is honest about
// what the SCHEMA carries rather than about what the C struct holds. The code
// wins: the materialization lives here, on the consumer side, exactly as
// views/timeline.cpp already does it for the annotation grammar.
//
// The struct is an OWNER: `asmtest_valtrace_t` and `asmtest_defuse_t` are views
// over caller-owned buffers, so the vectors and the structs must not drift
// apart. `vt()` / `g()` re-point the structs on every call, which makes copying
// or moving a feed safe.
#ifndef ASMDESK_LOOM_FEED_H
#define ASMDESK_LOOM_FEED_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "loom/fabric.h"

namespace asmdesk {

struct loom_feed_t {
    std::vector<uint64_t> insn_off;
    std::vector<at_val_rec_t> recs;
    std::vector<asmtest_defuse_edge_t> edges;
    loom_provenance_t prov;

    const asmtest_valtrace_t *vt() const;
    const asmtest_defuse_t *g() const;

  private:
    mutable asmtest_valtrace_t vt_{};
    mutable asmtest_defuse_t g_{};
};

// Materialize a decoded recording. `producer` names the recording's backend and
// `exact` its provenance — both come from the recording, never re-derived, so a
// statistical recording arrives here still marked statistical and is refused by
// loom_fabric_build rather than quietly woven.
loom_feed_t loom_feed_from_streams(const Streams &s);

// Convenience: decode + build in one call. False + `err` on the refusals
// loom_fabric_build makes, or when the recording carries no dataflow at all
// (the fabric's only day-one L0 producer is the x86-64 emulator value producer;
// a trace-only recording has no values to weave and says so).
bool loom_fabric_from_streams(const Streams &s, loom_feed_t *feed,
                              loom_fabric_t *out, std::string *err);

} // namespace asmdesk
#endif // ASMDESK_LOOM_FEED_H
