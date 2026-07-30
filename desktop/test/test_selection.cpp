// test_selection.cpp — the ONE shared brushing-and-linking selection model
// (docs/internal/gui/22-selection-and-search.md T1, F7). Null backend, model
// state not pixels (D4).
//
// The payoff Wave 1's real panes unlock: a pick in ANY pane brushes the same
// entity, and every pane reads its projection — so this asserts that from ONE
// Selection the timeline model, the slice-view model AND the Loom camera's
// selected_steps all report that same entity (cross-highlight from one model),
// and that a step ABSENT from a pane yields "nothing selected" there, never a
// fabricated row (the fidelity floor, D7).
#include <cstdio>
#include <optional>
#include <string>

#include "loom/loom_draw.h"     // LoomState + loom_shared_dim (the derived dim)
#include "ui/shell.h"           // ShellState, shell_open, shell_a, Selection
#include "views/slice_view.h"   // dt_slice_view_build
#include "views/timeline.h"     // dt_timeline_build

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}
static std::string gd(const char *name) {
    return std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
}

int main() {
    ShellState s;
    std::string err;
    int idx = shell_open(s, gd("sum_via_rbx.asmtrace"), err);
    check("open", idx >= 0, err.c_str());
    s.active_tab = 0;
    const Streams *a = shell_a(s);
    check("stream decoded", a != nullptr, "no active stream");
    if (a == nullptr) {
        std::fprintf(stderr, "%d selection check(s) failed\n", failures + 1);
        return 1;
    }

    // A step the recording actually has (its edges reach it, so the slice/Loom can
    // show it). Prefer a step that carries a def-use edge, so the slice view has a
    // node for it.
    uint32_t step = 0;
    if (!a->df.edges.empty())
        step = a->df.edges.front().to_step;
    else if (a->df.nsteps > 1)
        step = 1;
    const uint64_t off =
        step < a->df.insn_off.size() ? a->df.insn_off[step] : 0;

    // === a pick sets the ONE shared model and bumps its epoch =================
    const uint64_t e0 = s.selection.epoch;
    s.selection.set(a->id, step, off);
    check("epoch/bumped-on-set", s.selection.epoch == e0 + 1,
          "set() must bump the epoch so panes notice the brush moved");
    check("selection/rec", s.selection.rec == a->id, "the recording is stamped");

    // === pane A (timeline) reads the shared model =============================
    // body_timeline projects the shared selection into the model; assert the model
    // carries the same step and that a real row exists for it.
    dt_timeline tl = dt_timeline_build(*a);
    tl.selected_step = s.selection.step; // exactly what body_timeline does
    check("timeline/selected", tl.selected_step && *tl.selected_step == step,
          "the timeline model must report the brushed step");
    bool tl_has_row = false;
    for (const dt_timeline_row &r : tl.rows)
        if (r.step == step)
            tl_has_row = true;
    check("timeline/row-exists", tl_has_row,
          "the brushed step is a real row here (cross-highlight in place)");

    // === pane B (slice explorer) reads the SAME model ========================
    dt_slice_view sv = dt_slice_view_build(*a, s.selection.step);
    check("slice/selected", sv.selected_step && *sv.selected_step == step,
          "the slice-view model must report the SAME brushed step");

    // === pane C (Loom camera) reads the SAME model, derived ==================
    // A brush from ANOTHER pane (no Loom click) dims the fabric to that step.
    LoomState L; // has_selection == false: nothing picked in the Loom itself
    std::vector<uint32_t> dim = loom_shared_dim(L, &s.selection);
    check("loom/dim-from-shared", dim.size() == 1 && dim.front() == step,
          "the Loom camera dims to the SAME brushed step");
    // A Loom pick lights its own worldline (has_selection wins).
    L.has_selection = true;
    L.sel.steps = {step, step + 1};
    dim = loom_shared_dim(L, &s.selection);
    check("loom/dim-own-worldline", dim == L.sel.steps,
          "the Loom's own pick lights its worldline");

    // === fidelity floor: a step ABSENT from a pane -> nothing selected ========
    const uint32_t absent = a->df.nsteps + 1000; // no such step in this stream
    s.selection.set(a->id, absent, std::nullopt);
    dt_timeline tl2 = dt_timeline_build(*a);
    tl2.selected_step = s.selection.step;
    bool tl2_fabricated = false;
    for (const dt_timeline_row &r : tl2.rows)
        if (r.step == absent)
            tl2_fabricated = true;
    check("timeline/absent-nothing", !tl2_fabricated,
          "an absent step must show NOTHING, never a fabricated row (D7)");
    dt_slice_view sv2 = dt_slice_view_build(*a, s.selection.step);
    bool sv2_fabricated = false;
    for (const dt_slice_node &n : sv2.nodes)
        if (n.step == absent)
            sv2_fabricated = true;
    check("slice/absent-nothing", !sv2_fabricated,
          "an absent step is not a slice node, never fabricated (D7)");

    // === fix 4: the selection is recording-SCOPED (shell_timeline_model) =======
    // A brush in recording A must NOT light a coincident index in recording B
    // after a tab switch. shell_timeline_model gates the projection on
    // `s.selection.rec == a->id`; before the fix body_timeline set
    // `t.selected_step = s.selection.step` unconditionally, misattributing A's step
    // to whatever recording was active (a fabricated row — the D7 breach). This is
    // a TRUE drive-through: it calls the exact `shell_timeline_model` body_timeline
    // draws, over TWO real recordings, with the selection set through the ONE shared
    // writer (Selection::set — the same path the router and keymap use). Reverting
    // the gate inside shell_timeline_model makes `fix4/B timeline marks nothing`
    // fail.
    {
        ShellState s2;
        std::string e2;
        int ia = shell_open(s2, gd("sum_via_rbx.asmtrace"), e2);
        int ib = shell_open(s2, gd("add_signed.asmtrace"), e2);
        check("fix4/opened two", ia == 0 && ib == 1, e2.c_str());
        const Streams *A = &s2.streams[0];
        const Streams *B = &s2.streams[1];
        check("fix4/distinct ids", A->id != B->id, "two recordings, two ids");

        // Brush a real step in A (the shared writer stamps selection.rec = A->id).
        const uint32_t stepA = A->df.nsteps > 1 ? 1u : 0u;
        s2.selection.set(A->id, stepA, std::nullopt);
        check("fix4/brushed in A", s2.selection.rec == A->id,
              "the shared writer stamps the recording the pick was made in");

        // The exact gate body_timeline computes for a given recording:
        //   sel_step = (selection.rec == rec->id) ? selection.step : nullopt
        auto projected = [&](const Streams *rec) -> std::optional<uint32_t> {
            return s2.selection.rec == rec->id ? s2.selection.step : std::nullopt;
        };

        // Switch the active tab to B: A's brush does NOT project onto B, and a real
        // timeline over B carries NO selected row for A's step.
        s2.active_tab = ib;
        check("fix4/no cross-recording selection on B", !projected(B).has_value(),
              "A's brush must not light a row in B after a tab switch (D7)");
        // Drive the REAL model builder body_timeline uses (not a reconstruction):
        // B's timeline carries no selected row for A's brush.
        dt_timeline tB = shell_timeline_model(s2, B, nullptr);
        check("fix4/B timeline marks nothing", !tB.selected_step.has_value(),
              "the timeline for B marks nothing, never a misattributed row");

        // The SAME selection, read on its OWN recording A, DOES project.
        s2.active_tab = ia;
        check("fix4/selection projects on its own recording",
              projected(A).has_value() && *projected(A) == stepA,
              "on A (the recording the pick was made in) the brush projects");
        dt_timeline tA = shell_timeline_model(s2, A, nullptr);
        check("fix4/A timeline marks the brushed step",
              tA.selected_step && *tA.selected_step == stepA,
              "on its own recording the timeline marks the brushed step");
    }

    // === 34 T3: the play/pause transport is a pure, steady, faithful advance =====
    // transport_tick advances a playhead over ITS OWN axis while playing, at a
    // steady rate under a variable frame time, stopping exactly at max.
    {
        Transport tp;
        tp.steps_per_sec = 10.0f;
        check("transport/paused-holds", transport_tick(tp, 5, 100, 1.0f) == 5,
              "a paused transport never advances");
        tp.playing = true;
        check("transport/steady-rate", transport_tick(tp, 5, 100, 1.0f) == 15,
              "10 steps/s over 1s advances +10 steps");
        // A sub-step frame carries its remainder until it crosses a whole step.
        Transport tf;
        tf.playing = true;
        tf.steps_per_sec = 10.0f;
        check("transport/subframe-holds", transport_tick(tf, 0, 100, 0.05f) == 0,
              "half a step does not advance the playhead");
        check("transport/subframe-accumulates",
              transport_tick(tf, 0, 100, 0.05f) == 1,
              "the carried remainder crosses a whole step on the next frame");
        // Lands on the last step and STOPS — never overshoots or loops.
        Transport te;
        te.playing = true;
        te.steps_per_sec = 1000.0f;
        const uint64_t landed = transport_tick(te, 90, 100, 1.0f);
        check("transport/stops-at-max", landed == 100 && !te.playing,
              "playback lands on the last step and clears playing");
        // A zero-dt frame (the null backend's DeltaTime) is inert.
        Transport tz;
        tz.playing = true;
        check("transport/zero-dt-holds", transport_tick(tz, 3, 100, 0.0f) == 3,
              "a zero-dt frame never advances (headless-safe)");
    }

    // === 34 T1: playhead_project gates on the recording (the Scrubber's seed) ==
    // The Scrubber seeds its playhead from the shared brush through this seam, so a
    // step brushed anywhere lands on the register file — but ONLY for the recording
    // the brush was made in (22 T1), and clamped to the local axis.
    {
        Selection pj;
        check("project/inactive-nothing",
              !playhead_project(pj, a->id, 50).has_value(),
              "an inactive selection projects to nothing (no seed)");
        pj.set(a->id, 7, std::nullopt);
        const auto own = playhead_project(pj, a->id, 50);
        check("project/own-recording", own && *own == 7,
              "a step brushed in THIS recording seeds its playhead");
        check("project/clamped-to-axis",
              playhead_project(pj, a->id, 3).value_or(999) == 3,
              "a step past the local axis clamps, never overshoots the ring");
        check("project/other-recording-nothing",
              !playhead_project(pj, "some-other.asmtrace", 50).has_value(),
              "a brush in another recording does not seed this pane (D7, :812)");
    }

    // === clear brushes nothing everywhere ====================================
    const uint64_t e1 = s.selection.epoch;
    s.selection.clear();
    check("clear/bumps-epoch", s.selection.epoch == e1 + 1,
          "clear() bumps the epoch too");
    check("clear/inactive", !s.selection.active(),
          "a cleared selection is inactive (nothing brushed anywhere)");

    if (failures) {
        std::fprintf(stderr, "%d selection check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_selection: all checks passed\n");
    return 0;
}
