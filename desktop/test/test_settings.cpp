// test_settings.cpp — user display settings (20-workspace-and-settings.md T5,
// F6). Null backend, model state (D4): the Settings struct round-trips through
// serialise/parse; setting text_scale and calling the apply function sets
// io.FontGlobalScale to that value (the IO field, not pixels); the clamp bounds
// a hand-edited store; and a corrupt store degrades to defaults.
#include <cstdio>

#include "imgui.h"

#include "ui/settings.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // --- round-trip ---------------------------------------------------------
    Settings s;
    s.text_scale = 1.5f;
    s.content_scale = 2.0f;
    s.win_w = 1600;
    s.win_h = 900;
    s.light_theme = true;
    std::string json = settings_serialize(s);
    Settings back;
    check("round-trips", settings_parse(json, back), "settings must parse");
    check("text_scale", back.text_scale == 1.5f, "text_scale drifted");
    check("content_scale", back.content_scale == 2.0f, "content_scale drifted");
    check("win_w", back.win_w == 1600, "win_w drifted");
    check("win_h", back.win_h == 900, "win_h drifted");
    check("light_theme", back.light_theme, "light_theme drifted");

    // --- clamp: a hand-edited store must not make the UI unusable ------------
    check("clamp-hi", settings_clamp_text_scale(3.0f) == Settings::kTextScaleMax,
          "text_scale must clamp to the max");
    check("clamp-lo", settings_clamp_text_scale(0.1f) == Settings::kTextScaleMin,
          "text_scale must clamp to the min");
    check("clamp-bad", settings_clamp_text_scale(-1.0f) == 1.0f,
          "a nonsensical scale must fall back to 1.0");

    // --- apply: FontGlobalScale follows text_scale --------------------------
    Settings a;
    a.text_scale = 1.5f;
    float applied = settings_apply_text_scale(a, io);
    check("apply-returns", applied == 1.5f, "apply must return the value set");
    check("apply-io", io.FontGlobalScale == 1.5f,
          "settings_apply_text_scale must set io.FontGlobalScale");
    // An out-of-range stored value is clamped by apply, not passed through raw.
    Settings big;
    big.text_scale = 9.0f;
    settings_apply_text_scale(big, io);
    check("apply-clamps", io.FontGlobalScale == Settings::kTextScaleMax,
          "apply must clamp before touching FontGlobalScale");

    // --- defensive parse ----------------------------------------------------
    Settings d;
    check("parse-garbage", !settings_parse("nonsense", d),
          "malformed input must return false");
    check("parse-default", d.text_scale == 1.0f && d.win_w == 1280,
          "a bad parse must leave defaults");
    // A store hand-edited to an absurd size is bounded on the way in.
    Settings hb;
    check("parse-bounds",
          settings_parse("{\"win_w\":999999,\"text_scale\":50}", hb),
          "an object parses");
    check("parse-bounds-w", hb.win_w == 1280,
          "an absurd win_w falls back to the default");
    check("parse-bounds-ts", hb.text_scale == Settings::kTextScaleMax,
          "an absurd text_scale is clamped");

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "test_settings: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_settings: all checks passed\n");
    return 0;
}
