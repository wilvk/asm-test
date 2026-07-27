// primer.cpp — the first-open primer (see primer.h). Draw + pure predicates.
#include "ui/primer.h"

#include "ui/theme.h"

namespace asmdesk {

bool dt_primer_active(const dt_primer_state &st) { return !st.dismissed; }
void dt_primer_dismiss(dt_primer_state &st) { st.dismissed = true; }
void dt_primer_reopen(dt_primer_state &st) { st.dismissed = false; }
bool dt_primer_lean(const dt_primer_state &st) { return !st.dismissed; }

bool dt_primer(const char *id, const char *title, const char *body,
               const std::function<void()> &legend, dt_primer_state &st) {
    st.ever_shown = true;
    if (st.dismissed)
        return false;

    // An in-canvas card over the view's own draw area (not a modal): a bracketed
    // block at the top of the view, so it reads as an overlay while the view
    // stays behind it. Kept to stable ImGui API (no ChildFlags churn) so it
    // links identically into the app, the viewer and the null test backend.
    ImGui::PushID(id);
    ImGui::Separator();
    ImGui::TextUnformatted(title);
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
    ImGui::TextUnformatted(body);
    ImGui::PopTextWrapPos();
    if (legend) {
        ImGui::Spacing();
        legend();
    }
    ImGui::Spacing();
    if (ImGui::Button("Got it"))
        dt_primer_dismiss(st);
    ImGui::SameLine();
    ImGui::TextColored(dt_dim_col(),
                       "(re-open any time from the \"?\" in this view's header)");
    ImGui::Separator();
    ImGui::PopID();
    return true;
}

} // namespace asmdesk
