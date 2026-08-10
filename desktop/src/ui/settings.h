// settings.h — user display settings (20-workspace-and-settings.md T5, F6). The
// one accessibility lever fully inside ImGui's control: a DPI-aware font atlas, a
// user text-scale (~0.8x-2.0x), a persisted window size (retiring the hardcoded
// 1280x720), and a light theme. ImGui exposes no OS screen-reader tree
// (11-imgui-addons.md:461, recorded faithfully), so this text-scale lever is the
// accessibility surface the platform actually permits — the Settings pane says so
// rather than implying broader coverage.
//
// Pure model + a thin apply seam: `Settings` is what the tests assert (model
// state, not pixels). Persisted to build/desktop-settings.json beside the dock
// `.ini`; a corrupt store degrades to defaults, never crashes.
#ifndef ASMDESK_UI_SETTINGS_H
#define ASMDESK_UI_SETTINGS_H

#include <string>

struct ImGuiIO;

namespace asmdesk {

struct Settings {
    float text_scale = 1.0f;    // user text-scale, clamped to [kMin, kMax]
    float content_scale = 1.0f; // the OS/GLFW content (DPI) scale of the window
    int win_w = 1280;           // last window size (retires main.cpp's literal)
    int win_h = 720;
    bool light_theme = false; // light vs dark base style

    // The serve-host connection the "Capture a live process" flow uses, moved
    // out of the transient Connect pane so it persists across launches (the pane
    // stays as an on-the-fly fallback). Both blank = the old default: resolve
    // asmspy on $PATH then ./build/asmspy, capture the local machine.
    std::string asmspy_path; // asmspy the host spawns; blank = auto-resolve
    std::string ssh_host;    // ssh target; blank = local

    // The 3D scene's session behaviours, both ON by default so an `auto`-led
    // session gets them from its very first Start ("all options start as
    // checked"). `live_union_weave`: the live tab's 3D pane weaves the UNION
    // of every capture this session made (recordings() ∪ growing — the
    // in-memory twin of a `--record` tee), so a fresh Start ADDS to the scene
    // instead of replacing it. `stable_plane_layout`: regions pack the plane
    // in first-seen order (build_projection keep_order), so a region already
    // placed keeps its domain slot when a later capture adds more.
    bool live_union_weave = true;
    bool stable_plane_layout = true;

    // The 3D viewport's GPU (offscreen-GL) render path, ON by default. OFF
    // takes the SAME honest degraded branch a no-GL build shows — the flat 2D
    // reading surface for the address plane, a stated "no flat form" for the
    // standalone kinds — presented as the user's Settings choice, never as a
    // missing capability. The scene MODELS are woven either way.
    bool use_gpu = true;

    static constexpr float kTextScaleMin = 0.8f;
    static constexpr float kTextScaleMax = 2.0f;
};

std::string settings_serialize(const Settings &s);
// Defensive: bad input leaves `out` at defaults and returns false. Values are
// clamped to sane ranges on the way in (a store hand-edited to text_scale=99
// must not make the UI unusable).
bool settings_parse(const std::string &json, Settings &out);

// Clamp text_scale into [kTextScaleMin, kTextScaleMax].
float settings_clamp_text_scale(float v);

// Apply the cheap live text-scale path: io.FontGlobalScale = clamped text_scale.
// This is the per-frame lever the slider drives; a crisp atlas re-bake at the
// scaled px is the separate rebuild_fonts path (fonts.h). Returns the value set.
float settings_apply_text_scale(const Settings &s, ImGuiIO &io);

} // namespace asmdesk
#endif // ASMDESK_UI_SETTINGS_H
