// diff_view_draw.cpp — the ImGui half of the diff summary panel. Draws only.
#include "imgui.h"

#include "nav.h"
#include "views/views_draw.h"

namespace asmdesk {

void draw_diff_view(const dt_diff_view &v) {
    if (!v.refusal.empty()) {
        draw_banner(v.refusal.c_str(), true);
        return;
    }
    for (const dt_diff_row &r : v.rows) {
        switch (r.kind) {
        case dt_diff_row_kind::header:
            ImGui::SeparatorText(r.text.c_str());
            continue;
        case dt_diff_row_kind::note:
            ImGui::TextDisabled("%s", r.text.c_str());
            continue;
        case dt_diff_row_kind::divergence:
            ImGui::SeparatorText("divergence");
            break;
        default:
            break;
        }
        if (r.statistical) {
            // The provenance chip. A sampled edge count sits beside exact
            // heat counts in this panel and must never be mistaken for one.
            ImGui::TextDisabled("[statistical]");
            ImGui::SameLine();
        }
        if (r.link.empty()) {
            ImGui::TextWrapped("%s", r.text.c_str());
            continue;
        }
        // Every navigable row is a button that goes through the router — no
        // view-private navigation state (plan D4).
        ImGui::PushID(&r);
        if (ImGui::SmallButton("go"))
            ImGui::SetClipboardText(r.link.c_str());
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", r.text.c_str());
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", r.link.c_str());
    }
}

void draw_bindings_help() {
    if (!ImGui::BeginTable("bindings", 2, ImGuiTableFlags_RowBg))
        return;
    for (const dt_binding &b : dt_nav_bindings()) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b.keys);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b.what);
    }
    ImGui::EndTable();
}

} // namespace asmdesk
