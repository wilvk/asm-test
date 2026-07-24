// test_walkthrough.cpp — the Learn door's player model
// (docs/internal/gui/06-doors-and-learning.md T4). Headless: no display, no GL,
// no engines — it reads the committed walkthrough recordings and asserts on the
// decode.
#include <cstdio>
#include <string>
#include <vector>

#include "walkthrough.h"

#ifndef ASMTEST_WALKTHROUGH_DIR
#error "ASMTEST_WALKTHROUGH_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static wt_model load(const std::string &name) {
    std::string path = std::string(ASMTEST_WALKTHROUGH_DIR) + "/" + name;
    std::string err;
    auto rec = load_recording_file(path, err);
    if (!rec) {
        fail("load " + name, err);
        return wt_model{};
    }
    return wt_build(*rec);
}

int main() {
    // --- square: the quickstart ladder's first rung -----------------------
    {
        wt_model m = load("square.asmtrace");
        std::string d = wt_dump(m);
        check("square has 4 stops", m.stops.size() == 4, d);
        check("square recorded 3 steps", m.steps_recorded == 3, d);
        check("square is not truncated", !m.truncated, d);

        // Ordinals are FILE order, contiguous from 1 — the schema's contract.
        for (size_t i = 0; i < m.stops.size(); i++)
            check("square ordinal " + std::to_string(i),
                  m.stops[i].ordinal == static_cast<int>(i) + 1, d);

        check("stop 1 is anchored at step 0", m.stops[0].step_anchor == 0, d);
        check("stop 1 carries its title",
              m.stops[0].title == "the ABI is the contract", m.stops[0].title);
        check("stop 1 carries its body", !m.stops[0].body.empty(), d);
        check("stop 1 has no failure framing", !m.stops[0].has_framing(), d);

        // The quickstart §5 failure framing.
        const wt_stop &last = m.stops[3];
        check("the last stop is unanchored", last.step_anchor == -1, d);
        check("it frames expected 5", last.expected == "5", last.expected);
        check("...and got 4", last.got == "4", last.got);
        check("...and reads as a framing", last.has_framing(), d);

        // Every anchor resolves inside the recorded window.
        for (const wt_stop &s : m.stops)
            check("square anchor in window", m.anchor_in_window(s.step_anchor),
                  d);

        // Navigation.
        check("player starts at stop 1", m.cur == 0 && m.current() != nullptr,
              "");
        check("next moves", m.next() && m.cur == 1, "");
        check("prev moves back", m.prev() && m.cur == 0, "");
        check("prev at the start refuses", !m.prev(), "");
        while (m.next())
            ;
        check("next at the end refuses", !m.next() && m.cur == 3, "");
    }

    // --- demo-fail: the failure that IS the navigation spine --------------
    {
        wt_model m = load("demo-fail.asmtrace");
        std::string d = wt_dump(m);
        check("demo-fail has 4 stops", m.stops.size() == 4, d);
        check("demo-fail recorded 4 steps", m.steps_recorded == 4, d);
        const wt_stop &s = m.stops[2];
        check("the bug's stop is anchored at step 2", s.step_anchor == 2, d);
        check("it says what was expected", s.expected == "rax=4", s.expected);
        check("...and what happened", s.got == "rax=3", s.got);
        check("its title names the inverted predicate",
              s.title == "the predicate is inverted", s.title);
    }

    // --- ct_eq: the ladder's capstone (T3) --------------------------------
    {
        wt_model m = load("ct_eq.asmtrace");
        std::string d = wt_dump(m);
        check("ct_eq has 5 stops", m.stops.size() == 5, d);
        check("ct_eq recorded a real run", m.steps_recorded > 100, d);
        // The coverage verdict carries MEASURED numbers, not prose: the
        // generator read them off the trace it just recorded.
        const wt_stop &verdict = m.stops[3];
        check("the CT verdict frames expected vs got", verdict.has_framing(),
              d);
        check("expected and got AGREE for ct_eq — the union did not grow",
              verdict.expected == verdict.got,
              verdict.expected + " vs " + verdict.got);
        check("the verdict names a block count",
              verdict.got.find(" blocks") != std::string::npos, verdict.got);
        // The control's verdict must DISAGREE, or the oracle proves nothing.
        const wt_stop &control = m.stops[4];
        check("the control's verdict frames expected vs got",
              control.has_framing(), d);
        check("expected and got DIFFER for the leaky control",
              control.expected != control.got,
              control.expected + " vs " + control.got);
        check("the control reports the added block, at the early exit",
              control.got.find("(+1 at the early exit)") != std::string::npos,
              control.got);
    }

    // --- the D7 dishonesty fixture ----------------------------------------
    {
        wt_model m = load("square-truncated.asmtrace");
        std::string d = wt_dump(m);
        check("the truncated fixture IS truncated", m.truncated, d);
        check("it recorded fewer steps than square", m.steps_recorded == 2, d);
        check("it has the same 4 stops", m.stops.size() == 4, d);

        // Stops 1 and 2 are inside the window; stop 3 (step 2) is NOT.
        check("stop 1 (step 0) is in window", m.anchor_in_window(0), d);
        check("stop 2 (step 1) is in window", m.anchor_in_window(1), d);
        check("stop 3 (step 2) is BEYOND the window", !m.anchor_in_window(2),
              "the player would have clamped to a step it did not record");
        check("the unanchored stop stays playable", m.anchor_in_window(-1), d);
        check("the dump marks the out-of-window stop",
              d.find("BEYOND_WINDOW") != std::string::npos, d);

        // The refusal copy is the interface; a re-word is a change to a claim.
        check("the refusal copy is verbatim",
              std::string(kWtBeyondWindow) ==
                  "stop is beyond the recorded window — this recording is "
                  "truncated, so the step this stop is about was never "
                  "recorded",
              kWtBeyondWindow);
    }

    // --- a recording that is NOT a walkthrough ----------------------------
    // It loads, and yields no stops. The door says "no stops" rather than
    // synthesising them from the trace, which would be a story nobody wrote.
    {
        std::string path =
            std::string(ASMTEST_WALKTHROUGH_DIR) + "/../add_signed.asmtrace";
        std::string err;
        auto rec = load_recording_file(path, err);
        if (!rec) {
            fail("load add_signed", err);
        } else {
            wt_model m = wt_build(*rec);
            check("a plain recording yields no stops", m.stops.empty(),
                  wt_dump(m));
            check("...but still has its note as a title", !m.title.empty(),
                  wt_dump(m));
            check("...and no current stop", m.current() == nullptr, "");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d walkthrough check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_walkthrough: all checks passed\n");
    return 0;
}
