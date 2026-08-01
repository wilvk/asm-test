// test_undo.cpp — the app-level command / undo stack
// (docs/internal/archive/gui/22-selection-and-search.md T4, F12). Null backend, model
// state (D4).
//
// Asserts the stack mechanics the shell applies (undo_apply, ui/shell.cpp): a
// filter change, a cone change and a Loom take-set change each reverse to the
// exact prior value on undo() and re-apply on redo(); a new push truncates the
// redo tail (browser-history discipline); and the stack is a DISJOINT object from
// the Author editor's own text undo (doc 17 T2) — it names none of it. The live
// io.WantTextInput boundary that keeps the two from stealing each other's keys is
// exercised by the interaction lane (test_ui).
#include <cstdio>
#include <string>
#include <vector>

#include "loom/take_view.h" // loom_take_node_t + loom_takes_{add,remove,clear}
#include "ui/undo.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

static loom_take_node_t take(const char *label, const char *err = "") {
    loom_take_node_t n;
    n.label = label;
    n.err = err;
    n.disclosure = "emulator replay, not silicon";
    return n;
}

int main() {
    // === a Filter change reverses to the exact prior value ===================
    {
        UndoStack u;
        std::string filter = ""; // the live value the stack mirrors
        UndoCommand c;
        c.kind = UndoCommand::Kind::Filter;
        c.filter_before = filter;    // ""
        filter = "openat";           // the user typed it
        c.filter_after = filter;     // "openat"
        u.push(std::move(c));
        check("filter/can-undo", u.can_undo() && !u.can_redo(), "one applied");
        const UndoCommand *un = u.undo();
        check("filter/undo", un && un->filter_before == "",
              "Ctrl+Z restores the empty filter");
        filter = un->filter_before;
        check("filter/reverted", filter.empty(), "the filter reverted");
        const UndoCommand *re = u.redo();
        check("filter/redo", re && re->filter_after == "openat",
              "Ctrl+Y re-applies the filter");
        filter = re->filter_after;
        check("filter/reapplied", filter == "openat", "the filter is back");
    }

    // === a Cone change reverses ==============================================
    {
        UndoStack u;
        bool active = false, fwd = false;
        UndoCommand c;
        c.kind = UndoCommand::Kind::Cone;
        c.cone_active_before = active;
        c.cone_fwd_before = fwd;
        active = true;
        fwd = true; // `f` lit the forward cone
        c.cone_active_after = active;
        c.cone_fwd_after = fwd;
        u.push(std::move(c));
        const UndoCommand *un = u.undo();
        check("cone/undo", un && !un->cone_active_before && !un->cone_fwd_before,
              "Ctrl+Z restores the unlit cone");
        const UndoCommand *re = u.redo();
        check("cone/redo", re && re->cone_active_after && re->cone_fwd_after,
              "Ctrl+Y re-lights the forward cone");
    }

    // === a Take-set change reverses (add / remove / clear are reversible) =====
    {
        UndoStack u;
        std::vector<loom_take_node_t> takes = {take("take-0")};
        UndoCommand add;
        add.kind = UndoCommand::Kind::TakeSet;
        add.takes_before = takes;                     // {take-0}
        loom_takes_add(takes, take("take-1", "unassemblable patch")); // loud err
        add.takes_after = takes;                      // {take-0, take-1}
        u.push(std::move(add));
        const UndoCommand *un = u.undo();
        check("take/undo", un && un->takes_before.size() == 1,
              "Ctrl+Z restores the single-take set");
        takes = un->takes_before;
        check("take/reverted", takes.size() == 1 && takes[0].label == "take-0",
              "the removed take is back");
        const UndoCommand *re = u.redo();
        check("take/redo", re && re->takes_after.size() == 2,
              "Ctrl+Y re-adds the take");
        // The re-added take keeps its loud refusal verbatim (D7): clearing/removing
        // never quietly drops a take's failure.
        check("take/refusal-verbatim",
              re->takes_after[1].err == "unassemblable patch",
              "a take's loud refusal survives add/undo/redo intact");
    }

    // === per-take remove drops exactly one; clear empties the set ============
    {
        std::vector<loom_take_node_t> takes = {take("a"), take("b"), take("c")};
        loom_takes_remove(takes, 1); // drop the middle
        check("remove/one", takes.size() == 2 && takes[0].label == "a" &&
                                takes[1].label == "c",
              "remove drops exactly one node, order preserved");
        loom_takes_remove(takes, 99); // out of range -> no-op
        check("remove/oob-noop", takes.size() == 2, "an out-of-range remove is a no-op");
        loom_takes_clear(takes);
        check("clear/empties", takes.empty(), "clear forks empties the whole set");
    }

    // === a new push truncates the redo tail (browser-history discipline) =====
    {
        UndoStack u;
        UndoCommand a, b, c;
        a.kind = b.kind = c.kind = UndoCommand::Kind::Cone;
        u.push(a);
        u.push(b);
        u.undo(); // cursor now at 1; b is a redo candidate
        check("truncate/has-redo", u.can_redo(), "b is redoable before the push");
        u.push(c); // this must DROP b's redo tail
        check("truncate/no-redo", !u.can_redo(),
              "a fresh push truncates the redo tail");
        check("truncate/size", u.cmds.size() == 2,
              "the stack holds a, c — b was truncated");
    }

    if (failures) {
        std::fprintf(stderr, "%d undo check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_undo: all checks passed\n");
    return 0;
}
