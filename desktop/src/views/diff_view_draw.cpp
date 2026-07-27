// diff_view_draw.cpp — the ImGui half of the diff summary panel. Draws only.
#include "imgui.h"

#include "nav.h"
#include "views/views_draw.h"

namespace asmdesk {

void draw_diff_view(const dt_diff_view &v,
                    const std::function<void(const dt_link &)> &go) {
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
        // view-private navigation state (plan D4). The row already carries the
        // canonical textual link (dt_nav_format); parse it back and hand the
        // dt_link to the shell's go seam, the same path the Observer deck uses.
        ImGui::PushID(&r);
        if (ImGui::SmallButton("go") && go) {
            dt_link l;
            std::string err;
            if (dt_nav_parse(r.link, l, err))
                go(l);
        }
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
    // Generated ENTIRELY from dt_nav_bindings(), so help and behaviour cannot
    // drift (18-breach-stops.md T1). A wired binding renders normally; an
    // unwired one is greyed and tagged "planned" — the app's own
    // greyed-shows-why law, so the overlay never advertises a dead key as live.
    for (const dt_binding &b : dt_nav_bindings()) {
        ImGui::TableNextRow();
        if (!b.wired)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b.keys);
        ImGui::TableNextColumn();
        if (b.wired) {
            ImGui::TextUnformatted(b.what);
        } else {
            ImGui::Text("%s  (planned)", b.what);
            ImGui::PopStyleColor();
        }
    }
    ImGui::EndTable();
}

} // namespace asmdesk
