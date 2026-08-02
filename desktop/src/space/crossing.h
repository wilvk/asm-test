// crossing.h — the kernel-crossing spur layer's plane-space geometry
// (57-causal-layers.md T2): where control actually left userspace, shown ON
// the path being read rather than in a separate flat list.
//
// Pure data (a POD header, no .cpp), exactly like space/mispred.h: built by
// views/syscalls.cpp's `build_crossing_layer` (which needs SyscallView, a
// views/ type, so it cannot live in this space/-only island) and consumed by
// scene3d/. That split is what lets scene3d keep depending on space/ ONLY,
// never views/ (D4).
//
// **THE CLAIM THIS LAYER MUST NOT MAKE.** Nothing in the recording measures
// kernel dwell. A syscall carries a stream position (`Event::seq`, 54 T3) and
// nothing else about time — syscalls.h says so in its own words: seq "orders a
// syscall against a trace instruction; does NOT measure kernel dwell — no
// consumer may present it as a duration". So the out spur and the return spur
// MEET at one shared rail point: there is no along-rail extent for a reader to
// mistake for a duration, and `CrossingLayer::rail_span()` is 0 by
// construction (asserted by the layer test, so deleting the property fails a
// named check rather than silently reintroducing a fabricated duration).
#ifndef ASMDESK_SPACE_CROSSING_H
#define ASMDESK_SPACE_CROSSING_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace asmdesk::space {

// The derived syscall family a spur is hued by. DERIVED, never a recorded
// field: the schema carries a rendered `line`, not a class, so this is a
// parse of the name at the head of that line. `Other` is a REAL, visible grey
// bucket — every parser miss lands there, and nothing is ever folded into a
// known family on a miss (T2 step 3).
enum class SyscallClass {
    File,
    Net,
    Process,
    Memory,
    Signal,
    Time,
    Other, // parsed to no known family, OR no name could be read at all
};
// `inline`, not a .cpp: scene3d/ draws the legend from these names and must
// not link a views/ TU to get them (D4).
inline const char *syscall_class_name(SyscallClass c) {
    switch (c) {
    case SyscallClass::File:
        return "file";
    case SyscallClass::Net:
        return "net";
    case SyscallClass::Process:
        return "process";
    case SyscallClass::Memory:
        return "memory";
    case SyscallClass::Signal:
        return "signal";
    case SyscallClass::Time:
        return "time";
    case SyscallClass::Other:
        break;
    }
    return "other (unparsed)";
}

// The parsed outcome of the call, used to tint the RETURN spur only.
// `Unknown` is the visible grey bucket, on the same terms as
// SyscallClass::Other: a `= ?` return, a missing `=`, or anything that does
// not parse as a number lands here and is never read as success.
enum class SyscallOutcome { Unknown, Ok, Error };
inline const char *syscall_outcome_name(SyscallOutcome o) {
    switch (o) {
    case SyscallOutcome::Ok:
        return "ok";
    case SyscallOutcome::Error:
        return "error";
    case SyscallOutcome::Unknown:
        break;
    }
    return "unknown (unparsed)";
}

// One kernel crossing, anchored to a worldline vertex.
//
// The anchor is APPROXIMATE and says so in `anchor_label()`: it is the nearest
// RECORDED instruction before the syscall's stream position, not a measured
// crossing point. A trace that is sparse or truncated makes even that nearest
// instruction a weaker claim, which is what `hollow` marks.
struct CrossingSpur {
    size_t row = 0;   // index into SyscallView::rows — the drill-in target
    uint64_t seq = 0; // the syscall's own stream position (54 T3)

    // The anchor worldline vertex: the LAST trace instruction with a smaller
    // seq. `t` is that instruction's per-tid vertex ordinal — the same
    // `next_t[tid]++` counter build_trajectories assigns to TrajPoint::t, so
    // the spur hangs on the vertex the worldline actually drew. It is NOT a
    // dataflow step index (57 T1 / 46 G10); nothing here inverts it.
    uint64_t anchor_addr = 0;
    uint64_t anchor_t = 0;
    int32_t anchor_tid = -1;
    float anchor_u = 0, anchor_v = 0;
    uint32_t anchor_cell = 0;

    // The resume vertex: the FIRST trace instruction with a greater seq.
    // `has_resume == false` means the recording states no instruction after
    // this crossing — the return spur is then absent, never drawn to a
    // fabricated destination.
    bool has_resume = false;
    uint64_t resume_addr = 0;
    uint64_t resume_t = 0;
    float resume_u = 0, resume_v = 0;

    // The shared rail point the out and return spurs BOTH terminate at. One
    // point, so the pair has zero along-rail extent (see the header note).
    // `rail_lift` is a layer-wide constant, in the same world-Y units the
    // trajectory geometry uses.
    float rail_u = 0, rail_v = 0;

    SyscallClass cls = SyscallClass::Other;
    SyscallOutcome outcome = SyscallOutcome::Unknown;
    // Thickness channel: the payload's byte length when the wire carried one.
    // The BYTES ARE NEVER COPIED HERE — only their count — so this struct
    // cannot leak a payload into a scene, a dump or a golden file even by
    // accident (T2's own redaction bar, made structural).
    bool has_payload = false;
    uint64_t payload_bytes = 0;
    // The PRODUCER withheld payloads at record time (SyscallView::
    // record_redacted): the content is not in the file at all. Rendered
    // hatched, and its content is never rendered in 3D or in the drill-in.
    bool redacted = false;
    // The trace this anchor came from is sparse or truncated, so even the
    // nearest recorded instruction is a weaker claim: draw the anchor hollow.
    bool hollow = false;
};

struct CrossingLayer {
    std::vector<CrossingSpur> spurs;

    // The layer is OFF unless there is a worldline to hang a spur on and a
    // stream order to anchor by. `disabled_reason` is verbatim and NEVER
    // empty when `enabled` is false — a layer that is quietly absent is
    // indistinguishable from one that found nothing (T2 step 5).
    bool enabled = false;
    std::string disabled_reason;

    // Syscalls that could NOT be anchored, counted rather than dropped:
    // `before_first_insn` is the specific, dangerous case — a syscall whose
    // seq precedes every trace instruction has no earlier instruction, and
    // anchoring it to instruction 0 would be a fabrication. `off_plane` is an
    // anchor address the projection does not map.
    uint32_t before_first_insn = 0;
    uint32_t off_plane = 0;

    // The constant world-Y lift of the rail above the higher of the two
    // vertices a crossing joins. A CONSTANT, so the rail's height encodes
    // nothing (see the header note on why the geometry must not imply a
    // duration).
    float rail_lift = 0.35f;

    // 0 BY CONSTRUCTION: the out and return spurs share one rail point, so
    // there is no along-rail span. Kept as a function rather than a comment
    // so the layer test can assert it by name.
    static constexpr float rail_span() { return 0.0f; }

    // The two labels that ride in the DATA, so the HUD and the scene cannot
    // forget to state them (converge.h's ConvergenceSet::label() precedent).
    static const char *dwell_note() {
        return "kernel dwell not measured — the out and return spurs meet at "
               "one rail point";
    }
    static const char *anchor_label() {
        return "approx (last insn) — the nearest recorded instruction before "
               "the call, not a measured crossing point";
    }
    static const char *redacted_label() {
        return "withheld at record time";
    }
};

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_CROSSING_H
