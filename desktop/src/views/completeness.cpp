// completeness.cpp — the ImGui half of the backend-completeness panel
// (docs/internal/gui/02-exporters-and-readers.md T6). Draws only; every rule
// lives in views/completeness_model.h and is tested there.
//
// The panel NEVER probes. It renders a committed box record or a features file
// the user chose — running a live sweep is `make features`, and a view that
// silently ran one would be reporting on a different machine than the one whose
// row it is drawn under.
#include <string>

#include "imgui.h"

#include "views/completeness.h"
#include "views/views_draw.h"

namespace asmdesk {

namespace {
const ImVec4 kWarnCell(0.95f, 0.75f, 0.25f, 1.0f);
} // namespace

void draw_completeness(CompletenessState &s, const std::string &repo_root) {
    if (!s.scanned) {
        s.boxes = data::scan_boxes(repo_root);
        s.scanned = true;
    }
    if (ImGui::Button("rescan")) {
        s.boxes = data::scan_boxes(repo_root);
        s.error.clear();
    }
    ImGui::SameLine();
    const char *preview =
        s.selected >= 0 && s.selected < static_cast<int>(s.boxes.size())
            ? s.boxes[s.selected].box_id.c_str()
            : "(pick a committed box)";
    if (ImGui::BeginCombo("box", preview)) {
        for (int i = 0; i < static_cast<int>(s.boxes.size()); i++) {
            if (!s.boxes[i].has_features)
                continue;
            if (ImGui::Selectable(s.boxes[i].box_id.c_str(), s.selected == i)) {
                s.selected = i;
                try {
                    s.table = build_completeness(data::load_features_file(
                        s.boxes[i].dir + "/features.json"));
                    s.error.clear();
                } catch (const std::exception &e) {
                    // A load failure is shown verbatim, not swallowed into an
                    // empty table that would read as "this box has no backends".
                    s.error = e.what();
                    s.table = CompletenessTable{};
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputText("features.json", s.custom_path, sizeof s.custom_path);
    ImGui::SameLine();
    if (ImGui::Button("load")) {
        try {
            s.table =
                build_completeness(data::load_features_file(s.custom_path));
            s.error.clear();
            s.selected = -1;
        } catch (const std::exception &e) {
            s.error = e.what();
            s.table = CompletenessTable{};
        }
    }
    if (!s.error.empty())
        draw_banner(s.error.c_str(), true);
    if (s.table.rows.empty()) {
        ImGui::TextDisabled("no capability sweep loaded — pick a committed box "
                            "above, or point at an asmfeatures output. This "
                            "panel never runs a sweep itself.");
        return;
    }

    ImGui::SeparatorText(s.table.box_label.c_str());
    ImGui::Text("%zu rows, %zu available, %zu measured, %zu complete, %zu "
                "truncated",
                s.table.n_rows, s.table.n_available, s.table.n_measured,
                s.table.n_complete, s.table.n_truncated);

    if (!ImGui::BeginTable("completeness", 7,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_ScrollY))
        return;
    ImGui::TableSetupColumn("Tier", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Backend", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Arch", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Completeness", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();

    for (const CompletenessRow &r : s.table.rows) {
        ImGui::TableNextRow();
        // An unavailable row is greyed — but its REASON stays fully readable.
        if (!r.available)
            ImGui::BeginDisabled();
        const char *cols[] = {r.tier.c_str(), r.backend.c_str(), r.arch.c_str(),
                              r.scope.c_str()};
        for (const char *c : cols) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(c);
        }
        ImGui::TableNextColumn();
        if (r.truncated) {
            ImGui::PushStyleColor(ImGuiCol_Text, kWarnCell);
            ImGui::TextUnformatted(r.completeness.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(r.completeness.c_str());
        }
        if (!r.available)
            ImGui::EndDisabled();

        ImGui::TableNextColumn();
        // WRAPPED, never clipped: the skip_reason is the answer, and half of
        // it is not an answer.
        ImGui::TextWrapped("%s", r.status.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(r.note.c_str());
    }
    ImGui::EndTable();
}

} // namespace asmdesk
