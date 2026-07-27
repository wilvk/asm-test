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

// The ONE encoding table (T2). Built in a function with a static local so the
// ImU32 packs happen at first call (ColorConvertFloat4ToU32 is context-free, so
// this is safe before any ImGui context). Legend and test both read it.
const dt_encoding *dt_encoding_table(int *n) {
    // Glyphs are UTF-8: back-cone inflow "\xE2\x97\x84", fwd-cone outflow
    // "\xE2\x96\xBA", both "\xE2\x97\x8F", off-cone "\xC2\xB7". They match the
    // glyphs the slice view draws beside each node (slice_view_draw.cpp), so the
    // legend and the node agree.
    static const dt_encoding table[] = {
        // key           colour                channel        cat   text  amber
        {"good", dt_good_u32(), "yes", true, true, false},
        {"bad", dt_bad_u32(), "no", true, true, false},
        {"maybe", dt_maybe_u32(), "?", true, true, false},
        {"changed", dt_changed_u32(), "\xE2\x96\xB6", true, true, false},
        {"selected", dt_selected_u32(), "pick", true, true, false},
        {"cone-back", dt_cone_back_u32(), "\xE2\x97\x84", true, false, false},
        {"cone-fwd", dt_cone_fwd_u32(), "\xE2\x96\xBA", true, false, false},
        {"cone-both", dt_cone_both_u32(), "\xE2\x97\x8F", true, false, false},
        {"cone-dim", dt_cone_dim_u32(), "\xC2\xB7", true, false, false},
        {"take-hot", dt_hot_u32(), "solid", true, false, false},
        {"take-dim", dt_dim_u32(), "hollow", true, false, false},
        {"statistical", dt_statistical_u32(), "stipple", true, false, true},
        {"warn", dt_warn_u32(), "banner", false, false, true},
    };
    if (n != nullptr)
        *n = static_cast<int>(sizeof(table) / sizeof(table[0]));
    return table;
}

void dt_semantic_legend() {
    // Rendered from the ONE encoding table, so this legend is the single
    // readable statement of the palette AND its second channels (T2).
    int n = 0;
    const dt_encoding *t = dt_encoding_table(&n);
    static const char *label[] = {
        "good / attachable",  "bad / refused",       "maybe / weak",
        "changed this step",  "selected / paired",   "back-cone (produced it)",
        "fwd-cone (affects)", "the selection",       "off-cone",
        "hot take (solid)",   "dim take (hollow)",   "statistical (sampled)",
        "caution (banner)"};
    for (int i = 0; i < n; i++)
        dt_legend_row(t[i].col, t[i].channel, label[i]);
}

void dt_cone_legend() {
    dt_legend_row(dt_cone_back_u32(), "\xE2\x97\x84",
                  "back-cone (produced it)");
    dt_legend_row(dt_cone_fwd_u32(), "\xE2\x96\xBA", "fwd-cone (affects)");
    dt_legend_row(dt_cone_both_u32(), "\xE2\x97\x8F", "the selection");
    dt_legend_row(dt_cone_dim_u32(), "\xC2\xB7", "off-cone");
}

void dt_loom_take_legend() {
    // The take axis reads by PATTERN first (T2): a monochrome reader tells hot
    // from dim from an unaligned tail without the hue.
    dt_legend_row(dt_hot_u32(), "solid", "hot take (used this step)");
    dt_legend_row(dt_dim_u32(), "hollow", "dim take (present, not used)");
    dt_legend_row(dt_statistical_u32(), "dashed",
                  "unaligned tail (not agreement)");
}

} // namespace asmdesk
