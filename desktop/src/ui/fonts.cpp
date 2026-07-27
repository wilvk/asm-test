// fonts.cpp — see fonts.h (13-foundation-moves.md F3).
#include "ui/fonts.h"

#include <cstdio>

#include "imgui.h"

#include "IconsCodicons.h"     // ICON_MIN_CI / ICON_MAX_CI / ICON_CI_* (zlib)
#include "IconsFontAwesome6.h" // ICON_MIN_FA / ICON_MAX_FA (for ImGuiNotify, 16 T1)

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

bool load_fonts(ImGuiIO &io, const char *jbm_ttf, const char *codicon_ttf,
                const char *fa_ttf) {
    // No monospace TTF on disk -> keep the built-in bitmap font (honest degrade).
    if (!readable(jbm_ttf))
        return false;

    io.Fonts->Clear();
    io.Fonts->AddFontFromFileTTF(jbm_ttf, 15.0f);

    // Merge each icon range onto the same font so ICON_* macros render inline in
    // labels. Each is optional (absent TTF -> no icons from that set). The range
    // arrays are static const so they outlive the deferred atlas Build().
    if (readable(codicon_ttf)) { // debugger icons (13 F3)
        static const ImWchar ci_range[] = {ICON_MIN_CI, ICON_MAX_CI, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = 15.0f; // keep icons monospace-aligned
        io.Fonts->AddFontFromFileTTF(codicon_ttf, 15.0f, &cfg, ci_range);
    }
    if (readable(fa_ttf)) { // FontAwesome, for ImGuiNotify toasts (16 T1)
        static const ImWchar fa_range[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = 15.0f;
        io.Fonts->AddFontFromFileTTF(fa_ttf, 15.0f, &cfg, fa_range);
    }

    io.Fonts->Build();
    return true;
}

} // namespace asmdesk
