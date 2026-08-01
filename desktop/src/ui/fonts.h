// fonts.h — the real monospace font + merged debugger icons
// (docs/internal/archive/gui/13-foundation-moves.md F3).
#ifndef ASMDESK_UI_FONTS_H
#define ASMDESK_UI_FONTS_H

struct ImGuiIO;

namespace asmdesk {

// Load JetBrains Mono as the default UI font and merge the Codicons icon range
// (ICON_CI_*) over it, so hex/registers/disasm read on a real monospace face and
// the scrubber/observer/patch-bay can carry step-into/over/watch/breakpoint
// glyphs. GRACEFUL by design: a missing/unreadable TTF leaves the built-in
// bitmap font and returns false — the app degrades gracefully rather than
// asserting on a stripped install. Works via stb_truetype; a build with
// IMGUI_ENABLE_FREETYPE (the Docker desktop lane) simply rasterises it better.
//
// `base_px` × `content_scale` is the DPI-aware size the atlas is baked at
// (20-workspace-and-settings.md T5, F6): main.cpp passes the GLFW window content
// scale and re-bakes on a content-scale change, so a HiDPI display gets a crisp
// atlas rather than a 15px one upscaled. Both default to the historical 15.0f × 1
// so every existing caller and test is unaffected.
bool load_fonts(ImGuiIO &io, const char *jbm_ttf, const char *codicon_ttf,
                const char *fa_ttf, float base_px = 15.0f,
                float content_scale = 1.0f);

// How many times load_fonts has (re)built the atlas — a test seam for the
// DPI-rebuild path (T5): a content-scale change must trigger a fresh bake, and a
// pure counter is what a headless test can assert without a display.
unsigned long fonts_rebuild_count();

} // namespace asmdesk
#endif // ASMDESK_UI_FONTS_H
