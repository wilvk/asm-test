// test_loom_gutter.cpp — the Loom takes gutter accumulator + its reversible
// remove / clear (docs/internal/gui/22-selection-and-search.md T4, F12). Null
// backend, model state (D4).
//
// F12 said "the takes gutter accumulates forks with no remove/clear", but
// LoomState held NO take set — the multi-take gutter was doc 05's design, never
// wired. So T4 builds the accumulator (LoomState::takes) together with its
// per-take remove and clear-forks, both reversible undo Commands, and each take
// keeps its loud refusal verbatim so a remove/clear never quietly drops a
// failure (D7). This pins exactly that.
#include <cstdio>
#include <vector>

#include "loom/loom_draw.h" // LoomState::takes (the accumulator)
#include "loom/take_view.h" // loom_takes_{add,remove,clear}
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
    n.fault = "";
    n.disclosure = "a take is an emulator replay, never silicon";
    return n;
}

// The remove/clear the gutter draw performs, factored so the test drives the
// SAME reversible move without an ImGui context: snapshot -> mutate -> push.
static void gutter_remove(LoomState &L, UndoStack &u, size_t idx) {
    UndoCommand c;
    c.kind = UndoCommand::Kind::TakeSet;
    c.takes_before = L.takes;
    loom_takes_remove(L.takes, idx);
    c.takes_after = L.takes;
    u.push(std::move(c));
}
static void gutter_clear(LoomState &L, UndoStack &u) {
    UndoCommand c;
    c.kind = UndoCommand::Kind::TakeSet;
    c.takes_before = L.takes;
    loom_takes_clear(L.takes);
    c.takes_after = L.takes;
    u.push(std::move(c));
}

int main() {
    LoomState L;
    UndoStack u;
    check("start/empty", L.takes.empty(), "a fresh Loom has no forks");

    // A full build appends each take here; simulate three, one carrying a loud
    // refusal (an unassemblable patch — the loud-drop contract).
    loom_takes_add(L.takes, take("arg0 := 11"));
    loom_takes_add(L.takes, take("code patch", "patch did not assemble"));
    loom_takes_add(L.takes, take("arg1 := 0"));
    check("accumulate", L.takes.size() == 3, "the gutter accumulates takes");

    // === per-take remove drops exactly one, reversibly ======================
    gutter_remove(L, u, 1); // drop the middle (the refusal-bearing take)
    check("remove/one", L.takes.size() == 2 && L.takes[0].label == "arg0 := 11" &&
                            L.takes[1].label == "arg1 := 0",
          "remove drops exactly one node, order preserved");
    {
        const UndoCommand *un = u.undo();
        check("remove/undo", un != nullptr, "the remove is reversible");
        L.takes = un->takes_before;
        check("remove/restored", L.takes.size() == 3,
              "Ctrl+Z restores the removed take");
        // The restored take keeps its loud refusal verbatim (D7).
        check("remove/refusal-verbatim",
              L.takes[1].err == "patch did not assemble",
              "the removed take's loud refusal survives the round trip");
    }

    // === clear forks empties the set, reversibly ============================
    gutter_clear(L, u); // NOTE: this truncates the remove's redo tail (fresh push)
    check("clear/empties", L.takes.empty(), "clear forks empties the whole set");
    {
        const UndoCommand *un = u.undo();
        check("clear/undo", un != nullptr, "clear forks is reversible");
        L.takes = un->takes_before;
        check("clear/restored", L.takes.size() == 3,
              "Ctrl+Z restores every cleared fork");
        // Every fork comes back, including the one carrying the refusal — clearing
        // removed the whole node, refusal and all, and restoring brings it back.
        bool refusal_back = false;
        for (const loom_take_node_t &n : L.takes)
            if (n.err == "patch did not assemble")
                refusal_back = true;
        check("clear/refusal-back", refusal_back,
              "clearing never silently dropped the loud refusal");
    }

    if (failures) {
        std::fprintf(stderr, "%d loom-gutter check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_gutter: all checks passed\n");
    return 0;
}
