// theme.h — the shared honesty-chrome palette (11-imgui-addons.md, quick win
// #6). The caution and refusal colours of plan D7's non-collapsible banners had
// drifted to three different amber values across five draw TUs (canvas_draw,
// fabric_imgui, hud, completeness, observer_draw); this is the ONE place they
// live, so a banner in one view can never render a different amber than the same
// banner in another. No addon and no engine dep — just imgui.h for the types.
#ifndef ASMDESK_UI_THEME_H
#define ASMDESK_UI_THEME_H

#include "imgui.h"

namespace asmdesk {

// The amber caution colour: every warning / truncation / statistical banner and
// every "skipped"/"redacted" placard. The canonical value is the one three of
// the five sites already used (0.95, 0.75, 0.25) — the byte form (242, 191, 64)
// the Loom fabric draws with, so dt_warn_u32() below round-trips to it exactly.
inline ImVec4 dt_warn_col() { return ImVec4(0.95f, 0.75f, 0.25f, 1.0f); }

// The red refusal colour: a hard refusal placard (a comparison that cannot be
// made, a basis mismatch, an armed watchpoint the engine declined).
inline ImVec4 dt_refuse_col() { return ImVec4(0.95f, 0.45f, 0.40f, 1.0f); }

// The ImDrawList (ImU32) forms, packed from the SAME floats so the draw-list
// views (the Loom fabric) can never drift from the TextColored ones.
// ColorConvertFloat4ToU32 is a pure bit-pack — it reads no ImGui context, so
// these are safe to call at static-init time as well as per-frame.
inline ImU32 dt_warn_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_warn_col());
}
inline ImU32 dt_refuse_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_refuse_col());
}

} // namespace asmdesk
#endif // ASMDESK_UI_THEME_H
