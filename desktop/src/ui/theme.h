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

// ---------------------------------------------------------------------------
// The full SEMANTIC axis (24-one-visual-language.md T1 / F14). Before this, the
// good/bad/maybe/changed/cone/selected axis was re-invented inline in every draw
// TU — three barely-distinguishable yellows meant three different things and two
// reds both meant "refused". This is now the ONE place each of those meanings
// lives, so a colour can never drift its meaning between panes. Each accessor
// documents its ONE meaning; a later graded-honesty change (23 T4.1) re-tiers
// these values in ONE place instead of hunting call sites. Doc-comment discipline
// (one meaning per colour) is the F14 rule made explicit.

// verdict-YES / attachable / agreement. (inspect_door's old kGood.)
inline ImVec4 dt_good_col() { return ImVec4(0.45f, 0.80f, 0.45f, 1.0f); }

// verdict-NO / refused / a hard "cannot". Deliberately the SAME value as
// dt_refuse_col() — the old inspect_door kBad (0.90) and the refuse red (0.95)
// were two copies of one meaning; this kills the 0.90-vs-0.95 split (F14).
inline ImVec4 dt_bad_col() { return dt_refuse_col(); }

// weak / uncertain / "maybe" verdict — evidence that does not commit either way.
inline ImVec4 dt_maybe_col() { return ImVec4(0.90f, 0.78f, 0.35f, 1.0f); }

// a per-step register DELTA — the value this instruction changed. (The
// scrubber/abixray "changed" yellow; distinct from the caution amber.)
inline ImVec4 dt_changed_col() { return ImVec4(1.0f, 0.85f, 0.3f, 1.0f); }

// the four dependency-CONE hues (slice explorer). back = what produced the
// value, fwd = what it goes on to affect, both = the selection itself, dim =
// off-cone. Byte-fraction floats so the u32 forms below round-trip to the exact
// bytes the slice view drew before (IM_COL32(120,170,255) …).
inline ImVec4 dt_cone_back_col() {
    return ImVec4(120.0f / 255.0f, 170.0f / 255.0f, 255.0f / 255.0f, 1.0f);
}
inline ImVec4 dt_cone_fwd_col() {
    return ImVec4(255.0f / 255.0f, 180.0f / 255.0f, 110.0f / 255.0f, 1.0f);
}
inline ImVec4 dt_cone_both_col() { return ImVec4(1.0f, 1.0f, 1.0f, 1.0f); }
inline ImVec4 dt_cone_dim_col() {
    return ImVec4(110.0f / 255.0f, 110.0f / 255.0f, 110.0f / 255.0f, 1.0f);
}

// a selection / pairing highlight — the thing the user picked or the row a pair
// is marshalled against (abixray's cross-convention contrast). One shared blue,
// not a fourth bespoke tint per view.
inline ImVec4 dt_selected_col() { return ImVec4(0.45f, 0.85f, 1.0f, 1.0f); }

// a dimmed / secondary label (the 3D HUD's asides). Not a warning — just quiet.
inline ImVec4 dt_dim_col() { return ImVec4(0.65f, 0.65f, 0.70f, 1.0f); }

// HOT — a Loom "used this step" take and the patient-zero worldline (05/24 T2).
// The exact value the Loom fabric drew inline (IM_COL32(230,110,90)); shared so
// hot reads the same in the take legend as on the fabric. Its second channel is
// the SOLID fill (dim takes are hollow), so hot never rides on colour alone.
inline ImVec4 dt_hot_col() {
    return ImVec4(230.0f / 255.0f, 110.0f / 255.0f, 90.0f / 255.0f, 1.0f);
}

// STATISTICAL / sampled provenance — today the caution amber (it IS a caveat),
// but NAMED so 23 T4.1 can move statistical off the caution amber without
// touching a single call site. Always paired with a second channel (the
// stipple/dashed pattern) so it never rides on colour alone (T2).
inline ImVec4 dt_statistical_col() { return dt_warn_col(); }

// ---------------------------------------------------------------------------
// Contrast & CVD gate metadata (24-one-visual-language.md T2 / F15).
//
// The canonical panel background the contrast gate measures every semantic text
// colour against — ImGui's default dark WindowBg, opaque. `theme.h` stays the
// ONE place this value lives so the T2 contrast test and the running app agree
// on what "against the panel" means.
inline ImVec4 dt_panel_bg_col() { return ImVec4(0.06f, 0.06f, 0.06f, 1.0f); }

// The caution amber (dt_warn / dt_statistical) is LARGE-TEXT-ONLY: it clears the
// 3:1 large-text / graphical-object contrast bar but is confined to headers and
// non-collapsible banners (>= the large-text size), never small body rows. Where
// amber must annotate a small row, that row also carries the text token (T2's
// second-channel rule), so the meaning never rides on the amber alone. This is a
// POLICY marker the T2 test asserts exists — the palette states its own limit.
inline bool dt_warn_large_text_only() { return true; }

// The ImDrawList (ImU32) forms, packed from the SAME floats so the draw-list
// views (the Loom fabric, the slice cones) can never drift from the TextColored
// ones. ColorConvertFloat4ToU32 is a pure bit-pack — it reads no ImGui context,
// so these are safe to call at static-init time as well as per-frame.
inline ImU32 dt_warn_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_warn_col());
}
inline ImU32 dt_refuse_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_refuse_col());
}
inline ImU32 dt_good_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_good_col());
}
inline ImU32 dt_bad_u32() { return dt_refuse_u32(); }
inline ImU32 dt_maybe_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_maybe_col());
}
inline ImU32 dt_changed_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_changed_col());
}
inline ImU32 dt_cone_back_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_cone_back_col());
}
inline ImU32 dt_cone_fwd_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_cone_fwd_col());
}
inline ImU32 dt_cone_both_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_cone_both_col());
}
inline ImU32 dt_cone_dim_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_cone_dim_col());
}
inline ImU32 dt_selected_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_selected_col());
}
inline ImU32 dt_dim_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_dim_col());
}
inline ImU32 dt_hot_u32() {
    return ImGui::ColorConvertFloat4ToU32(dt_hot_col());
}
inline ImU32 dt_statistical_u32() { return dt_warn_u32(); }

} // namespace asmdesk
#endif // ASMDESK_UI_THEME_H
