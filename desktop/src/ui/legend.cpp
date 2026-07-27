// legend.cpp — the shared semantic-palette legend (see legend.h). Draws only.
#include "ui/legend.h"

#include "ui/theme.h"

namespace asmdesk {

void dt_legend_row(ImU32 col, const char *channel, const char *label) {
    ImVec4 sw = ImGui::ColorConvertU32ToFloat4(col);
    ImGui::ColorButton(
        label,
        sw, // the swatch: a fixed 12px square, no picker, no tooltip on it
        ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
        ImVec2(12, 12));
    ImGui::SameLine();
    // The SECOND CHANNEL token (T2): a monochrome reader reads the distinction
    // off THIS, not the swatch. Dimmed so it reads as an annotation.
    if (channel != nullptr && channel[0] != '\0') {
        ImGui::TextColored(dt_dim_col(), "[%s]", channel);
        ImGui::SameLine();
    }
    ImGui::TextUnformatted(label);
}

void dt_semantic_legend() {
    // Each row pairs the theme accessor with the second channel it is drawn with
    // elsewhere, so this legend is the single readable statement of the palette.
    dt_legend_row(dt_good_u32(), "yes", "good / attachable");
    dt_legend_row(dt_bad_u32(), "no", "bad / refused");
    dt_legend_row(dt_maybe_u32(), "?", "maybe / weak");
    dt_legend_row(dt_changed_u32(), "\xE2\x96\xB6", "changed this step");
    dt_legend_row(dt_selected_u32(), "pick", "selected / paired");
    dt_legend_row(dt_statistical_u32(), "stipple", "statistical (sampled)");
}

void dt_cone_legend() {
    dt_legend_row(dt_cone_back_u32(), "\xE2\x97\x84",
                  "back-cone (produced it)");
    dt_legend_row(dt_cone_fwd_u32(), "\xE2\x96\xBA", "fwd-cone (affects)");
    dt_legend_row(dt_cone_both_u32(), "\xE2\x97\x8F", "the selection");
    dt_legend_row(dt_cone_dim_u32(), "\xC2\xB7", "off-cone");
}

} // namespace asmdesk
