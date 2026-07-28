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

static unsigned long g_rebuilds = 0;

unsigned long fonts_rebuild_count() { return g_rebuilds; }

bool load_fonts(ImGuiIO &io, const char *jbm_ttf, const char *codicon_ttf,
                const char *fa_ttf, float base_px, float content_scale) {
    // No monospace TTF on disk -> keep the built-in bitmap font (honest degrade).
    if (!readable(jbm_ttf))
        return false;

    // The DPI-aware baked size (T5). Clamp defensively: a nonsensical scale from
    // a corrupt settings store must never bake a zero- or absurd-px atlas.
    float px = base_px * content_scale;
    if (!(px > 4.0f))
        px = base_px;
    if (px > 256.0f)
        px = 256.0f;

    io.Fonts->Clear();
    // The GLYPH RANGE the atlas rasterizes. Without this, AddFontFromFileTTF
    // defaults to GetGlyphRangesDefault() = Basic Latin + Latin-1 only
    // (U+0020..U+00FF), so every character the UI uses OUTSIDE that block — the
    // em-dash (U+2014, in >1000 strings), the ellipsis in "Open…"/"Browse…", the
    // "-> " arrow, the • bullet, the ▸/◄ menu/nav triangles, ∘/∩ set operators —
    // is never baked and renders as the fallback '?'. JetBrains Mono has all of
    // them; the range just has to ASK for them. Static const so the array outlives
    // the deferred atlas Build() (same discipline as the icon ranges below). The
    // blocks are deliberately whole (not a hand-picked codepoint list) so a future
    // string that reaches for standard punctuation renders too, never a new '?'.
    static const ImWchar text_ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement (§ ± · × ° …)
        0x2000, 0x206F, // General Punctuation: – — ‘ ’ “ ” … • † ‰
        0x2190, 0x21FF, // Arrows: → ← ↑ ↓ ↔
        0x2200, 0x22FF, // Mathematical Operators: ∘ ∩ ∪ ≈ ≠ ≤ ≥
        0x2500, 0x257F, // Box Drawing (defensive: any ASCII-art table borders)
        0x25A0, 0x25FF, // Geometric Shapes: ▸ ◄ ● ○ ■ ▲ ▼
        0x2700, 0x27BF, // Dingbats: ✕ ✓ ✗
        0,
    };
    io.Fonts->AddFontFromFileTTF(jbm_ttf, px, nullptr, text_ranges);

    // Merge each icon range onto the same font so ICON_* macros render inline in
    // labels. Each is optional (absent TTF -> no icons from that set). The range
    // arrays are static const so they outlive the deferred atlas Build().
    if (readable(codicon_ttf)) { // debugger icons (13 F3)
        static const ImWchar ci_range[] = {ICON_MIN_CI, ICON_MAX_CI, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = px; // keep icons monospace-aligned
        io.Fonts->AddFontFromFileTTF(codicon_ttf, px, &cfg, ci_range);
    }
    if (readable(fa_ttf)) { // FontAwesome, for ImGuiNotify toasts (16 T1)
        static const ImWchar fa_range[] = {ICON_MIN_FA, ICON_MAX_FA, 0};
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = px;
        io.Fonts->AddFontFromFileTTF(fa_ttf, px, &cfg, fa_range);
    }

    io.Fonts->Build();
    g_rebuilds++;
    return true;
}

} // namespace asmdesk
