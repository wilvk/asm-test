// ptslice.h — the PT-replay def-use slice (08-observer-views.md T8).
//
// The promise: a live value slice with **zero single-steps of the target**. Every
// other data-flow capture asmspy has stops the process at every instruction; a
// PT host does not have to. Intel PT records the control-flow path in hardware,
// and the F5 producer replays that path through Unicorn against the code image
// to reconstruct the operand VALUES the hardware never recorded. The target
// never stops, so what is measured is a program running at its own speed.
//
// **No new rendering.** The result is a def-use stream, so 04's slice explorer
// and 05's fabric draw it exactly as they draw a replayed recording. This file
// is the plumbing and the gate, and that is all it should ever be.
//
// THE LIBRARY-EXPOSURE DECISION, stated rather than assumed. The PT replay
// producer ships **no public header on purpose** — the data-flow tier's
// deliberate pattern: a value-trace producer is a tier, not part of the shared
// sink API, and its smoke drivers re-declare its entry points
// (examples/test_dataflow_pt.c:71-82). This TU does the same, in ONE place.
// Promoting it to a public header is a library decision, and 08 does not make
// it. What 08 does do is defend the hazard that pattern carries: a re-declared
// struct silently skews if the real one gains a field, so the layout self-check
// (`asmtest_dataflow_pt_info_layout`) is asserted at init and its failure is a
// refusal to run, not a warning.
//
// THE GATE has two levels and they are not the same fact:
//  - **capture** needs PT silicon + libipt. That is hardware, so it self-skips
//    with the library's own measured reason.
//  - **replay** needs only Unicorn + Capstone, because the path is already
//    decoded and recorded (`stitch`) and the bytes with it (`codeimage`). So a
//    recording made on a PT box replays anywhere the full app builds — which is
//    also how this is tested without PT hardware.
#ifndef ASMDESK_LIVE_PTSLICE_H
#define ASMDESK_LIVE_PTSLICE_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/recording.h"
#include "doc/streams.h"

namespace asmdesk {

// What the gate depends on. Every field is DATA the caller probed, so every
// combination is assertable on a host that has none of it.
struct PtSliceFacts {
    // This binary links the PT replay producer at all. False in the
    // render-only viewer (D4: it links no engine), and the reason a user sees
    // there must say so rather than blaming the host.
    bool producer_linked = false;
    // Live PT CAPTURE availability, from asmtest_hwtrace_status(PT).
    bool capture_available = false;
    std::string capture_reason; // VERBATIM from the library; "" when available
    // The re-declared-struct layout self-check.
    bool layout_ok = true;
    std::string layout_detail;
};

struct PtSliceGate {
    bool can_replay = false;  // a recorded path can be replayed here
    bool can_capture = false; // a live PT capture can be started here
    std::string reason;       // why not; "" when both are true
};

PtSliceGate ptslice_gate(const PtSliceFacts &f);

// Probe this binary + host. `producer_linked` is a compile-time fact; the
// capture status comes from the library's own API where it is linked.
PtSliceFacts ptslice_facts();

// One replayable unit, assembled from a recording: the region's bytes as of the
// code-image version the path was decoded against, plus the executed offsets.
//
// This is why `stitch` carries a `version` per slice (schema, `stitch`): a PT
// path is only meaningful against the bytes that were live while it was
// recorded, and on a JIT those change under the same address. Pairing the two
// by version is the whole reason both kinds exist.
struct PtSliceInput {
    uint64_t base = 0;
    std::vector<uint8_t> code;
    std::vector<uint64_t> path; // executed offsets, in order
    long tid = 0;
    std::vector<long> args; // SysV integer entry arguments (<= 6)
    std::string error; // non-empty: why this recording cannot feed a replay
};

// Assemble from the recording's `stitch` + `codeimage` events. `tid == 0` takes
// the first stitch. PURE: no producer, no engine — so the assembly rules are
// tested everywhere, including in the render-only build.
PtSliceInput ptslice_input_from(const Recording &r, long tid = 0);

struct PtSliceResult {
    bool ok = false;
    std::string reason; // VERBATIM from the producer's info.reason, or the gate
    DataflowStream df;  // feeds 04's slice explorer / 05's fabric unchanged
    uint64_t steps = 0;
    uint64_t path_len = 0;
    uint64_t diverged_at = 0; // the first step whose offset did not match
    bool diverged = false;
    bool pure = false;      // the producer's region-purity verdict
    bool truncated = false; // a partial replay: values stop at the divergence
};

// Replay one input. Never throws, never partially reports: a refusal carries
// its reason and an empty stream, because a half-populated slice would be
// indistinguishable from a short one.
PtSliceResult ptslice_run(const PtSliceInput &in);

// A recording's own claim about how it was captured, for the chrome: a PT slice
// is exact about the PATH (hardware-recorded) and RECONSTRUCTED about the
// values (an emulator replay of that path). Those are two different grades of
// evidence in one view and the label says both.
const char *ptslice_disclosure();

} // namespace asmdesk
#endif // ASMDESK_LIVE_PTSLICE_H
