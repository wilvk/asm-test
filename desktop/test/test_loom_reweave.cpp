// test_loom_reweave.cpp — the reweave consumption path
// (docs/internal/gui/42-loom-reweave-consumption.md T2/T3). FULL BUILD ONLY:
// like test_loom_forks.cpp, this binary links Unicorn (the resume seam) —
// nothing in asmtest-viewer links any of it.
//
// T2 (loom_reweave_available) is pure and asserted first, with no engine
// involved at all. T3 (loom_run_reweave) reuses test_loom_forks.cpp's exact
// `poly` fixture (a straight-line routine, step 3 the reweave point) so this
// test cannot silently drift from the one that already validates
// loom_take_run_from_step + loom_take_view directly — it asserts the SAME
// facts through the new engine-leg wrapper, plus the refusal path the wrapper
// adds (an unknown register never returns an empty-but-successful view).
#include <cstdio>
#include <string>
#include <vector>

#include "loom/fabric_plan.h" // loom_view_t + loom_plan/loom_take_plan (paint-gate check)
#include "loom/forks.h" // loom_take_run/loom_edit_t/loom_take_t (the base run)
#include "loom/reweave_apply.h"
#include "loom/reweave_input.h"

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

// poly(a,b,c) — test_loom_forks.cpp's straight-line fixture; step 3 (imul) is
// the reweave point.
//   0 mov rax,rdi  1 add rax,rsi  2 add rax,rdx  3 imul rax,rax
//   4 mov [rsp-8],rax  5 mov rcx,[rsp-8]  6 add rax,rcx  7 ret
static const uint8_t poly[] = {
    0x48, 0x89, 0xf8,       0x48, 0x01, 0xf0, 0x48, 0x01, 0xd0,
    0x48, 0x0f, 0xaf, 0xc0, 0x48, 0x89, 0x44, 0x24, 0xf8,
    0x48, 0x8b, 0x4c, 0x24, 0xf8, 0x48, 0x01, 0xc8, 0xc3,
};
static const long pa[3] = {2, 3, 4};

static loom_fabric_t weave_entry_run() {
    // The base fabric a reweave diverges against: an entry-arg identity fork
    // (arg0 := 2, matching pa[0]). Null session/base_state: the value fabric
    // needs no fault-card leg, per forks.h's documented null tolerance.
    loom_edit_t ident;
    ident.arg_index = 0;
    ident.arg_value = 2;
    loom_take_t parent;
    loom_take_run(nullptr, nullptr, poly, sizeof poly, pa, 3, ident, &parent);
    loom_fabric_t f;
    std::string err;
    if (!loom_fabric_build(parent.vt(), parent.g(), parent.provenance(), &f,
                           &err))
        fail("weave base", err);
    return f;
}

int main() {
    // --- T2: the identity latch, pure, no engine involved -------------------
    {
        check("available: matching sha, in-range nargs",
              loom_reweave_available("abc123", "abc123", 3), "");
        check("unavailable: mismatched sha",
              !loom_reweave_available("abc123", "def456", 3), "");
        check("unavailable: empty streams sha (no code identity)",
              !loom_reweave_available("", "abc123", 3), "");
        check("unavailable: empty author sha (no eligible run)",
              !loom_reweave_available("abc123", "", 3), "");
        check("available: nargs == 0 is a legitimate argless routine",
              loom_reweave_available("abc123", "abc123", 0), "");
        check("unavailable: nargs out of AuthorState's fixed 6-slot range",
              !loom_reweave_available("abc123", "abc123", 7), "");
        check("unavailable: negative nargs",
              !loom_reweave_available("abc123", "abc123", -1), "");
    }

    loom_fabric_t base = weave_entry_run();
    check("base fabric wove", base.steps > 0, "");

    ReweaveSource src;
    src.code = poly;
    src.code_len = sizeof poly;
    src.args = pa;
    src.nargs = 3;
    src.code_sha = "irrelevant-here"; // loom_run_reweave doesn't check identity;
                                      // that's the caller's job (T2), already
                                      // asserted above.

    // --- T3: a clean reweave, re-checking the same facts test_loom_forks
    // pins for the raw engine call, now through the wrapper ------------------
    {
        loom_take_view_t v = loom_run_reweave(base, src, /*req_step=*/3,
                                              /*req_reg=*/"rax",
                                              /*req_value=*/0);
        check("clean reweave carries no loud error", v.node.err.empty(),
              v.node.err);
        check("straight-line reweave is aligned end-to-end",
              !v.div.diverged &&
                  v.node.alignment == std::string(kLoomAlignedEndToEnd),
              loom_take_dump(v));
        check("the reweave carries the crossing-the-line disclosure",
              v.node.disclosure == std::string(kLoomForkDisclosure),
              v.node.disclosure);
        check("the cone seeds at the reweave step (K=3)",
              !v.cone.empty() && v.cone.front() == 3, loom_take_dump(v));
        bool any_hot = false;
        for (const loom_take_step_t &x : v.steps)
            if (x.in_cone && x.verdict == take_verdict::hot)
                any_hot = true;
        check("the reweave's cone carries a real value divergence (HOT)",
              any_hot, loom_take_dump(v));
        check("the take's one-fact edit is the edit's own describe() text",
              v.node.edit.find("rax") != std::string::npos &&
                  v.node.edit.find("step3") != std::string::npos,
              v.node.edit);
        check("a real take has a real fabric (paints)", v.node.has_fabric, "");
    }

    // --- T3 refusal path: an unknown register never returns an empty-but-
    // successful view — the loud engine err survives verbatim (D7). Also
    // closes the review's Bug D: a refusal must not compute a fabricated
    // "diverged at step 0" alignment, and the paint layer's has_fabric gate
    // must actually produce ZERO prims for it (not just carry the flag) -----
    {
        loom_take_view_t v =
            loom_run_reweave(base, src, /*req_step=*/3,
                             /*req_reg=*/"not_a_register", /*req_value=*/0);
        check("an unknown register refuses loudly", !v.node.err.empty(),
              "an invalid edit must not silently produce a fabric");
        check("a hard refusal has no fabric to paint", !v.node.has_fabric,
              "an unassembled/unrun take must never look paintable");
        check("the refusal carries no invented cone", v.cone.empty(),
              "a refused reweave must never paint fabricated content");
        check(
            "the refusal carries no fabricated alignment claim",
            v.node.alignment.empty(),
            "a take with no real fabric must not claim a patient-zero step: " +
                v.node.alignment);
        check("the refusal still carries the crossing-the-line disclosure",
              v.node.disclosure == std::string(kLoomForkDisclosure),
              v.node.disclosure);
        // The actual paint-layer gate (fabric_imgui.cpp mirrors this exact
        // check): a caller that skips painting on !has_fabric gets zero prims,
        // never the whole-canvas dashed band + patient-zero line Bug D shipped.
        if (v.node.has_fabric) {
            std::vector<loom_prim_t> prims;
            loom_view_t cam;
            cam.px_w = 400;
            cam.steps_per_px = 1.0;
            loom_take_plan(v, cam, &prims);
            check("has_fabric gate: a refusal must produce zero prims when "
                  "the gate is honoured",
                  prims.empty(), "got " + std::to_string(prims.size()));
        }
    }

    // --- T3 refusal path: K past the run's length ---------------------------
    {
        loom_take_view_t v = loom_run_reweave(base, src, /*req_step=*/9999,
                                              /*req_reg=*/"rax",
                                              /*req_value=*/0);
        check("K past the run's length refuses loudly", !v.node.err.empty(),
              "a checkpoint past the run must refuse, not fabricate a tail");
        check("K past the run's length has no fabric to paint",
              !v.node.has_fabric, "");
        check("K past the run's length carries no fabricated alignment claim",
              v.node.alignment.empty(), v.node.alignment);
    }

    // --- T3 success-with-caveat: a reweave that faults mid-tail is still a
    // valid counterfactual (loom_take_run_from_step returns true), and its
    // caveat must reach node.err — not be silently dropped on the success
    // path (the review's Bug C). Reweaving rsp to a non-canonical address
    // right before poly's step-4 stack write reliably faults the guest
    // (empirically confirmed: rct==1 on this seam every run). --------------
    {
        loom_take_view_t v = loom_run_reweave(
            base, src, /*req_step=*/3, /*req_reg=*/"rsp",
            /*req_value=*/0x8000000000000000ULL);
        check("a reweave that faults after the edit is not a refusal",
              !v.node.err.empty(),
              "loom_take_run_from_step returns true for a post-edit fault; "
              "the caveat must not be silently dropped");
        check("a faulted reweave still has a real fabric (it DID run)",
              v.node.has_fabric,
              "a caveat alongside a real, if partial, run must still paint — "
              "it is not a hard refusal");
        check("the caveat names the fault, verbatim from the engine",
              v.node.err.find("faulted") != std::string::npos, v.node.err);
    }

    if (failures) {
        std::fprintf(stderr, "%d loom-reweave check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_reweave: all checks passed\n");
    return 0;
}
