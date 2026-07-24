// canvas.h — the trace canvas (docs/internal/gui/04-replay-views.md T3).
//
// Per-offset heat, block boundaries, a coverage gutter, and the recorded disasm
// text where the producer attached it (D10 — absent degrades to bare offsets and
// says so, it never errors). The builder is pure and carries every rule; the
// draw (canvas_draw.cpp) is thin.
//
// Two rules here are refusals rather than renderings:
//
//   - MIXED BASES DRAW NOTHING. A stream carrying both region-relative and
//     absolute events would mis-attribute every row, so `basis_error` is set and
//     `rows` is left EMPTY; the pane shows a placard instead. A canvas that
//     stacked the two would be confidently wrong at every offset.
//   - COVERAGE IS BLOCK-GRANULAR. The gutter marks block starts, because that
//     is what a `coverage` event records. Marking every executed instruction
//     "covered" would claim per-instruction coverage the data does not carry.
#ifndef ASMDESK_VIEWS_CANVAS_H
#define ASMDESK_VIEWS_CANVAS_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "doc/streams.h"

namespace asmdesk {

struct dt_canvas_row {
    uint64_t off = 0;
    uint32_t heat = 0; // occurrences in the ordered insn stream (EXACT)
    bool block_start = false;
    bool covered = false; // == block_start; coverage is block-granular
    std::string disasm;   // recorded text, or "" (render the bare offset)

    // Set only when a B recording is attached (T7). `heat_b` is B's count at
    // this offset; `in_a`/`in_b` say which side's coverage carries the block.
    uint32_t heat_b = 0;
    bool in_a = false, in_b = false;
    bool delta = false; // this row exists in the A/B comparison at all
};

struct dt_canvas {
    std::vector<dt_canvas_row> rows; // ascending by off
    std::string basis;               // "rel" | "abs" (the schema's tag)
    std::string basis_error;         // non-empty => NO rows; draw the placard
    bool truncated = false;
    std::string banner; // "" when the recording is complete
    std::optional<uint64_t> selected_off;

    bool two_up = false;              // built against a B recording
    std::optional<uint32_t> div_step; // T7: the "patient zero" step, if any
    std::optional<uint64_t> div_off; // and the offset it sits at, on the A side
};

dt_canvas dt_canvas_build(const Streams &s);
// Two-recording form (plan D3: every view takes one OR two recordings).
dt_canvas dt_canvas_build2(const Streams &a, const Streams &b);

// Deterministic dump, one line per row — the golden-test surface.
std::string dt_canvas_dump(const dt_canvas &c);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_CANVAS_H
