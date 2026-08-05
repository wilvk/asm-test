// timepos.cpp — the one time-position widget (see timepos.h).
#include "ui/timepos.h"
#include "ui/flow.h"

#include <cstring>

#include "ui/theme.h"

namespace asmdesk {

bool dt_timepos_scrub(const char *label, int *t, int total) {
    if (t == nullptr)
        return false;
    if (total < 0)
        total = 0;
    ImGui::SetNextItemWidth(200);
    bool moved = ImGui::SliderInt(label, t, 0, total);
    if (*t < 0)
        *t = 0;
    if (*t > total)
        *t = total;
    return moved;
}

bool dt_timepos_step(const char *label, int *idx, int count,
                     const char *reason) {
    if (idx == nullptr || count <= 0)
        return false;
    bool moved = false;
    ImGui::PushID(label);
    if (ImGui::Button("< previous") && *idx > 0) {
        (*idx)--;
        moved = true;
    }
    flow_same_line(flow_textf_w("%s #%d of %d", label, *idx + 1, count));
    ImGui::Text("%s #%d of %d", label, *idx + 1, count);
    flow_same_line(flow_button_w("next >"));
    if (ImGui::Button("next >") && *idx + 1 < count) {
        (*idx)++;
        moved = true;
    }
    // The always-visible "intentional discrete" marker (F16): a step glyph in
    // the caution amber, its verbatim reason on hover — so the discreteness
    // reads as a deliberate fidelity choice, never a missing scrubber.
    flow_same_line(flow_text_w("\xE2\x8F\xADstep"));
    ImGui::TextColored(dt_warn_col(), "\xE2\x8F\xADstep");
    if (reason != nullptr && reason[0] != '\0' && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.0f);
        ImGui::TextUnformatted(reason);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    ImGui::PopID();
    return moved;
}

const char *dt_timepos_discrete_reason(const char *surface) {
    if (surface == nullptr)
        return "";
    if (std::strcmp(surface, "invocations") == 0)
        return "Discrete paging, never a scrub: between two invocations the "
               "target ran unobserved for an unknown time, and a slider would "
               "draw that gap as elapsed captured time.";
    if (std::strcmp(surface, "disasm-logical-time") == 0)
        return "Logical time steps between recorded code-image versions, not a "
               "continuous clock: bytes resolve to the version with the "
               "greatest `when` <= this, because a JIT reuses addresses.";
    return "";
}

} // namespace asmdesk
