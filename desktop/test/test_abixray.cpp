// test_abixray.cpp — the ABI x-ray's pure builder (09-teaching-producers.md T4).
// No ImGui, no engine: it links abixray.o + scrubber.o + stepindex.o +
// walkthrough.o + the doc model and NOTHING else, the same proof test_scrubber
// makes that a render-only viewer locks two register scrubbers with zero engine
// deps (D4).
//
// It covers the brief's "Done when": the paired recording opens as a LOCKED
// two-pane x-ray (one playhead seeks both), every stop lands on its step, and
// the walkthrough narrates SysV-vs-Win64 differences that the CROSS-PANE
// register deltas visibly show — plus the two fidelity refusals (unaligned panes,
// an absent producer).
#include "view_test.h"

#include "analysis/stepindex.h"
#include "views/abixray.h"
#include "walkthrough.h"

using namespace asmdesk;

namespace {

Recording load_rec(const std::string &name) {
    std::string path = std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
    std::string err;
    auto r = load_recording_file(path, err);
    if (!r) {
        vt::fail("load " + name, err);
        return Recording{};
    }
    return *r;
}

const dt_abixray_row *row(const dt_abixray &x, const std::string &n) {
    for (const dt_abixray_row &r : x.rows)
        if (r.name == n)
            return &r;
    return nullptr;
}

// The whole walkthrough-driven animation, as text: the x-ray dumped at every
// authored stop in order, so one golden pins stop navigation, the locked
// playhead AND the cross-pane deltas in one artefact.
std::string walk_dump(const StepIndex &s, const StepIndex &w, wt_model walk) {
    std::string o;
    int n = static_cast<int>(walk.stops.size());
    for (int i = 0; i < n; i++) {
        walk.cur = i;
        o += dt_abixray_dump(dt_abixray_build(s, w, walk));
    }
    return o;
}

} // namespace

int main() {
    // --- make_pair: the struct-return eightbyte contrast ---------------------
    {
        Recording sr = load_rec("abixray-make_pair-sysv.asmtrace");
        Recording wr = load_rec("abixray-make_pair-win64.asmtrace");
        StepIndex sysv = build_step_index(sr);
        StepIndex win64 = build_step_index(wr);
        wt_model walk = wt_build(sr);

        vt::check("both panes carry a regstate producer",
                  sysv.present() && win64.present(), "the pair must be armed");
        vt::eq("make_pair sysv steps", sysv.total_steps(), uint64_t{3});
        vt::eq("make_pair win64 steps", win64.total_steps(), uint64_t{3});
        vt::eq("the reference leg carries the walkthrough", walk.stops.size(),
               size_t{4});
        vt::check("the rail title is the SysV leg's intro",
                  walk.title.find("marshalled two ways") != std::string::npos,
                  "title: " + walk.title);

        // Stop 0 anchors step 0: the panes lock together on the entry state.
        walk.cur = 0;
        dt_abixray x0 = dt_abixray_build(sysv, win64, walk);
        vt::check("the pair opens present + aligned", x0.present && x0.aligned,
                  "a step-aligned pair");
        vt::eq("x-ray total steps", x0.total, uint64_t{3});
        vt::eq("stop 0 lands the playhead on step 0", x0.playhead, uint64_t{0});
        vt::eq("stop 0 anchor", x0.stop_anchor, long{0});
        // LOCKED: one playhead, both decks seeked to it.
        vt::check("both panes are locked to the one playhead",
                  x0.sysv.playhead == x0.playhead &&
                      x0.win64.playhead == x0.playhead,
                  "the panes must share the playhead");
        vt::check("stop 0 is in the recorded window (not refused)",
                  !x0.stop_beyond_window, "step 0 was recorded");

        // The a0 -> rdi (SysV) vs rcx (Win64) contrast, mechanical.
        const dt_abixray_row *rdi = row(x0, "rdi");
        const dt_abixray_row *rcx = row(x0, "rcx");
        vt::check("a0 is in rdi on SysV, absent on Win64",
                  rdi && rdi->sysv == 7 && rdi->win64 == 0 && rdi->differs,
                  "rdi must differ 7 vs 0");
        vt::check("a0 is in rcx on Win64, absent on SysV",
                  rcx && rcx->sysv == 0 && rcx->win64 == 7 && rcx->differs,
                  "rcx must differ 0 vs 7");
        vt::check("the narration names the register contrast",
                  x0.stop_body.find("rdi") != std::string::npos &&
                      x0.stop_body.find("rcx") != std::string::npos,
                  "body: " + x0.stop_body);

        // Stop 1 anchors step 1: the SAME instruction, a different input. SysV's
        // rax takes the argument (a write the deck highlights); Win64's rax stays
        // 0 (rdi was never an argument), so its rax is NOT highlighted.
        walk.cur = 1;
        dt_abixray x1 = dt_abixray_build(sysv, win64, walk);
        vt::eq("stop 1 lands on step 1", x1.playhead, uint64_t{1});
        const dt_abixray_row *rax1 = row(x1, "rax");
        vt::check("SysV rax took the argument and is highlighted",
                  rax1 && rax1->sysv == 7 && rax1->sysv_changed,
                  "rax should be 7 and changed on SysV");
        vt::check("Win64 rax stayed 0 and is not highlighted",
                  rax1 && rax1->win64 == 0 && !rax1->win64_changed,
                  "rax should be 0 and unchanged on Win64");
        vt::check("rax differs across the panes at step 1",
                  rax1 && rax1->differs, "7 vs 0");

        // Stop 2 anchors step 2: SysV returns the struct in the pair rax:rdx.
        walk.cur = 2;
        dt_abixray x2 = dt_abixray_build(sysv, win64, walk);
        vt::eq("stop 2 lands on step 2", x2.playhead, uint64_t{2});
        const dt_abixray_row *rax2 = row(x2, "rax");
        const dt_abixray_row *rdx2 = row(x2, "rdx");
        vt::check("SysV returns (a, b) in rax:rdx",
                  rax2 && rax2->sysv == 7 && rdx2 && rdx2->sysv == 11,
                  "rax:rdx = (7, 11)");
        vt::check("Win64's rax:rdx do not carry the struct",
                  rax2 && rax2->win64 == 0 && rdx2 && rdx2->win64 == 0,
                  "the aggregate went by hidden pointer, not registers");

        // Stop 3 is unanchored (a summary): it must NOT move the panes — the
        // reader keeps their place at the last anchored step (2).
        walk.cur = 3;
        dt_abixray x3 = dt_abixray_build(sysv, win64, walk);
        vt::eq("an unanchored stop keeps the playhead", x3.playhead,
               uint64_t{2});
        vt::eq("the unanchored stop reports no anchor", x3.stop_anchor,
               long{-1});

        vt::golden("abixray-make_pair.txt", walk_dump(sysv, win64, walk));
    }

    // --- sum3: the a0->rdi/rcx contrast + the rsi/rdi callee-saved swap -------
    {
        Recording sr = load_rec("abixray-sum3-sysv.asmtrace");
        Recording wr = load_rec("abixray-sum3-win64.asmtrace");
        StepIndex sysv = build_step_index(sr);
        StepIndex win64 = build_step_index(wr);
        wt_model walk = wt_build(sr);

        vt::eq("sum3 total steps", sysv.total_steps(), uint64_t{4});
        vt::eq("sum3 win64 steps", win64.total_steps(), uint64_t{4});

        walk.cur = 0;
        dt_abixray x0 = dt_abixray_build(sysv, win64, walk);
        vt::check("sum3 opens present + aligned", x0.present && x0.aligned,
                  "aligned 4-step pair");
        const dt_abixray_row *rdi = row(x0, "rdi");
        const dt_abixray_row *rcx = row(x0, "rcx");
        vt::check("sum3 a0 -> rdi (SysV) vs rcx (Win64)",
                  rdi && rdi->sysv == 11 && rcx && rcx->win64 == 11 &&
                      rdi->differs && rcx->differs,
                  "a0 = 11 lands in rdi on the left, rcx on the right");

        // Stop 1 anchors step 1: rsi is SysV's second argument (22) but Win64's
        // callee-saved register (0) — the role reversal, made visible.
        walk.cur = 1;
        dt_abixray x1 = dt_abixray_build(sysv, win64, walk);
        vt::eq("sum3 stop 1 lands on step 1", x1.playhead, uint64_t{1});
        const dt_abixray_row *rsi = row(x1, "rsi");
        vt::check("rsi is an argument on SysV but callee-saved on Win64",
                  rsi && rsi->sysv == 22 && rsi->win64 == 0 && rsi->differs,
                  "rsi must differ 22 vs 0");
        vt::check("the narration names the callee-saved swap",
                  x1.stop_body.find("callee-saved") != std::string::npos ||
                      x1.stop_body.find("CALLEE-SAVED") != std::string::npos,
                  "body: " + x1.stop_body);

        vt::golden("abixray-sum3.txt", walk_dump(sysv, win64, walk));
    }

    // --- fidelity: two DIFFERENT runs cannot share a playhead -----------------
    {
        Recording mp = load_rec("abixray-make_pair-sysv.asmtrace");
        Recording s3 = load_rec("abixray-sum3-win64.asmtrace");
        StepIndex mp_idx = build_step_index(mp); // 3 steps
        StepIndex s3_idx = build_step_index(s3); // 4 steps
        wt_model walk = wt_build(mp);
        dt_abixray x = dt_abixray_build(mp_idx, s3_idx, walk);
        vt::check("mismatched lengths are present but NOT aligned",
                  x.present && !x.aligned, "3 vs 4 steps cannot lock");
        vt::check("the banner names both step counts",
                  x.banner.find("NOT ALIGNED") != std::string::npos &&
                      x.banner.find("3 step") != std::string::npos &&
                      x.banner.find("4 step") != std::string::npos,
                  "banner: " + x.banner);
    }

    // --- fidelity: a pane with no producer refuses, and points at --steps -----
    {
        Recording noreg = load_rec("sum_via_rbx.asmtrace"); // predates the ring
        Recording wr = load_rec("abixray-make_pair-win64.asmtrace");
        StepIndex none = build_step_index(noreg);
        StepIndex win64 = build_step_index(wr);
        wt_model walk = wt_build(wr);
        vt::check("the fixture really has no producer", !none.present(),
                  "sum_via_rbx carries no regstate");
        dt_abixray x = dt_abixray_build(none, win64, walk);
        vt::check("a producer-absent pane refuses the x-ray", !x.present,
                  "present must be false");
        vt::check("the placard names the --steps producer",
                  x.message.find("--steps") != std::string::npos,
                  "message: " + x.message);
        vt::check("the view deep-links the docs", !x.docs.empty(),
                  "docs link must be present");
    }

    return vt::report("test_abixray");
}
