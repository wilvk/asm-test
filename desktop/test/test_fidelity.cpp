// test_fidelity.cpp — the ONE graded fidelity vocabulary (23-graded-truth-layer.md
// T1, F5). Model state, not pixels (D4/D7).
//
// The load-bearing test: fidelity_severity() grades the committed deliberately
// low-fidelity fixtures (tests/golden-asmtrace/low-fidelity/*) into the RIGHT tier, so
// the grading is pinned against the same fixtures the whole fidelity layer is. The
// grading must RESTRUCTURE, never remove: a neutral tier for a statistical/
// dropped survey still surfaces its lost/throttled drop record (D7), and the T3
// integrity tier stays non-collapsible.
#include <cstdio>
#include <string>

#include "imgui.h"

#include "doc/recording.h"
#include "ui/fidelity.h"
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
        std::string(ASMTEST_GOLDEN_DIR) + "/low-fidelity/" + name;
    std::string err;
    auto r = load_recording_file(path, err);
    if (!r) {
        check("load", false, path + ": " + err);
        return Recording{};
    }
    return *r;
}

static const char *tname(FidelityTier t) { return fidelity_tier_name(t); }

int main() {
    // --- the four low-fidelity fixtures grade into the right tier ---------------
    {
        // torn.asmtrace: no `end` footer -> INTEGRITY (loud, non-collapsible).
        Recording r = load("torn.asmtrace");
        check("torn/is-torn", r.torn, "the fixture must load torn");
        FidelityTier t = fidelity_severity(fidelity_facts_of(r));
        check("torn/integrity", t == FidelityTier::Integrity,
              std::string("torn graded ") + tname(t) + ", want integrity");
    }
    {
        // truncated.asmtrace: end.truncated with a usable prefix -> CAUTION.
        Recording r = load("truncated.asmtrace");
        check("trunc/flag", r.end_truncated && !r.torn,
              "the fixture must load truncated-but-not-torn");
        FidelityTier t = fidelity_severity(fidelity_facts_of(r));
        check("trunc/caution", t == FidelityTier::Caution,
              std::string("truncated graded ") + tname(t) + ", want caution");
    }
    {
        // redacted.asmtrace: payload withheld at record time, exact, closed ->
        // NEUTRAL (a policy choice, not a data-integrity breach).
        Recording r = load("redacted.asmtrace");
        check("red/flag", r.provenance.redacted, "the fixture must load redacted");
        FidelityTier t = fidelity_severity(fidelity_facts_of(r));
        check("red/neutral", t == FidelityTier::Neutral,
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
        FidelityTier t = fidelity_severity(fidelity_facts_of(r));
        check("drop/neutral", t == FidelityTier::Neutral,
              std::string("statistical dropped graded ") + tname(t) +
                  ", want neutral");
        // The drop is still surfaced (D7): the fact survives the neutral grade.
        check("drop/record-kept", r.drops_lost == 12345 && r.drops_throttled,
              "the lost/throttled drop record must still be present");
        // A drop on an EXACT capture WOULD be integrity — the tier turns on
        // exactness, not on the drop alone.
        FidelityFacts exact_drop;
        exact_drop.dropped = true;
        exact_drop.statistical = false;
        check("drop/exact-is-integrity",
              fidelity_severity(exact_drop) == FidelityTier::Integrity,
              "a drop on an EXACT capture makes its addresses UNKNOWN -> "
              "integrity");
    }

    // --- a skip=success grades NEUTRAL, never caution/integrity (schema:98) ----
    {
        FidelityFacts skip;
        skip.has_skip = true;
        check("skip/neutral", fidelity_severity(skip) == FidelityTier::Neutral,
              "a skip is a successful session with nothing to report");
        // A statistical capture, on its own, is neutral too.
        FidelityFacts stat;
        stat.statistical = true;
        check("stat/neutral", fidelity_severity(stat) == FidelityTier::Neutral,
              "statistical alone is a caveat, not a caution/integrity breach");
        // A bounded window and a coarse rung are neutral.
        FidelityFacts win;
        win.bounded_window = true;
        win.coarse = true;
        check("neutral/window-coarse",
              fidelity_severity(win) == FidelityTier::Neutral,
              "bounded window / coarse rung grade neutral");
    }

    // --- the dominant tier is the loudest active signal -----------------------
    {
        FidelityFacts mixed; // statistical (neutral) + truncated (caution)
        mixed.statistical = true;
        mixed.truncated = true;
        check("dominant/caution",
              fidelity_severity(mixed) == FidelityTier::Caution,
              "caution dominates a neutral statistical caveat");
        mixed.torn = true; // + torn (integrity)
        check("dominant/integrity",
              fidelity_severity(mixed) == FidelityTier::Integrity,
              "integrity dominates every lower tier");
    }

    // --- the derivable `severity` field: parsed when present, else derived -----
    {
        check("wire/parse-neutral",
              fidelity_tier_from_wire("neutral") == FidelityTier::Neutral,
              "the wire string parses");
        check("wire/parse-integrity",
              fidelity_tier_from_wire("integrity") == FidelityTier::Integrity, "");
        check("wire/unknown-nullopt", !fidelity_tier_from_wire("bogus"),
              "an unknown severity never guesses a tier");
        // A declared wire tier OVERRIDES the derivation (additive + honoured).
        FidelityFacts f;
        f.torn = true;                        // would derive integrity
        f.declared = FidelityTier::Neutral;    // but the producer said neutral
        check("wire/override", fidelity_severity(f) == FidelityTier::Neutral,
              "a present `severity` field is honoured verbatim");
        // Round-trip the names.
        for (FidelityTier t : {FidelityTier::Neutral, FidelityTier::Caution,
                              FidelityTier::Integrity})
            check("wire/roundtrip", fidelity_tier_from_wire(fidelity_tier_name(t)) == t,
                  "name round-trip");
    }

    // --- collapsibility: T3 non-collapsible, T2 collapses, T1 is a chip --------
    {
        check("collapse/integrity-never",
              !fidelity_tier_collapsible(FidelityTier::Integrity),
              "an integrity banner must never collapse");
        check("collapse/caution-yes",
              fidelity_tier_collapsible(FidelityTier::Caution),
              "a caution banner collapses to its chip after first read");
        check("collapse/neutral-no",
              !fidelity_tier_collapsible(FidelityTier::Neutral),
              "a neutral signal is already a chip, nothing to collapse");
    }

    // --- per-signal tiers ------------------------------------------------------
    {
        check("sig/torn", fidelity_signal_tier(FidelitySignal::Torn) ==
                              FidelityTier::Integrity, "");
        check("sig/basis", fidelity_signal_tier(FidelitySignal::BasisError) ==
                               FidelityTier::Integrity, "");
        check("sig/dropped-exact",
              fidelity_signal_tier(FidelitySignal::DroppedExact) ==
                  FidelityTier::Integrity, "");
        check("sig/truncated", fidelity_signal_tier(FidelitySignal::Truncated) ==
                                   FidelityTier::Caution, "");
        check("sig/paused", fidelity_signal_tier(FidelitySignal::PausedDropped) ==
                                FidelityTier::Caution, "");
        check("sig/dropped-stat",
              fidelity_signal_tier(FidelitySignal::DroppedStatistical) ==
                  FidelityTier::Neutral, "");
        check("sig/redacted", fidelity_signal_tier(FidelitySignal::Redacted) ==
                                  FidelityTier::Neutral, "");
        check("sig/skip", fidelity_signal_tier(FidelitySignal::Skip) ==
                              FidelityTier::Neutral, "");
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
        ImGui::Begin("fidelity");
        draw_fidelity_chip("statistical survey", FidelityTier::Neutral);
        bool collapsed = false;
        draw_fidelity_banner("truncated but usable", FidelityTier::Caution,
                            &collapsed);
        // An integrity banner ignores the collapsed bit entirely.
        bool ignored = true;
        draw_fidelity_banner("TORN — do not trust the tail", FidelityTier::Integrity,
                            &ignored);
        ImGui::End();
        ImGui::Render();
        ImDrawData *dd = ImGui::GetDrawData();
        check("chrome/draws", dd && dd->TotalVtxCount >= 0,
              "the graded chrome drew nothing");
        ImGui::DestroyContext();
    }

    if (failures) {
        std::fprintf(stderr, "test_fidelity: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_fidelity: all checks passed\n");
    return 0;
}
