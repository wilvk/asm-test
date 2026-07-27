// perspectives.cpp — see perspectives.h (20-workspace-and-settings.md T4).
#include "ui/perspectives.h"

#include <cstring>

#include "ui/layout.h" // layout_build, LayoutPreset, layout_preset_name

namespace asmdesk {

static const char kPresetPrefix[] = "preset:";
static const char kIniPrefix[] = "ini:";

std::string perspective_preset_value(const char *preset_name) {
    return std::string(kPresetPrefix) + (preset_name ? preset_name : "");
}

std::string perspective_snapshot() {
    // SaveIniSettingsToMemory returns the whole ini (dock layout + window state);
    // for a named perspective the dock layout is what matters, and carrying the
    // rest costs nothing and lets an "ini:" perspective round-trip faithfully.
    return std::string(kIniPrefix) + ImGui::SaveIniSettingsToMemory(nullptr);
}

bool perspective_apply(const std::string &value, ImGuiID dockspace_id,
                       ImVec2 size) {
    if (value.rfind(kPresetPrefix, 0) == 0) {
        std::string name = value.substr(sizeof(kPresetPrefix) - 1);
        for (LayoutPreset p : {LayoutPreset::ReplayInspect, LayoutPreset::Author,
                               LayoutPreset::LiveObserver})
            if (name == layout_preset_name(p)) {
                layout_build(dockspace_id, size, p);
                return true;
            }
        return false; // an unknown preset name — degrade, never crash
    }
    if (value.rfind(kIniPrefix, 0) == 0) {
        std::string blob = value.substr(sizeof(kIniPrefix) - 1);
        ImGui::LoadIniSettingsFromMemory(blob.c_str(), blob.size());
        return true;
    }
    return false;
}

size_t filter_preset_apply(const std::string &query, char *buf,
                           size_t buf_size) {
    if (buf == nullptr || buf_size == 0)
        return 0;
    size_t n = query.size();
    if (n > buf_size - 1)
        n = buf_size - 1;
    std::memcpy(buf, query.data(), n);
    buf[n] = '\0';
    return n;
}

} // namespace asmdesk
