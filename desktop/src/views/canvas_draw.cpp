// canvas_draw.cpp — the ImGui half of the trace canvas. Draws only.
#include "imgui.h"

#include "views/views_draw.h"

namespace asmdesk {

// Warn / refusal colours, kept in one place so every honesty placard in the app
// looks the same and none of them can be mistaken for ordinary chrome.
static const ImVec4 kWarn(0.95f, 0.75f, 0.25f, 1.0f);
static const ImVec4 kRefuse(0.95f, 0.45f, 0.40f, 1.0f);

void draw_banner(const char *text, bool refusal) {
    if (text == nullptr || *text == '\0')
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, refusal ? kRefuse : kWarn);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::Separator();
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
