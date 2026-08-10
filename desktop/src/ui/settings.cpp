// settings.cpp — see settings.h (20-workspace-and-settings.md T5).
#include "ui/settings.h"

#include <algorithm>

#include <nlohmann/json.hpp>

#include "imgui.h"

namespace asmdesk {

using nlohmann::json;

float settings_clamp_text_scale(float v) {
    if (!(v > 0.0f)) // NaN or <=0 from a hand-edited store -> the default
        return 1.0f;
    return std::clamp(v, Settings::kTextScaleMin, Settings::kTextScaleMax);
}

std::string settings_serialize(const Settings &s) {
    json j;
    j["text_scale"] = s.text_scale;
    j["content_scale"] = s.content_scale;
    j["win_w"] = s.win_w;
    j["win_h"] = s.win_h;
    j["light_theme"] = s.light_theme;
    j["asmspy_path"] = s.asmspy_path;
    j["ssh_host"] = s.ssh_host;
    j["live_union_weave"] = s.live_union_weave;
    j["stable_plane_layout"] = s.stable_plane_layout;
    return j.dump(2);
}

bool settings_parse(const std::string &text, Settings &out) {
    out = Settings{};
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object())
        return false;
    if (auto it = j.find("text_scale"); it != j.end() && it->is_number())
        out.text_scale = settings_clamp_text_scale(it->get<float>());
    if (auto it = j.find("content_scale"); it != j.end() && it->is_number()) {
        float c = it->get<float>();
        out.content_scale = (c > 0.1f && c < 8.0f) ? c : 1.0f;
    }
    if (auto it = j.find("win_w"); it != j.end() && it->is_number_integer()) {
        int w = it->get<int>();
        out.win_w = (w >= 320 && w <= 16384) ? w : 1280;
    }
    if (auto it = j.find("win_h"); it != j.end() && it->is_number_integer()) {
        int h = it->get<int>();
        out.win_h = (h >= 240 && h <= 16384) ? h : 720;
    }
    if (auto it = j.find("light_theme"); it != j.end() && it->is_boolean())
        out.light_theme = it->get<bool>();
    // Free-form strings; a blank (or missing) field is the auto/local default, so
    // no clamp is meaningful — a hand-edited path is the operator's to own.
    if (auto it = j.find("asmspy_path"); it != j.end() && it->is_string())
        out.asmspy_path = it->get<std::string>();
    if (auto it = j.find("ssh_host"); it != j.end() && it->is_string())
        out.ssh_host = it->get<std::string>();
    // Absent keys keep the checked defaults (an old store predates them).
    if (auto it = j.find("live_union_weave"); it != j.end() && it->is_boolean())
        out.live_union_weave = it->get<bool>();
    if (auto it = j.find("stable_plane_layout");
        it != j.end() && it->is_boolean())
        out.stable_plane_layout = it->get<bool>();
    return true;
}

float settings_apply_text_scale(const Settings &s, ImGuiIO &io) {
    float v = settings_clamp_text_scale(s.text_scale);
    io.FontGlobalScale = v;
    return v;
}

} // namespace asmdesk
