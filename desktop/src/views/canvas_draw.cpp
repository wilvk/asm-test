// canvas_draw.cpp — the ImGui half of the trace canvas + the ONE graded honesty
// vocabulary's draw half (23-graded-truth-layer.md T1) + the uniform busy signal
// (T4). Draws only; every grading rule is pure in ui/honesty.h / ui/progress.h.
#include <cfloat>

#include "imgui.h"

#include "IconsCodicons.h" // the ONE glyph set per tier (doc 13 F3 Codicons)
#include "ui/honesty.h"
#include "ui/progress.h"
#include "ui/theme.h"
#include "views/views_draw.h"

namespace asmdesk {

// Warn / refusal colours from the shared honesty-chrome palette (ui/theme.h) so
// every placard in the app looks the same and none can be mistaken for chrome.
static const ImVec4 kWarn = dt_warn_col();
static const ImVec4 kRefuse = dt_refuse_col();

namespace {
// The ONE tier -> colour map (doc 24 T5.1's semantic accessors; NO new literals
// here — F14/T5.1 owns that axis). Neutral is the quiet "maybe" tint (not the
// caution amber), caution is the amber, integrity is the refusal red.
ImVec4 tier_col(HonestyTier t) {
    switch (t) {
    case HonestyTier::Neutral:
        return dt_maybe_col();
    case HonestyTier::Caution:
        return dt_warn_col();
    case HonestyTier::Integrity:
        return dt_refuse_col();
    }
    return dt_maybe_col();
}

// The ONE tier -> glyph map: a Codicon per tier so the tier reads without colour
// alone (24 T5.2's second channel). info / warning / error, the three the whole
// app already understands.
const char *tier_glyph(HonestyTier t) {
    switch (t) {
    case HonestyTier::Neutral:
        return ICON_CI_INFO;
    case HonestyTier::Caution:
        return ICON_CI_WARNING;
    case HonestyTier::Integrity:
        return ICON_CI_ERROR;
    }
    return ICON_CI_INFO;
}
} // namespace

void draw_honesty_banner(const char *text, HonestyTier tier, bool *collapsed) {
    if (text == nullptr || *text == '\0')
        return;
    // Integrity NEVER collapses — the one signal that means "do not trust the
    // tail of this data" cannot be dismissed while the numbers below are wrong.
    const bool may_collapse = tier != HonestyTier::Integrity && collapsed;
    ImGui::PushStyleColor(ImGuiCol_Text, tier_col(tier));
    if (may_collapse && *collapsed) {
        // The collapsed caution form IS the chip — same text, never gone (D7).
        ImGui::Text("%s %s", tier_glyph(tier), text);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton("show"))
            *collapsed = false;
        return;
    }
    ImGui::Text("%s", tier_glyph(tier));
    ImGui::SameLine();
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    if (may_collapse) {
        if (ImGui::SmallButton("collapse"))
            *collapsed = true;
    }
    ImGui::Separator();
}

void draw_honesty_chip(const char *text, HonestyTier tier) {
    if (text == nullptr || *text == '\0')
        return;
    // A chip is inline and quiet: a glyph + the text, no separator, no wrap-to-
    // full-width. The placement contract (a header calls this) keeps a T3
    // integrity signal from ever masquerading as a quiet chip.
    ImGui::PushStyleColor(ImGuiCol_Text, tier_col(tier));
    ImGui::Text("%s %s", tier_glyph(tier), text);
    ImGui::PopStyleColor();
}

void draw_banner(const char *text, bool refusal) {
    // The thin shim (T1 step 3): refusal -> integrity (loud, non-collapsible),
    // else -> caution (amber). The ten legacy sites route through here unchanged.
    draw_honesty_banner(text, refusal ? HonestyTier::Integrity
                                      : HonestyTier::Caution);
}

void draw_progress(LongOp &op) {
    if (op.mode() == ProgressMode::Hidden)
        return;
    ImGui::PushID(&op);
    ImGui::TextUnformatted(op.label);
    ImGui::SameLine();
    if (op.mode() == ProgressMode::Determinate)
        // An HONEST fraction: only when a real total exists (progress.h's rule).
        ImGui::ProgressBar(op.fraction(), ImVec2(-FLT_MIN, 0));
    else
        // No honest total -> the indeterminate spinner/bar, never a fake %.
        ImGui::ProgressBar(-1.0f * (float)op.now, ImVec2(-FLT_MIN, 0),
                           "working");
    ImGui::Text("elapsed %.1fs", op.elapsed());
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel"))
        op.request_cancel();
    ImGui::PopID();
}

void draw_canvas(const dt_canvas &c) {
    if (!c.basis_error.empty()) {
        // A full-pane placard and NOTHING else: there are no rows, by design.
        draw_banner(c.basis_error.c_str(), true);
        return;
    }
    draw_banner(c.banner.c_str(), false);
    ImGui::Text("basis: %s", c.basis.empty() ? "(none)" : c.basis.c_str());

    const int cols = c.two_up ? 6 : 5;
    if (!ImGui::BeginTable("canvas", cols,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("cov", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn(c.two_up ? "heat A" : "heat",
                            ImGuiTableColumnFlags_WidthFixed);
    if (c.two_up)
        ImGui::TableSetupColumn("heat B", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("blk", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("disassembly");
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (const dt_canvas_row &r : c.rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (c.two_up)
            ImGui::TextUnformatted(
                r.in_a && r.in_b ? "AB"
                                 : (r.in_a ? "A " : (r.in_b ? " B" : "  ")));
        else
            ImGui::TextUnformatted(r.covered ? "C" : " ");

        ImGui::TableNextColumn();
        bool zero = c.div_off && *c.div_off == r.off;
        if (zero)
            ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::Text("0x%llx", (unsigned long long)r.off);
        if (zero) {
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted("<- patient zero");
        }

        ImGui::TableNextColumn();
        ImGui::Text("%u", r.heat);
        if (c.two_up) {
            ImGui::TableNextColumn();
            ImGui::Text("%u", r.heat_b);
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.block_start ? "B" : " ");
        ImGui::TableNextColumn();
        if (r.disasm.empty())
            // D10 absence: dimmed, and never invented.
            ImGui::TextDisabled("(no recorded disassembly)");
        else
            ImGui::TextUnformatted(r.disasm.c_str());
    }
    ImGui::EndTable();
}

} // namespace asmdesk
