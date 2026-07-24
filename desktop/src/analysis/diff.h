// diff.h — two recordings, aligned (docs/internal/gui/04-replay-views.md T6).
//
// Plan D3 makes diff a PRIMITIVE, not a view feature: coverage union/delta,
// per-offset heat delta, hot-edge delta, and the first divergence of the shared
// prefix. This alignment seam is also the Loom's patient-zero mechanism
// (05-loom-day-one.md consumes dt_divergence) — the logic lives here once.
//
// Three honesty rules are structural, not cosmetic:
//
//  1. A refused pair yields an EMPTY diff and a reason, never a plausible one.
//     Recordings in different address bases, or of different architectures,
//     cannot be aligned; producing numbers anyway would be worse than useless.
//  2. `bounded` marks a verdict that only covers the recorded prefix. If either
//     side is truncated, "no divergence" means "none observed within what was
//     recorded" — a view must never render that as "identical".
//  3. Hot-edge deltas keep their statistical provenance attached and are never
//     merged into the exact heat delta (schema Provenance rule).
//
// ROUTINE IDENTITY IS NOT VERIFIABLE IN v1. The doc that specified this task
// assumed a code-bytes hash in the recording header; the shipped schema
// (docs/internal/gui/asmtrace-schema.md, Envelope) has no such field, and the
// code wins. So the precondition check covers what the format actually carries
// — arch and address basis — and `identity_note` states plainly that the caller
// is asserting the two recordings are of the same routine. Silently pretending
// to have checked would be exactly the false confidence this tree exists to
// avoid; a header identity field is a Phase-3-freeze item.
#ifndef ASMDESK_ANALYSIS_DIFF_H
#define ASMDESK_ANALYSIS_DIFF_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"

namespace asmdesk {

// The first step at which the two ordered instruction streams disagree.
struct dt_divergence {
    bool diverged = false;
    // The verdict covers only the shorter RECORDED prefix: at least one side
    // was truncated, so agreement past it was never observed.
    bool bounded = false;
    uint32_t step = 0;
    uint64_t off_a = 0, off_b = 0;
};

struct dt_heat_delta {
    uint64_t off = 0;
    uint32_t a = 0, b = 0;
};

// STATISTICAL. Views label this; nothing merges it into dt_heat_delta.
struct dt_edge_delta {
    uint64_t from = 0, to = 0;
    uint64_t a = 0, b = 0;
};

struct dt_diff {
    std::vector<uint64_t> only_a, only_b, both; // block coverage, ascending
    std::vector<dt_heat_delta> heat;            // offsets whose counts differ
    std::vector<dt_edge_delta> edges;           // statistical
    dt_divergence div;

    // Non-empty when the recordings could not be compared at all.
    std::string err;
    // Always set: what this build could and could not verify about the pair.
    std::string identity_note;
    // Per-side truncation, so a view can name WHICH side bounds the verdict.
    bool a_truncated = false, b_truncated = false;
};

// Align two recordings. Returns false with `out.err` set (and everything else
// empty) on a refused pair.
bool dt_diff_build(const Streams &a, const Streams &b, dt_diff &out,
                   std::string &err);

// Deterministic one-line-per-fact dump — the golden-test surface.
std::string dt_diff_dump(const dt_diff &d);

} // namespace asmdesk
#endif // ASMDESK_ANALYSIS_DIFF_H
