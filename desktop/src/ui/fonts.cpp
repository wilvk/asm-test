// fonts.cpp — see fonts.h (13-foundation-moves.md F3).
#include "ui/fonts.h"

#include <cstdio>

#include "imgui.h"

#include "IconsCodicons.h" // ICON_MIN_CI / ICON_MAX_CI / ICON_CI_* (zlib)

namespace asmdesk {

static bool readable(const char *p) {
    if (!p || !*p)
        return false;
    FILE *f = std::fopen(p, "rb");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

bool load_fonts(ImGuiIO &io, const char *jbm_ttf, const char *codicon_ttf) {
    // No monospace TTF on disk -> keep the built-in bitmap font (honest degrade).
    if (!readable(jbm_ttf))
        return false;

    io.Fonts->Clear();
    io.Fonts->AddFontFromFileTTF(jbm_ttf, 15.0f);

    // Merge the Codicons range onto the same font so ICON_CI_* macros render
    // inline in labels. Absent codicon.ttf is fine — text just shows no icons.
    if (readable(codicon_ttf)) {
        static const ImWchar range[] = {ICON_MIN_CI, ICON_MAX_CI, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = 15.0f; // keep the icons monospace-aligned
        io.Fonts->AddFontFromFileTTF(codicon_ttf, 15.0f, &cfg, range);
    }

    io.Fonts->Build();
    return true;
}

} // namespace asmdesk
