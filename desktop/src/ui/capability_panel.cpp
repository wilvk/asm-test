// capability_panel.cpp — "what can this host do, and why not?"
// (docs/internal/gui/06-doors-and-learning.md T6). Draws and PROBES ONCE; every
// rule lives in capview.h, which test_capview drives on synthetic data.
//
// In the render-only viewer this file still compiles but probes nothing: a
// viewer that ran a capability sweep would be reporting on the machine it is
// running on, not the machine the loaded recording came from — so it shows the
// RECORDING's provenance and says so.
#include "imgui.h"

#include <climits>
#include <cstring>

#include "capview.h"
#include "ui/doors.h"
#include "views/views_draw.h"

// The probe compiles only where the capability objects are on the link line.
// The full app defines it; the render-only viewer and the headless tests do
// not, and both say so on screen rather than showing an empty deck.
#ifdef ASMTEST_DESKTOP_CAN_PROBE
extern "C" {
#include "asmtest_ibs.h"
}
#endif

namespace asmdesk {

void cap_probe(CapState &s) {
    s.probed = true;
    s.rows.clear();
#ifndef ASMTEST_DESKTOP_CAN_PROBE
    (void)0;
#else
    // ONE probe, at open (and on an explicit Refresh). The GUI never re-derives
    // any of this: every string below is the library's own.
    asmtest_trace_choice_t cascade[8];
    std::memset(cascade, 0, sizeof cascade);
    int policy = s.native_only ? ASMTEST_TRACE_NATIVE_ONLY : ASMTEST_TRACE_BEST;
    int n = asmtest_trace_resolve(policy, cascade,
                                  (int)(sizeof cascade / sizeof cascade[0]));
    if (n < 0)
        n = 0;

    asmtest_hwtrace_status_t st[4];
    for (int b = 0; b < 4; b++) {
        std::memset(&st[b], 0, sizeof st[b]);
        st[b].perf_event_paranoid = INT_MIN;
        asmtest_hwtrace_status((asmtest_trace_backend_t)b, &st[b]);
    }

    s.rows = capview_build(cascade, (size_t)n, st, asmtest_ibs_available(),
                           asmtest_ibs_skip_reason(),
                           asmtest_ibs_unavail_reason(), s.native_only);
#endif
}

void draw_capability_panel(CapState &s, const Recording *loaded) {
#ifndef ASMTEST_DESKTOP_CAN_PROBE
    (void)s;
    draw_banner(kCapViewerNoProbe, false);
    if (loaded == nullptr) {
        ImGui::TextDisabled("no recording open");
        return;
    }
    ImGui::Text("backend:  %s", loaded->provenance.backend.c_str());
    ImGui::Text("exact:    %s", loaded->provenance.exact ? "yes" : "no");
    ImGui::Text("trust:    %s", loaded->provenance.trust.c_str());
    ImGui::Text("arch:     %s", loaded->arch.c_str());
    if (loaded->truncated())
        ImGui::TextDisabled("this recording is truncated");
    return;
#else
    (void)loaded;
    if (!s.probed)
        cap_probe(s);

    if (ImGui::Checkbox("native only", &s.native_only))
        cap_probe(s); // re-resolve under ASMTEST_TRACE_NATIVE_ONLY
    ImGui::SameLine();
    if (ImGui::SmallButton("refresh"))
        cap_probe(s);
    ImGui::SameLine();
    ImGui::TextDisabled("probed once at open; the GUI never re-derives these");
    ImGui::Separator();

    bool drew_line = false;
    for (const cap_row &r : s.rows) {
        // The rule goes in exactly once, before the first row that crosses it.
        if (r.below_fidelity_line && !drew_line) {
            drew_line = true;
            ImGui::Separator();
            ImGui::TextDisabled("%s", kCapFidelityLine);
        }
        if (r.kind == cap_kind::refusal) {
            draw_banner(r.reason.c_str(), true);
            continue;
        }
        if (!r.available)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1));
        ImGui::Text("%s %s", r.available ? "[ok]  " : "[grey]",
                    r.label.c_str());
        if (!r.available)
            ImGui::PopStyleColor();
        if (!r.chip.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", r.chip.c_str());
        }
        // UI LAW 1: the machine reason, VERBATIM and wrapped — never clipped,
        // never paraphrased. It is what tells a user whether to change a
        // sysctl, install a package, or buy a different CPU.
        if (!r.reason.empty()) {
            ImGui::Indent();
            ImGui::TextWrapped("%s", r.reason.c_str());
            ImGui::Unindent();
        }
    }
#endif
}

} // namespace asmdesk
