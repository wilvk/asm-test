// test_cvd.cpp — the CVD-safe palette + second-channel gate (24 T2 / F15).
//
// A headless assert on the ENCODING, not pixels (D4):
//  (a) every CATEGORICAL entry in the ONE encoding table carries a non-empty
//      second channel (shape / pattern / label) — a colour-only distinction
//      fails here; the legend is generated from the same table, so it cannot
//      drift from the encoding;
//  (b) contrast at the smallest font — text colours >= 4.5:1 against the panel
//      background, fills/borders >= 3:1, and the amber is confined to large text
//      (dt_warn_large_text_only marker present, cleared only at the 3:1 bar);
//  (c) simulate protan/deuter/tritan over the categorical hues and assert every
//      pair that must be distinguished is EITHER still separable OR covered by a
//      second channel — the test passes BECAUSE of the redundant channel, not by
//      pretending the hues alone suffice.
#include <cstdio>
#include <string>

#include "imgui.h"

#include "ui/cvd.h"
#include "ui/legend.h"
#include "ui/theme.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 720);
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    int n = 0;
    const dt_encoding *t = dt_encoding_table(&n);
    check("table-nonempty", n > 0, "the encoding table is empty");

    const ImVec4 bg = dt_panel_bg_col();

    // --- (a) every categorical distinction has a non-colour second channel ----
    for (int i = 0; i < n; i++) {
        if (!t[i].categorical)
            continue;
        check("second-channel",
              t[i].channel != nullptr && t[i].channel[0] != '\0',
              std::string(t[i].key) +
                  " is categorical but has no second channel — a colour-only "
                  "distinction (F15)");
    }

    // --- (b) contrast at the smallest font ------------------------------------
    for (int i = 0; i < n; i++) {
        ImVec4 c = ImGui::ColorConvertU32ToFloat4(t[i].col);
        float ratio = dt_contrast_ratio(c, bg);
        if (t[i].large_text_only) {
            // The amber clears only the 3:1 large-text bar and MUST be marked.
            check("amber-marked", dt_warn_large_text_only(),
                  "dt_warn must be marked large-text-only");
            check("amber-3to1", ratio >= 3.0f,
                  std::string(t[i].key) + " amber below 3:1 large-text (" +
                      std::to_string(ratio) + ")");
        } else if (t[i].text) {
            check("text-4.5to1", ratio >= 4.5f,
                  std::string(t[i].key) + " text below 4.5:1 (" +
                      std::to_string(ratio) + ")");
        } else {
            check("fill-3to1", ratio >= 3.0f,
                  std::string(t[i].key) + " fill/border below 3:1 (" +
                      std::to_string(ratio) + ")");
        }
    }

    // --- (c) CVD simulation: separable OR covered by a second channel ---------
    const dt_cvd_kind kinds[] = {dt_cvd_kind::protan, dt_cvd_kind::deuter,
                                 dt_cvd_kind::tritan};
    for (int i = 0; i < n; i++) {
        if (!t[i].categorical)
            continue;
        for (int j = i + 1; j < n; j++) {
            if (!t[j].categorical)
                continue;
            ImVec4 ci = ImGui::ColorConvertU32ToFloat4(t[i].col);
            ImVec4 cj = ImGui::ColorConvertU32ToFloat4(t[j].col);
            for (dt_cvd_kind k : kinds) {
                float d = dt_cvd_distance(ci, cj, k);
                bool separable = d >= 0.15f;
                bool redundant = t[i].channel[0] != '\0' &&
                                 t[j].channel[0] != '\0' &&
                                 std::string(t[i].channel) != t[j].channel;
                check("cvd-redundant", separable || redundant,
                      std::string(t[i].key) + " vs " + t[j].key +
                          " collapse under a CVD sim with no distinct second "
                          "channel");
            }
        }
    }

    // The test is not vacuous: the classic green/red verdict pair DOES collapse
    // for a deuteranope, which is exactly why the yes/no token is load-bearing.
    ImVec4 good = dt_good_col(), bad = dt_bad_col();
    check("green-red-collapses",
          dt_cvd_distance(good, bad, dt_cvd_kind::deuter) < 0.30f,
          "expected good/bad to be near-indistinguishable under deuteranopia "
          "(so the second channel is what saves it)");
    // The identity kind returns the colour unchanged.
    check("identity", dt_cvd_distance(good, good, dt_cvd_kind::none) == 0.0f,
          "a colour must simulate to itself under kind::none");

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "%d test_cvd check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_cvd: all checks passed\n");
    return 0;
}
