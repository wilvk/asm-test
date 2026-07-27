// fonts.h — the real monospace font + merged debugger icons
// (docs/internal/gui/13-foundation-moves.md F3).
#ifndef ASMDESK_UI_FONTS_H
#define ASMDESK_UI_FONTS_H

struct ImGuiIO;

namespace asmdesk {

// Load JetBrains Mono as the default UI font and merge the Codicons icon range
// (ICON_CI_*) over it, so hex/registers/disasm read on a real monospace face and
// the scrubber/observer/patch-bay can carry step-into/over/watch/breakpoint
// glyphs. GRACEFUL by design: a missing/unreadable TTF leaves the built-in
// bitmap font and returns false — the app degrades honestly rather than
// asserting on a stripped install. Works via stb_truetype; a build with
// IMGUI_ENABLE_FREETYPE (the Docker desktop lane) simply rasterises it better.
bool load_fonts(ImGuiIO &io, const char *jbm_ttf, const char *codicon_ttf,
                const char *fa_ttf);

} // namespace asmdesk
#endif // ASMDESK_UI_FONTS_H
