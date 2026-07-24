// timeline.h — the operand-value timeline (04-replay-views.md T4).
//
// Per recorded step: the disassembly (or the bare offset — D10), the captured
// VALUES as annotation tokens, and the def-use in/out counts. The annotation
// grammar is NOT reimplemented here: the rows are built by calling
// cli/asmspy_dataview.h's `asmspy_df_annotate` / `_defuse_counts` / `_loc_str`
// on a hand-filled asmtest_valtrace_t VIEW of the recording's own vectors, so
// the TUI and the GUI cannot drift into two dialects of "->0x2a". The test
// copies two annotation strings as literals; a divergence fails the build.
//
// The one helper deliberately NOT used is `asmspy_df_rowstyle` — it calls
// asmtest_slice_contains, a src/dataflow.c symbol, and the render-only viewer
// links no engine objects (D4). `dt_rowstyle` below is its mirror over dt_slice.
#ifndef ASMDESK_VIEWS_TIMELINE_H
#define ASMDESK_VIEWS_TIMELINE_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "analysis/slice.h"
#include "doc/streams.h"

namespace asmdesk {

// The mirror of asmspy_df_rowstyle (cli/asmspy_dataview.h:112) over the
// client-side slice: with no cone active every row is NORMAL; with one active a
// step inside it is IN-SLICE and one outside is DIMMED.
enum class dt_rowstyle { normal, in_slice, dimmed };
dt_rowstyle dt_row_style(const dt_slice *cone, uint32_t step);

struct dt_timeline_row {
    uint32_t step = 0;
    uint64_t off = 0;
    std::string disasm; // recorded text, or "" -> the view shows the offset
    std::string ann;    // the value annotation, asmspy_df_annotate's grammar
    size_t n_in = 0, n_out = 0;
    dt_rowstyle style = dt_rowstyle::normal;
    bool unaligned = false; // T7: past the divergence in a two-recording view
    // No df_step event covered this step: its offset and operands are UNKNOWN,
    // not zero. The draw greys the row and the dump says so, because a row
    // reading "0x0" would claim the routine's entry instruction ran here.
    bool missing = false;
};

struct dt_timeline {
    std::vector<dt_timeline_row> rows; // step order
    bool truncated = false;
    std::string banner;
    std::optional<uint32_t> selected_step;
    std::optional<uint32_t> div_step; // T7: where A and B stop agreeing
    bool two_up = false;
};

// `cone` is optional; when non-null, rows carry IN-SLICE / DIMMED emphasis.
dt_timeline dt_timeline_build(const Streams &s, const dt_slice *cone = nullptr);
// Two-recording form: rows past the divergence are marked `unaligned` so the
// draw can separate them. They are NEVER silently rendered as agreement.
dt_timeline dt_timeline_build2(const Streams &a, const Streams &b,
                               const dt_slice *cone = nullptr);

std::string dt_timeline_dump(const dt_timeline &t);

} // namespace asmdesk
#endif // ASMDESK_VIEWS_TIMELINE_H
