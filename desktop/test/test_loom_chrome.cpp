// test_loom_chrome.cpp — the Loom's honesty chrome (05-loom-day-one.md T2, D7).
//
// Every assertion here is about a SENTENCE the user reads. The words are the
// interface: "alive at trace end" and "thread died" are the same rectangle and
// opposite claims, and only one of them is something the fabric knows. These
// strings are pinned verbatim so a re-word has to be a deliberate change to a
// measurement claim, reviewed as one.
#include <cstdio>
#include <string>
#include <vector>

#include "loom/fabric_plan.h"
#include "loom_fixture.h"

using namespace asmdesk;
using namespace loomfx;

static int failures;
static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}

static std::vector<const loom_prim_t *> of(const std::vector<loom_prim_t> &p,
                                           loom_prim k) {
    std::vector<const loom_prim_t *> v;
    for (const loom_prim_t &x : p)
        if (x.kind == k)
            v.push_back(&x);
    return v;
}

static loom_view_t wide() {
    loom_view_t v;
    v.steps_per_px = 0.05;
    v.px_w = 800;
    v.px_h = 400;
    v.lane_h = 18;
    return v;
}

int main() {
    Fixture fx;
    std::vector<loom_prim_t> prims;

    // --- torn edge: the exact N-of-M copy ----------------------------------
    {
        loom_provenance_t p = prov();
        p.truncated = true;
        p.steps_recorded = 6;
        p.steps_total = 19;
        loom_fabric_t f = build(fx, p);
        loom_plan(f, wide(), &prims);
        auto torn = of(prims, loom_prim::torn_edge);
        check("a truncated fabric gets exactly one torn edge", torn.size() == 1,
              "got " + std::to_string(torn.size()));
        if (torn.size() == 1)
            check("the torn edge names N of M",
                  torn[0]->text == "trace truncated: 6 of 19 steps recorded",
                  "text was: " + torn[0]->text);
    }

    // The v1 schema carries no dataflow step total, so a replayed truncated
    // recording usually cannot supply M. The banner must NAME that gap, never
    // print "6 of 6" — which would claim the run ended where the buffer did.
    {
        loom_provenance_t p = prov();
        p.truncated = true;
        p.steps_recorded = 6;
        p.steps_total = 0;
        loom_fabric_t f = build(fx, p);
        loom_plan(f, wide(), &prims);
        auto torn = of(prims, loom_prim::torn_edge);
        check("an unknown total still tears", torn.size() == 1, "");
        if (torn.size() == 1) {
            check("the unknown-total copy names the gap",
                  torn[0]->text == "trace truncated: 6 steps recorded; this "
                                   "feed did not record the total",
                  "text was: " + torn[0]->text);
            check("it never invents an M",
                  torn[0]->text.find("of 6") == std::string::npos,
                  "text was: " + torn[0]->text);
        }
    }

    // --- an untruncated fabric has NO torn edge ----------------------------
    {
        loom_fabric_t f = build(fx);
        loom_plan(f, wide(), &prims);
        check("a complete fabric is not torn",
              of(prims, loom_prim::torn_edge).empty(), "a torn edge appeared");
    }

    // --- fade-out: one per alive-at-end span, and never a death cap --------
    {
        loom_fabric_t f = build(fx);
        loom_plan(f, wide(), &prims);
        size_t alive = 0;
        for (const loom_span_t &s : f.spans)
            if (s.t_end == kLoomAlive)
                alive++;
        auto fades = of(prims, loom_prim::fade_out);
        check("one fade-out per span still live at the last recorded step",
              fades.size() == alive,
              "got " + std::to_string(fades.size()) + " for " +
                  std::to_string(alive) + " alive spans");
        check("there IS something alive at the end", alive > 0, "");
        for (const loom_prim_t *p : fades)
            check("the fade-out says only what is known",
                  p->text == std::string(kLoomFadeOutText), "text: " + p->text);
        check("the fade-out copy is exactly the plan's wording",
              std::string(kLoomFadeOutText) == "alive at trace end",
              kLoomFadeOutText);
    }

    // --- born-of-untraced-state glyphs -------------------------------------
    {
        loom_fabric_t f = build(fx);
        loom_plan(f, wide(), &prims);
        auto glyphs = of(prims, loom_prim::born_untraced_glyph);
        size_t born = 0;
        for (const loom_span_t &s : f.spans)
            if (s.born_untraced)
                born++;
        check("one glyph per synthetic entry span", glyphs.size() == born,
              "got " + std::to_string(glyphs.size()) + " for " +
                  std::to_string(born));
        check("the entry args have glyphs", born >= 2, "");
        for (const loom_prim_t *p : glyphs)
            check("the glyph's hover text is the plan's wording",
                  p->text == std::string(kLoomBornUntracedText),
                  "text: " + p->text);
        check("the born-untraced copy is verbatim",
              std::string(kLoomBornUntracedText) ==
                  "born of untraced state — provenance starts at "
                  "instrumentation",
              kLoomBornUntracedText);
    }

    // --- the isolated-guest badge, and only when earned --------------------
    {
        loom_fabric_t f = build(fx);
        loom_plan(f, wide(), &prims);
        auto badge = of(prims, loom_prim::guest_badge);
        check("an emulator fabric carries the badge", badge.size() == 1,
              "got " + std::to_string(badge.size()));
        if (badge.size() == 1)
            check("the badge copy is verbatim",
                  badge[0]->text == "isolated guest — emulator replay, not "
                                    "silicon",
                  "text: " + badge[0]->text);

        loom_provenance_t p = prov();
        p.isolated_guest = false;
        p.producer = "ptrace-region";
        loom_fabric_t g = build(fx, p);
        loom_plan(g, wide(), &prims);
        check("a native fabric carries no guest badge",
              of(prims, loom_prim::guest_badge).empty(),
              "a native producer got the emulator badge");
    }

    // --- hollow threads stay hollow ----------------------------------------
    {
        loom_fabric_t f = build(fx);
        loom_plan(f, wide(), &prims);
        check("the deferred rip write plans as a HOLLOW span",
              !of(prims, loom_prim::span_hollow).empty(),
              "a value the producer never captured was drawn as if it had one");
        for (const loom_prim_t *p : of(prims, loom_prim::span_hollow))
            check("a hollow span carries no value chip", p->text.empty(),
                  "text: " + p->text);
    }

    // --- statistical-never-woven, restated at the chrome layer -------------
    {
        loom_provenance_t p = prov();
        p.exact = false;
        p.producer = "ibs";
        loom_fabric_t f;
        std::string err;
        bool built = loom_fabric_build(&fx.vt, &fx.g, p, &f, &err);
        check("an ibs feed never becomes fabric", !built, "it wove");
        check("the refusal names the statistical rule",
              err.find("statistical") != std::string::npos, "err: " + err);
        check("the refusal names the producer",
              err.find("ibs") != std::string::npos, "err: " + err);
        check("the refusal points at the annex",
              err.find("annex") != std::string::npos, "err: " + err);
    }

    if (failures) {
        std::fprintf(stderr, "%d loom chrome check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_loom_chrome: all checks passed\n");
    return 0;
}
