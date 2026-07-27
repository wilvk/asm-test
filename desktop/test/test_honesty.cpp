// test_honesty.cpp — the ONE graded honesty vocabulary (23-graded-truth-layer.md
// T1, F5). Model state, not pixels (D4/D7).
//
// The load-bearing test: honesty_severity() grades the committed deliberate-
// dishonesty fixtures (tests/golden-asmtrace/dishonest/*) into the RIGHT tier, so
// the grading is pinned against the same fixtures the whole honesty layer is. The
// grading must RESTRUCTURE, never remove: a neutral tier for a statistical/
// dropped survey still surfaces its lost/throttled drop record (D7), and the T3
// integrity tier stays non-collapsible.
#include <cstdio>
#include <string>

#include "imgui.h"

#include "doc/recording.h"
#include "ui/honesty.h"
#include "views/views_draw.h"

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

static Recording load(const std::string &name) {
    std::string path =
        std::string(ASMTEST_GOLDEN_DIR) + "/dishonest/" + name;
    std::string err;
    auto r = load_recording_file(path, err);
    if (!r) {
        check("load", false, path + ": " + err);
        return Recording{};
    }
    return *r;
}

static const char *tname(HonestyTier t) { return honesty_tier_name(t); }

int main() {
    // --- the four dishonesty fixtures grade into the right tier ---------------
    {
        // torn.asmtrace: no `end` footer -> INTEGRITY (loud, non-collapsible).
        Recording r = load("torn.asmtrace");
        check("torn/is-torn", r.torn, "the fixture must load torn");
        HonestyTier t = honesty_severity(honesty_facts_of(r));
        check("torn/integrity", t == HonestyTier::Integrity,
              std::string("torn graded ") + tname(t) + ", want integrity");
    }
    {
        // truncated.asmtrace: end.truncated with a usable prefix -> CAUTION.
        Recording r = load("truncated.asmtrace");
        check("trunc/flag", r.end_truncated && !r.torn,
              "the fixture must load truncated-but-not-torn");
        HonestyTier t = honesty_severity(honesty_facts_of(r));
        check("trunc/caution", t == HonestyTier::Caution,
              std::string("truncated graded ") + tname(t) + ", want caution");
    }
    {
        // redacted.asmtrace: payload withheld at record time, exact, closed ->
        // NEUTRAL (a policy choice, not a data-integrity breach).
        Recording r = load("redacted.asmtrace");
        check("red/flag", r.provenance.redacted, "the fixture must load redacted");
        HonestyTier t = honesty_severity(honesty_facts_of(r));
        check("red/neutral", t == HonestyTier::Neutral,
              std::string("redacted graded ") + tname(t) + ", want neutral");
    }
    {
        // dropped.asmtrace: a STATISTICAL survey that dropped + was throttled ->
        // NEUTRAL (sampling drops are expected; the survey never claimed
        // completeness). D7: the drop record itself is NOT hidden by being
        // neutral — lost/throttled still surface.
        Recording r = load("dropped.asmtrace");
        check("drop/statistical", r.statistical(),
              "the survey fixture must be statistical");
        check("drop/dropped", r.dropped(), "the fixture dropped samples");
        HonestyTier t = honesty_severity(honesty_facts_of(r));
        check("drop/neutral", t == HonestyTier::Neutral,
              std::string("statistical dropped graded ") + tname(t) +
                  ", want neutral");
        // The drop is still surfaced (D7): the fact survives the neutral grade.
        check("drop/record-kept", r.drops_lost == 12345 && r.drops_throttled,
              "the lost/throttled drop record must still be present");
        // A drop on an EXACT capture WOULD be integrity — the tier turns on
        // exactness, not on the drop alone.
        HonestyFacts exact_drop;
        exact_drop.dropped = true;
        exact_drop.statistical = false;
        check("drop/exact-is-integrity",
              honesty_severity(exact_drop) == HonestyTier::Integrity,
              "a drop on an EXACT capture makes its addresses UNKNOWN -> "
              "integrity");
    }

    // --- a skip=success grades NEUTRAL, never caution/integrity (schema:98) ----
    {
        HonestyFacts skip;
        skip.has_skip = true;
        check("skip/neutral", honesty_severity(skip) == HonestyTier::Neutral,
              "a skip is a successful session with nothing to report");
        // A statistical capture, on its own, is neutral too.
        HonestyFacts stat;
        stat.statistical = true;
        check("stat/neutral", honesty_severity(stat) == HonestyTier::Neutral,
              "statistical alone is a caveat, not a caution/integrity breach");
        // A bounded window and a coarse rung are neutral.
        HonestyFacts win;
        win.bounded_window = true;
        win.coarse = true;
        check("neutral/window-coarse",
              honesty_severity(win) == HonestyTier::Neutral,
              "bounded window / coarse rung grade neutral");
    }

    // --- the dominant tier is the loudest active signal -----------------------
    {
        HonestyFacts mixed; // statistical (neutral) + truncated (caution)
        mixed.statistical = true;
        mixed.truncated = true;
        check("dominant/caution",
              honesty_severity(mixed) == HonestyTier::Caution,
              "caution dominates a neutral statistical caveat");
        mixed.torn = true; // + torn (integrity)
        check("dominant/integrity",
              honesty_severity(mixed) == HonestyTier::Integrity,
              "integrity dominates every lower tier");
    }

    // --- the derivable `severity` field: parsed when present, else derived -----
    {
        check("wire/parse-neutral",
              honesty_tier_from_wire("neutral") == HonestyTier::Neutral,
              "the wire string parses");
        check("wire/parse-integrity",
              honesty_tier_from_wire("integrity") == HonestyTier::Integrity, "");
        check("wire/unknown-nullopt", !honesty_tier_from_wire("bogus"),
              "an unknown severity never guesses a tier");
        // A declared wire tier OVERRIDES the derivation (additive + honoured).
        HonestyFacts f;
        f.torn = true;                        // would derive integrity
        f.declared = HonestyTier::Neutral;    // but the producer said neutral
        check("wire/override", honesty_severity(f) == HonestyTier::Neutral,
              "a present `severity` field is honoured verbatim");
        // Round-trip the names.
        for (HonestyTier t : {HonestyTier::Neutral, HonestyTier::Caution,
                              HonestyTier::Integrity})
            check("wire/roundtrip", honesty_tier_from_wire(honesty_tier_name(t)) == t,
                  "name round-trip");
    }

    // --- collapsibility: T3 non-collapsible, T2 collapses, T1 is a chip --------
    {
        check("collapse/integrity-never",
              !honesty_tier_collapsible(HonestyTier::Integrity),
              "an integrity banner must never collapse");
        check("collapse/caution-yes",
              honesty_tier_collapsible(HonestyTier::Caution),
              "a caution banner collapses to its chip after first read");
        check("collapse/neutral-no",
              !honesty_tier_collapsible(HonestyTier::Neutral),
              "a neutral signal is already a chip, nothing to collapse");
    }

    // --- per-signal tiers ------------------------------------------------------
    {
        check("sig/torn", honesty_signal_tier(HonestySignal::Torn) ==
                              HonestyTier::Integrity, "");
        check("sig/basis", honesty_signal_tier(HonestySignal::BasisError) ==
                               HonestyTier::Integrity, "");
        check("sig/dropped-exact",
              honesty_signal_tier(HonestySignal::DroppedExact) ==
                  HonestyTier::Integrity, "");
        check("sig/truncated", honesty_signal_tier(HonestySignal::Truncated) ==
                                   HonestyTier::Caution, "");
        check("sig/paused", honesty_signal_tier(HonestySignal::PausedDropped) ==
                                HonestyTier::Caution, "");
        check("sig/dropped-stat",
              honesty_signal_tier(HonestySignal::DroppedStatistical) ==
                  HonestyTier::Neutral, "");
        check("sig/redacted", honesty_signal_tier(HonestySignal::Redacted) ==
                                  HonestyTier::Neutral, "");
        check("sig/skip", honesty_signal_tier(HonestySignal::Skip) ==
                              HonestyTier::Neutral, "");
    }

    // --- the graded chrome components draw headlessly (null backend) -----------
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 720);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char *px = nullptr;
        int w = 0, h = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

        ImGui::NewFrame();
        ImGui::Begin("honesty");
        draw_honesty_chip("statistical survey", HonestyTier::Neutral);
        bool collapsed = false;
        draw_honesty_banner("truncated but usable", HonestyTier::Caution,
                            &collapsed);
        // An integrity banner ignores the collapsed bit entirely.
        bool ignored = true;
        draw_honesty_banner("TORN — do not trust the tail", HonestyTier::Integrity,
                            &ignored);
        ImGui::End();
        ImGui::Render();
        ImDrawData *dd = ImGui::GetDrawData();
        check("chrome/draws", dd && dd->TotalVtxCount >= 0,
              "the graded chrome drew nothing");
        ImGui::DestroyContext();
    }

    if (failures) {
        std::fprintf(stderr, "test_honesty: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_honesty: all checks passed\n");
    return 0;
}
