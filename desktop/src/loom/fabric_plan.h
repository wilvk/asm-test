// fabric_plan.h — the Loom's draw plan (05-loom-day-one.md T2).
//
// `loom_plan` is a PURE function of (fabric, camera) producing an ordered list
// of primitives. Nothing about zoom, collapse, or honesty chrome lives in the
// painter — which is what makes every one of those rules assertable headlessly,
// and what makes "two identical cameras produce byte-identical plans" a test
// rather than a hope. `fabric_imgui.cpp` walks the list and calls ImDrawList.
//
// The honesty prims are not decoration. Each one is the visual form of a fact
// the fabric knows and the user must not have to infer:
//   torn_edge            the recorded window ended before the run did
//   fade_out             this worldline was still live at the last recorded
//                        step — NOT a death, and never drawn as one
//   born_untraced_glyph  this worldline's provenance starts at instrumentation
//   guest_badge          this is an emulator replay, not silicon
#ifndef ASMDESK_LOOM_FABRIC_PLAN_H
#define ASMDESK_LOOM_FABRIC_PLAN_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "loom/fabric.h"

namespace asmdesk {

enum class loom_prim {
    lane_header,
    span,
    span_hollow,
    hop,
    knot,
    value_chip,
    density_ribbon,
    byte_row,
    // honesty chrome
    torn_edge,
    fade_out,
    born_untraced_glyph,
    guest_badge,
    // forks (T6)
    take_dim,
    take_hot,
    take_dashed_tail,
    patient_zero,
    fault_card,
};

const char *loom_prim_name(loom_prim k);

// One drawable. `a`/`b` carry the prim's indices into the fabric — which span,
// which lane, which edge — so a hit test resolves a click back to a worldline
// without the painter keeping a parallel map.
struct loom_prim_t {
    loom_prim kind = loom_prim::span;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t a = 0, b = 0;
    std::string text;
};

// The camera. `steps_per_px` is the horizontal scale (steps per pixel, so
// larger = further out); `lane0`/`lane_h` scroll and size the vertical deck.
struct loom_view_t {
    double step0 = 0;
    double steps_per_px = 1.0;
    int lane0 = 0;
    float lane_h = 18.0f;
    float px_w = 800.0f;
    float px_h = 400.0f;
    // Optional selection dimming (T3). When non-empty, spans and hops whose
    // step is outside it are marked dim via prim `b` bit 0.
    std::vector<uint32_t> selected_steps;
};

// Build the plan. Returns out->size(). Deterministic: the same (fabric, view)
// always yields the identical vector, prim for prim, byte for byte.
size_t loom_plan(const loom_fabric_t &f, const loom_view_t &v,
                 std::vector<loom_prim_t> *out);

// Deterministic dump — the golden surface for the plan tests.
std::string loom_plan_dump(const std::vector<loom_prim_t> &prims);

// --- the copy strings, in one place -----------------------------------------
// They are asserted verbatim by test_loom_chrome.cpp. A view that reworded one
// of these would be rewording a measurement claim, so the words live here and
// the tests pin them.
std::string loom_torn_text(const loom_provenance_t &p);
extern const char *const kLoomFadeOutText;
extern const char *const kLoomBornUntracedText;
extern const char *const kLoomGuestBadgeText;

} // namespace asmdesk
#endif // ASMDESK_LOOM_FABRIC_PLAN_H
