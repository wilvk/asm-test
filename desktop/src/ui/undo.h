// undo.h — the app-level command / undo stack
// (docs/internal/archive/gui/22-selection-and-search.md T4, F12).
//
// Ctrl+Z / Ctrl+Y over reversible VIEW-MODEL state: the filter predicate, the
// cone/selection, and the Loom take set. It is DELIBERATELY DISTINCT from the
// Author editor's own text undo (doc 17 T2, which owns its buffer): both guard on
// io.WantTextInput and own disjoint state, so the two undos never entangle. It is
// also distinct from the router's back/forward history (doc 21 T3.2 / doc 18 T6):
// navigation-only view switches (1/2/3/4) and find-cycling drive NAV, not this.
//
// Reversible state is small and serialisable, so a command carries the value
// itself (a filter string, a cone flag pair, a take set) rather than a diff — no
// diffing needed at this scale. The stack is pure data + inline mechanics, so the
// null-backend undo test asserts before/after reversal as model state (D4); the
// APPLY onto ShellState/LoomState lives in ui/shell.cpp (undo_apply), where those
// types are in scope.
#ifndef ASMDESK_UI_UNDO_H
#define ASMDESK_UI_UNDO_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "loom/take_view.h" // loom_take_node_t (the pure take view model)
#include "ui/selection.h"

namespace asmdesk {

// One reversible mutation, before + after. Only the field(s) named by `kind`
// carry meaning; the rest stay default. Fat but cheap at this scale, and it keeps
// undo/redo a plain value assignment with no per-kind bookkeeping.
struct UndoCommand {
    enum class Kind { Cone, Filter, TakeSet, Selection };
    Kind kind = Kind::Cone;

    // Cone (cone_active / cone_fwd on ShellState).
    bool cone_active_before = false, cone_fwd_before = false;
    bool cone_active_after = false, cone_fwd_after = false;

    // Filter — a syscall name-filter string, scoped to the recording it was
    // edited on. `filter_rec` is the recording id (NOT a tab index, which shifts
    // as tabs open/close): undo/redo must restore the filter on THAT recording,
    // never on whichever tab happens to be active when Ctrl+Z is pressed.
    std::string filter_rec;
    std::string filter_before, filter_after;

    // TakeSet — the Loom takes gutter's accumulator (add / remove / clear).
    // `take_rec` is the recording id the edit was made on (LoomState::source_id
    // at push time) — mirrors filter_rec above: LoomState is a SINGLE,
    // non-per-recording state (unlike the per-recording `observers` array), so
    // switching the active recording clears it outright (42) rather than
    // stashing it; a stale undo/redo from a different recording must not
    // resurrect its takes onto whatever recording is showing now.
    std::string take_rec;
    std::vector<loom_take_node_t> takes_before, takes_after;
    // The paintable view alongside each take (42 T4), index-parallel to
    // takes_before/after — moved through undo/redo in lockstep so the overlay
    // never drifts out of alignment with the gutter after a Ctrl+Z/Ctrl+Y.
    std::vector<loom_take_view_t> take_views_before, take_views_after;

    // Selection — the shared brushed entity.
    Selection sel_before, sel_after;
};

// The stack + a cursor. cmds[0 .. cursor-1] are applied; cmds[cursor ..] are the
// redo tail. A new push truncates the redo tail (browser-history discipline).
struct UndoStack {
    std::vector<UndoCommand> cmds;
    std::size_t cursor = 0;

    void push(UndoCommand c) {
        cmds.resize(cursor); // drop any redo tail
        cmds.push_back(std::move(c));
        cursor = cmds.size();
    }
    bool can_undo() const { return cursor > 0; }
    bool can_redo() const { return cursor < cmds.size(); }

    // Pop one toward the past; the caller restores its `*_before`. nullptr at the
    // bottom of the stack.
    const UndoCommand *undo() {
        if (!can_undo())
            return nullptr;
        return &cmds[--cursor];
    }
    // Replay one toward the future; the caller restores its `*_after`. nullptr at
    // the top.
    const UndoCommand *redo() {
        if (!can_redo())
            return nullptr;
        return &cmds[cursor++];
    }
    void clear() {
        cmds.clear();
        cursor = 0;
    }
};

} // namespace asmdesk
#endif // ASMDESK_UI_UNDO_H
