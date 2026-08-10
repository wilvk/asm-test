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
    s.asmspy_path = "/opt/asmspy";
    s.ssh_host = "build-box";
    std::string json = settings_serialize(s);
    Settings back;
    check("round-trips", settings_parse(json, back), "settings must parse");
    check("text_scale", back.text_scale == 1.5f, "text_scale drifted");
    check("content_scale", back.content_scale == 2.0f, "content_scale drifted");
    check("win_w", back.win_w == 1600, "win_w drifted");
    check("win_h", back.win_h == 900, "win_h drifted");
    check("light_theme", back.light_theme, "light_theme drifted");
    check("asmspy_path", back.asmspy_path == "/opt/asmspy",
          "asmspy_path drifted");
    check("ssh_host", back.ssh_host == "build-box", "ssh_host drifted");
    // The blank default (auto/local) round-trips as an empty string, not a
    // missing key that reads back as garbage.
    Settings blank;
    Settings blank_back;
    check("blank-round-trips", settings_parse(settings_serialize(blank), blank_back),
          "a default Settings must serialise + parse");
    check("blank-asmspy", blank_back.asmspy_path.empty(),
          "a blank asmspy_path must stay blank");
    check("blank-ssh", blank_back.ssh_host.empty(),
          "a blank ssh_host must stay blank");

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

    // The 3D-scene toggles (live union weave + stable plane layout): both
    // default ON — "all options start as checked", so an `auto`-led session
    // gets both from its very first Start — and an OLD store with neither key
    // must not silently uncheck them.
    Settings sd;
    check("scene-defaults-on", sd.live_union_weave && sd.stable_plane_layout,
          "both 3D-scene toggles must default checked");
    Settings so;
    so.live_union_weave = false;
    so.stable_plane_layout = false;
    Settings sb;
    check("scene-roundtrip",
          settings_parse(settings_serialize(so), sb) && !sb.live_union_weave &&
              !sb.stable_plane_layout,
          "serialize/parse must carry both bools");
    Settings sl;
    check("scene-absent-keys-stay-checked",
          settings_parse("{}", sl) && sl.live_union_weave &&
              sl.stable_plane_layout,
          "an old store without the keys must keep the checked defaults");

    ImGui::DestroyContext();
    if (failures) {
        std::fprintf(stderr, "test_settings: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_settings: all checks passed\n");
    return 0;
}
