// test_fonts.cpp — the real monospace font + merged Codicons (13-foundation-
// moves.md F3), built headless via stb_truetype (no freetype needed here). Pins
// that the vendored TTFs load, the atlas builds, a Codicons glyph is present, and
// a missing font degrades honestly to the built-in bitmap.
#include <cstdio>

#include "imgui.h"

#include "IconsCodicons.h"
#include "IconsFontAwesome6.h"
#include "ui/fonts.h"

#ifndef ASMTEST_JBM_TTF
#define ASMTEST_JBM_TTF ""
#endif
#ifndef ASMTEST_CODICON_TTF
#define ASMTEST_CODICON_TTF ""
#endif
#ifndef ASMTEST_FA_TTF
#define ASMTEST_FA_TTF ""
#endif

static int fails;
static void check(const char *what, bool cond) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        fails++;
    }
}

int main() {
    IMGUI_CHECKVERSION();

    // 1) the real fonts load and the atlas builds with a Codicons glyph merged.
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    check("load_fonts accepts the vendored JetBrains Mono TTF",
          asmdesk::load_fonts(io, ASMTEST_JBM_TTF, ASMTEST_CODICON_TTF,
                              ASMTEST_FA_TTF));
    unsigned char *px = nullptr;
    int w = 0, h = 0;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    check("the font atlas builds", w > 0 && h > 0);
    ImFont *f = io.Fonts->Fonts.empty() ? nullptr : io.Fonts->Fonts[0];
    check("a font is in the atlas", f != nullptr);
    if (f) {
        check("a Codicons glyph (ICON_MIN_CI) is merged in",
              f->FindGlyphNoFallback(static_cast<ImWchar>(ICON_MIN_CI)) !=
                  nullptr);
        check("a FontAwesome glyph (ICON_MIN_FA) is merged in (for ImGuiNotify)",
              f->FindGlyphNoFallback(static_cast<ImWchar>(ICON_MIN_FA)) !=
                  nullptr);
        // The TEXT punctuation the UI actually uses, which the DEFAULT glyph range
        // (Latin-1 only) left out — so it rendered as '?' until fonts.cpp asked for
        // the right blocks. Each codepoint is from a rendered string: the em-dash
        // (>1000 uses), the ellipsis in "Open…"/"Browse…", the "-> " arrow, the
        // bullet, the ▸/◄ menu + nav triangles, the ✕ close, the ∘/∩ set operators.
        // FindGlyphNoFallback is null when the glyph was never baked — exactly the
        // old bug, so this fails loudly if the range ever regresses.
        struct {
            ImWchar cp;
            const char *name;
        } punct[] = {
            {0x2014, "em-dash U+2014"},   {0x2026, "ellipsis U+2026"},
            {0x2192, "arrow U+2192"},     {0x2022, "bullet U+2022"},
            {0x25B8, "triangle U+25B8"},  {0x25C4, "pointer U+25C4"},
            {0x2715, "mult-x U+2715"},    {0x2218, "ring U+2218"},
            {0x2229, "intersect U+2229"},
        };
        for (auto &g : punct)
            check(g.name, f->FindGlyphNoFallback(g.cp) != nullptr);
    }
    ImGui::DestroyContext();

    // 2) honest degrade: a missing TTF returns false and leaves a usable default.
    ImGui::CreateContext();
    ImGuiIO &io2 = ImGui::GetIO();
    io2.IniFilename = nullptr;
    check("a missing TTF makes load_fonts return false",
          !asmdesk::load_fonts(io2, "/no/such/font.ttf", "/no/such/icons.ttf",
                               "/no/such/fa.ttf"));
    io2.Fonts->GetTexDataAsRGBA32(&px, &w, &h);
    check("the built-in bitmap font still builds after the degrade", w > 0);
    ImGui::DestroyContext();

    // 3) DPI-aware bake (20-workspace-and-settings.md T5): baking at a scaled px
    // rebuilds a non-empty atlas and advances the rebuild counter — the seam the
    // content-scale change (main.cpp) drives.
    ImGui::CreateContext();
    ImGuiIO &io3 = ImGui::GetIO();
    io3.IniFilename = nullptr;
    unsigned long before = asmdesk::fonts_rebuild_count();
    check("load_fonts at 2x content scale builds",
          asmdesk::load_fonts(io3, ASMTEST_JBM_TTF, ASMTEST_CODICON_TTF,
                              ASMTEST_FA_TTF, 15.0f, 2.0f));
    check("a content-scale change rebuilt the atlas",
          asmdesk::fonts_rebuild_count() > before);
    unsigned char *p3 = nullptr;
    int w3 = 0, h3 = 0;
    io3.Fonts->GetTexDataAsRGBA32(&p3, &w3, &h3);
    check("the scaled atlas builds non-empty", w3 > 0 && h3 > 0 && p3 != nullptr);
    ImGui::DestroyContext();

    if (fails) {
        std::fprintf(stderr, "test_fonts: %d FAILURE(S)\n", fails);
        return 1;
    }
    std::printf("test_fonts: all checks passed\n");
    return 0;
}
