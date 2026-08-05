// capability_panel.cpp — "what can this host do, and why not?"
// (docs/internal/archive/gui/06-doors-and-learning.md T6). Draws and PROBES ONCE; every
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
#include "ui/flow.h"
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
    flow_same_line(flow_small_button_w("refresh"));
    if (ImGui::SmallButton("refresh"))
        cap_probe(s);
    flow_same_line(
        flow_text_w("probed once at open; the GUI never re-derives these"));
    ImGui::TextDisabled("probed once at open; the GUI never re-derives these");

    // Lead with the POSITIVES (T4, F19): a one-line summary of what the host CAN
    // do, plus the Learn/Author floor, so a bare host no longer reads as "the
    // tool does not work here". Composed by the pure resolver from the same rows.
    ImGui::Separator();
    ImGui::TextWrapped("%s", capview_summary(s.rows).c_str());

    // Which arches Author mode can actually RUN (not just assemble), read
    // straight from author_arch_table() — the SAME single source of truth
    // gating the door's own Run button (32-per-guest-value-producer.md R5 T3)
    // — so this line can never go stale: it names arm64 automatically now
    // that the per-guest value-fabric producer backs it, with no separate
    // capability probe to keep in sync.
    {
        std::string names;
        for (const author_arch_row &row : author_arch_table())
            if (row.can_run)
                names += (names.empty() ? "" : ", ") + std::string(row.name);
        ImGui::TextWrapped("Author mode runs/traces: %s", names.c_str());
    }
    ImGui::Separator();

    // Available backends stay above the fold. The refusal (native-only empty) is
    // the answer to the panel's own question, so it stays visible too.
    bool drew_line = false;
    bool any_negative = false;
    for (const cap_row &r : s.rows) {
        if (r.kind == cap_kind::refusal) {
            draw_banner(r.reason.c_str(), true);
            continue;
        }
        if (!r.available) {
            any_negative = true;
            continue; // demoted below, under the expander
        }
        if (r.below_fidelity_line && !drew_line) {
            drew_line = true;
            ImGui::Separator();
            ImGui::TextDisabled("%s", kCapFidelityLine);
        }
        ImGui::Text("[ok]   %s", r.label.c_str());
        if (!r.chip.empty()) {
            flow_same_line(flow_textf_w("(%s)", r.chip.c_str()));
            ImGui::TextDisabled("(%s)", r.chip.c_str());
        }
    }

    // Demote the negatives under an expander, COLLAPSED by default (T4). The
    // verbatim machine reason (UI LAW 1) is preserved under each row — this is a
    // restructure of the fidelity chrome, not a removal (D7) — and a recognised
    // condition also gets the shared attach_verdict remedy as a next step.
    if (any_negative && ImGui::CollapsingHeader(kCapWhyNotHeader)) {
        for (const cap_row &r : s.rows) {
            if (r.kind == cap_kind::refusal || r.available)
                continue;
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.65f, 1));
            ImGui::Text("[grey] %s", r.label.c_str());
            ImGui::PopStyleColor();
            if (!r.chip.empty()) {
                flow_same_line(flow_textf_w("(%s)", r.chip.c_str()));
                ImGui::TextDisabled("(%s)", r.chip.c_str());
            }
            if (!r.reason.empty()) {
                ImGui::Indent();
                ImGui::TextWrapped("%s", r.reason.c_str());
                std::string remedy = capview_remedy(r);
                if (!remedy.empty())
                    ImGui::TextDisabled("-> %s", remedy.c_str());
                // A perf_event_paranoid / ptrace_scope remedy has a one-line fix;
                // offer it copy-pasteable under the greyed backend it unblocks.
                draw_command_hint(r.label.c_str(), remedy_command(remedy));
                ImGui::Unindent();
            }
        }
    }
#endif
}

} // namespace asmdesk
