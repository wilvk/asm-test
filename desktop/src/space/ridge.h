// ridge.h — the dominant-path ridge (57-causal-layers.md T5): at each fork,
// which successor control usually takes, and how much mass leaves the other
// way.
//
// Pure and engine-free (D4): the doc/ streams and the space/ models only.
//
// **THIS IS AN AGGREGATE, NOT A PATH.** `label()` is the words that must
// appear wherever this layer is shown — *"modal path (aggregate)"* — because
// threading a bright tube through a sequence of blocks is EXACTLY the shape a
// reader takes for "one run went this way", and it is not that claim. It is a
// per-fork histogram of transitions that were each observed, drawn along one
// visit order.
//
// **THE ONE PRIOR MISTAKE THIS TREE HAS ALREADY MADE TWICE.**
// `docs/internal/analysis/2026-07-17-blockstep-reconstruction-defects.md`
// records two static block-step reconstructors that shipped a GREEDY rule with
// the correct rule written in front of them, and notes that the emulator-replay
// tier is structurally immune "because it does not statically guess the
// terminator". This builder is immune the same way and for the same reason: it
// NEVER derives a successor. A transition exists here only when the recorded
// instruction stream actually went from one recorded block start to another.
// In particular:
//
//   * a block whose successor was never recorded is CAPPED — `RidgeCap`, drawn
//     as an unknown continuation. It never wraps to offset 0, and it is never
//     joined to the next block by address adjacency;
//   * an instruction is never attributed to a block by "the greatest recorded
//     block start below it" — that containment guess needs block LENGTHS the
//     wire does not carry, and it would silently fabricate membership for an
//     instruction in a block the recording never opened. Instructions seen
//     before the first recorded block start are counted
//     (`unattributed_insns`), not attributed.
#ifndef ASMDESK_SPACE_RIDGE_H
#define ASMDESK_SPACE_RIDGE_H

#include <cstdint>
#include <string>
#include <vector>

#include "doc/streams.h"
#include "space/projection.h"

namespace asmdesk::space {

// One observed block-to-block transition, projected onto the plane.
struct RidgeSegment {
    uint64_t from_addr = 0, to_addr = 0;
    uint64_t count = 0;      // times this exact transition was observed
    uint64_t from_total = 0; // all transitions observed leaving from_addr
    double fraction = 0.0;   // count / from_total — this successor's share
    bool modal = false;      // it is A max-count successor of from_addr
    bool self_loop = false;  // from_addr == to_addr (a real observed loop)
    float ua = 0, va = 0, ub = 0, vb = 0; // projected endpoints
    float height = 0.0f;                  // log1p(count): the tube's own axis
    // Program-visit order index: the order this transition was FIRST observed
    // in the instruction stream. The tube is threaded in this order, which is
    // a drawing order and not a claim about any single run.
    uint32_t visit = 0;
};

// A block with two or more observed successors.
struct RidgeFork {
    uint64_t addr = 0;
    float u = 0, v = 0;
    uint64_t total = 0;           // transitions observed leaving it
    uint32_t successors = 0;      // distinct successors observed
    double modal_fraction = 0.0;  // the largest successor's share
    double leaving_mass = 0.0;    // 1 - modal_fraction: the glyph's size
};

// A block the trace ENTERED and was never observed to leave. Its continuation
// is UNKNOWN, and the ridge caps there.
struct RidgeCap {
    uint64_t addr = 0;
    float u = 0, v = 0;
};

// The SURVEY fallback (step 6): the max-count outgoing statistical edge per
// block. Kept in its own struct, filled by a DIFFERENT function
// (views/hotedges.h's build_ridge_survey), so it is physically impossible to
// blend it into `PathRidge::segments`. Drawn in statistical ink, on the
// statistical layer's terms.
struct RidgeSurveyEdge {
    uint64_t from_addr = 0, to_addr = 0;
    uint64_t count = 0;
    float ua = 0, va = 0, ub = 0, vb = 0;
};
struct RidgeSurvey {
    std::vector<RidgeSurveyEdge> edges;
    uint32_t off_plane = 0;
    std::string sampler; // the survey's own sampler id, for the label
    static const char *label() {
        return "STATISTICAL — survey: the max-count outgoing sampled edge per "
               "block, never blended into the exact ridge";
    }
};

struct PathRidge {
    std::vector<RidgeSegment> segments; // in program-visit order
    std::vector<RidgeFork> forks;       // only blocks with >= 2 successors
    std::vector<RidgeCap> caps;         // entered, never observed to leave

    uint32_t off_plane = 0;          // endpoints the projection does not map
    uint32_t unattributed_insns = 0; // instructions before any recorded block
    uint32_t blocks_unvisited = 0;   // recorded blocks the insn stream never
                                     // reached (a truncation artefact, stated)

    // The trace is truncated (or its block/insn totals exceed what was kept):
    // every count here is a STATED LOWER BOUND.
    bool truncated = false;

    bool enabled = false;
    std::string disabled_reason;

    // The words that must appear wherever this layer is labelled.
    static const char *label() { return "modal path (aggregate)"; }
    static const char *aggregate_note() {
        return "modal path (aggregate): a per-fork transition histogram, NOT a "
               "claim that any single run followed this whole chain";
    }
    static const char *cap_note() {
        return "unknown continuation — this block's successor was never "
               "recorded";
    }
};

// Tie-dimming, as a PURE FUNCTION so it is pinned by a test rather than left
// to a shader constant.
//
// It is mandatory, not cosmetic: a 51/49 fork and a 99/1 fork must not look
// alike, because that difference is the entire distinction between a backbone
// and a coin toss. `fraction` 1.0 renders solid and bright; 0.5 — a true
// split — renders at exactly `kRidgeSplitBrightness`; a minority successor
// darkens further still.
inline constexpr float kRidgeSplitBrightness = 0.35f;
inline constexpr float kRidgeMinBrightness = 0.12f;
float ridge_brightness(double fraction);

// Build the exact ridge from a recording's `trace` stream, placed on `proj`.
// `truncated` is the recording's own truncation fact, threaded in by the
// caller.
PathRidge build_path_ridge(const TraceStream &trace, const Projection &proj,
                           bool truncated = false);

} // namespace asmdesk::space
#endif // ASMDESK_SPACE_RIDGE_H
