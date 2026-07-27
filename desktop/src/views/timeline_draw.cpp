// timeline_draw.cpp — the ImGui half of the operand timeline. Draws only.
#include "imgui.h"

#include "views/views_draw.h"

namespace asmdesk {

void draw_timeline(const dt_timeline &t) {
    draw_banner(t.banner.c_str(), false);
    if (!ImGui::BeginTable("timeline", 5,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("step", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("instruction");
    ImGui::TableSetupColumn("values");
    ImGui::TableSetupColumn("in/out", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    bool drew_separator = false;
    for (const dt_timeline_row &r : t.rows) {
        // The unaligned treatment: one visible break where the two runs stop
        // being comparable. Everything below it is A's alone.
        if (r.unaligned && !drew_separator) {
            drew_separator = true;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("- - -");
            ImGui::TableNextColumn();
            ImGui::TextDisabled(
                "the two recordings diverge here; rows below are A only");
        }
        ImGui::TableNextRow();
        // 22 T1: the brushed entity is cross-highlighted in place (the same entity
        // the other panes brush). 22 T3: every global-find hit is MARKED — find
        // measures, never hides, so the row stays and is merely lit. Selection wins
        // over a find tint when a row is both.
        const bool is_sel = t.selected_step && *t.selected_step == r.step;
        bool is_hit = false;
        for (uint32_t h : t.find_hits)
            if (h == r.step) {
                is_hit = true;
                break;
            }
        if (is_sel)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(ImGuiCol_Header));
        else if (is_hit)
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(120, 100, 0, 90));
        const bool dim = r.style == dt_rowstyle::dimmed || r.missing;
        if (dim)
            ImGui::BeginDisabled();
        ImGui::TableNextColumn();
        ImGui::Text("%u", r.step);
        ImGui::TableNextColumn();
        if (r.missing)
            ImGui::TextUnformatted("(unknown)");
        else
            ImGui::Text("0x%llx", (unsigned long long)r.off);
        ImGui::TableNextColumn();
        if (r.missing)
            // A dropped step is UNKNOWN, not an instruction at offset 0.
            ImGui::TextUnformatted(
                "(no df_step event - this step was dropped)");
        else if (r.disasm.empty())
            ImGui::Text("0x%llx", (unsigned long long)r.off);
        else
            ImGui::TextUnformatted(r.disasm.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.ann.c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%zu/%zu", r.n_in, r.n_out);
        if (dim)
            ImGui::EndDisabled();
    }
    ImGui::EndTable();
}

} // namespace asmdesk
