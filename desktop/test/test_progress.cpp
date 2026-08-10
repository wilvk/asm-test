// test_progress.cpp — the uniform busy signal + the 3D-scrub degrade decision
// (23-graded-truth-layer.md T4). Pure model, no GL, no ImGui context (D4).
//
// The fidelity rule (14 T3) is preserved: an op with no real total gets the
// indeterminate spinner, NEVER a fabricated percentage. The generalisation adds
// an elapsed clock + a cancel flag (observable), and a should_degrade() predicate
// so the 3D scrub degrades to the labelled coarse plane instead of stalling.
#include <cstdio>
#include <string>

#include "ui/progress.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

int main() {
    // --- the no-fabricated-total fidelity rule still holds ---------------------
    check("mode/idle-hidden",
          progress_mode(false, true, 100) == ProgressMode::Hidden,
          "nothing in flight -> no bar");
    check("mode/unbounded-indeterminate",
          progress_mode(true, false, 0) == ProgressMode::Indeterminate,
          "no real total -> the indeterminate spinner, never a fake %");
    check("mode/footered-determinate",
          progress_mode(true, true, 200) == ProgressMode::Determinate,
          "a genuine total gives a real fraction");
    check("mode/zero-total-not-determinate",
          progress_mode(true, true, 0) == ProgressMode::Indeterminate,
          "has_total but total==0 cannot form a fraction");

    // --- LongOp: fidelity carried through + elapsed advances + cancel observable
    {
        LongOp op;
        check("op/hidden-when-inactive", op.mode() == ProgressMode::Hidden,
              "a fresh op is not in flight");
        op.begin(10.0); // start at t=10s
        check("op/active-indeterminate",
              op.mode() == ProgressMode::Indeterminate,
              "an unbounded op gets the indeterminate bar");
        check("op/elapsed-zero-at-start", op.elapsed() == 0.0,
              "elapsed is zero at the moment it begins");
        op.now = 12.5;
        check("op/elapsed-advances", op.elapsed() == 2.5,
              "elapsed advances with the injected clock");
        // A stale/rewound clock never reports negative elapsed.
        op.now = 9.0;
        check("op/elapsed-nonneg", op.elapsed() == 0.0,
              "a rewound clock clamps elapsed at zero");
        // Cancel is observable, and begin() re-arms it.
        check("op/cancel-clear", !op.cancel_requested, "starts un-cancelled");
        op.request_cancel();
        check("op/cancel-set", op.cancel_requested, "a cancel is observable");
        op.begin(20.0);
        check("op/cancel-rearmed", !op.cancel_requested,
              "begin() clears a prior cancel");

        // A bounded op gets a real fraction, clamped.
        LongOp d;
        d.active = true;
        d.has_total = true;
        d.total = 200;
        d.done = 50;
        check("op/determinate", d.mode() == ProgressMode::Determinate, "");
        check("op/fraction", d.fraction() == 0.25f, "50/200");
        d.done = 500;
        check("op/fraction-clamped", d.fraction() == 1.0f,
              "over-100% is clamped, never shown");
    }

    // --- the 3D-scrub degrade decision + its coarse label ---------------------
    check("degrade/over-budget", should_degrade(500000, 200000),
          "an over-budget cell count degrades to coarse");
    check("degrade/under-budget", !should_degrade(1000, 200000),
          "a golden-sized terrain never degrades — the full slice lands");
    check("degrade/at-budget", !should_degrade(200000, 200000),
          "exactly at budget is not over budget");
    // --- playback holds the coarse plane (animation cost bound) --------------
    // The transport already keeps the RATE wall-clock steady (transport_tick);
    // this keeps the per-frame COST bounded: an over-budget slice never lands
    // mid-play, so the frame rate holds too. Paused behavior is unchanged:
    // coarse for the in-flight frame, full one frame later.
    check("scrub/paused first frame degrades", scrub_hold_coarse(false, false),
          "an over-budget scrub still shows coarse for the in-flight frame");
    check("scrub/paused second frame lands full",
          !scrub_hold_coarse(false, true),
          "the full slice must land one frame later when not playing");
    check("scrub/playing always holds coarse",
          scrub_hold_coarse(true, false) && scrub_hold_coarse(true, true),
          "playback must never pay the full re-slice per animated frame");

    check("degrade/zero-budget-never", !should_degrade(999999999, 0),
          "a zero (unmeasured) budget means never degrade");
    // The degraded frame carries a coarse provenance label — not measured
    // emptiness (D7 / terrain.h:22).
    {
        std::string note = scrub_degrade_note();
        check("degrade/note-nonempty", !note.empty(),
              "the degraded frame must carry a label");
        check("degrade/note-says-coarse",
              note.find("coarse") != std::string::npos,
              "the label must name the coarse rung");
        check("degrade/note-not-empty-claim",
              note.find("not measured emptiness") != std::string::npos,
              "the label must deny it is measured emptiness");
    }

    if (failures) {
        std::fprintf(stderr, "test_progress: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_progress: all checks passed\n");
    return 0;
}
