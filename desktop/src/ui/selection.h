// selection.h — the ONE shared brushing-and-linking selection model
// (docs/internal/gui/22-selection-and-search.md T1, F7).
//
// Before this, every pane held its own selection and the analyst re-found the
// same address by hand in each one — the exact recall load recognition-over-
// recall exists to remove. This is the dataviz convention (Perfetto's linked
// Current Selection, speedscope's sandwich): ONE selection model that cross-
// highlights the same entity everywhere it appears, and ONLY there.
//
// It is DISTINCT from navigation. `dt_nav_table::current` records "where the
// view is pointed"; the Selection records "what entity is brushed". A pick sets
// both; a plain view-switch (1/2/3/4) sets only nav. A cone (cone_active/
// cone_fwd on ShellState) is a derived highlight OF the selection, not a second
// selection — it stays beside this, keyed on `step`.
//
// Pure: only the standard library, so ShellState (ui/shell.h) and the Loom draw
// (loom/loom_draw.h) both include it with no dependency, and the null-backend
// selection test asserts it as model state (D4).
#ifndef ASMDESK_UI_SELECTION_H
#define ASMDESK_UI_SELECTION_H

#include <cstdint>
#include <optional>
#include <string>

namespace asmdesk {

// The canonical brushed entity — a dt_link-shaped {rec, step, off, lane}. `epoch`
// bumps on every change, so a pane can cheaply notice "the selection moved since I
// last drew" without deep-comparing. A pane that cannot show the entity (no such
// offset/step in its stream) shows NOTHING selected — never a fabricated row (the
// honesty floor, D7).
struct Selection {
    std::string rec;              // recording id (basename) the entity lives in
    std::optional<uint32_t> step; // dataflow step index
    std::optional<uint64_t> off;  // code offset, in the recording's basis
    int lane = -1;                // Loom lane, when the pick came from the fabric
    uint64_t epoch = 0;           // monotonic; bumped on every set/clear

    // Set the brushed entity and bump the epoch. This is the ONE writer every
    // pick funnels through (the router's go() lambda, the keymap step keys, a
    // Loom click, a 3D drill), so a link and a keypress brush identically (D4).
    void set(const std::string &r, std::optional<uint32_t> s,
             std::optional<uint64_t> o, int ln = -1) {
        rec = r;
        step = s;
        off = o;
        lane = ln;
        ++epoch;
    }

    // Nothing brushed. Bumps the epoch so panes re-read (and dim to nothing).
    void clear() {
        step.reset();
        off.reset();
        lane = -1;
        ++epoch;
    }

    bool active() const { return step.has_value() || off.has_value(); }
};

} // namespace asmdesk
#endif // ASMDESK_UI_SELECTION_H
