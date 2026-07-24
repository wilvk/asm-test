// inspect_door.cpp — the Inspect door's ImGui half (07-serve-live-host.md
// T4/T5). Same split as the other doors: everything decidable lives in the
// pure modules (live/inspect.h for attachability + evidence, live/budget.h for
// the jack), which test_inspect and test_budget drive headlessly. This file is
// the drawing and the wiring, and holds no rule of its own.
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "ui/doors.h"

namespace asmdesk {

void inspect_scan(InspectState &s) {
    s.rows = list_processes();
    s.scanned = true;
}

void inspect_connect(InspectState &s) {
    if (s.host_started)
        return;
    LiveSession::Spec spec;
    spec.asmspy_path = s.asmspy_path;
    spec.ssh_host = s.ssh_host;
    std::string err;
    s.host_started = s.session.start(spec, err);
    s.host_error = s.host_started ? std::string() : err;
}

bool inspect_request_start(InspectState &s) {
    s.swap_pending = false;
    BudgetDecision d = budget_can_start(s.want, s.active);
    if (!d.allowed) {
        // Do NOT fire the command. The serve loop would refuse it, but a UI
        // that lets the user pull a lever and then reports an error has taught
        // them nothing about why the lever was never available.
        s.swap_pending = true;
        s.swap_blocker = d.blocker;
        s.swap_reason = d.reason;
        return false;
    }
    s.session.send_start(mode_name(s.want), s.selected_pid);
    s.active.push_back(s.want);
    return true;
}

void inspect_confirm_swap(InspectState &s) {
    if (!s.swap_pending)
        return;
    // Stopping someone else's capture is a real consequence, which is why this
    // is a separate, explicit action rather than something inspect_request_start
    // does on the user's behalf.
    s.session.send_stop();
    s.active.clear();
    s.swap_pending = false;
    s.session.send_start(mode_name(s.want), s.selected_pid);
    s.active.push_back(s.want);
}

namespace {

const ImVec4 kGood(0.45f, 0.80f, 0.45f, 1.0f);
const ImVec4 kBad(0.90f, 0.45f, 0.40f, 1.0f);
const ImVec4 kMaybe(0.90f, 0.78f, 0.35f, 1.0f);

const char *verdict_word(Attach a) {
    return a == Attach::Yes ? "attachable"
                            : (a == Attach::No ? "NOT attachable" : "maybe");
}
ImVec4 verdict_colour(Attach a) {
    return a == Attach::Yes ? kGood : (a == Attach::No ? kBad : kMaybe);
}

// The live session's own status: what is running, what was refused, what was
// skipped. Refusals and skips are DIFFERENT things and are shown differently —
// a skip is a successful session that had nothing to report.
void draw_status(InspectState &s) {
    const LiveStatus &st = s.session.status();
    ImGui::SeparatorText("session");
    const char *state = st.state == LiveState::Running  ? "running"
                        : st.state == LiveState::Idle   ? "idle"
                        : st.state == LiveState::Failed ? "FAILED"
                                                        : "ended";
    ImGui::Text("host: %s  |  state: %s", st.command.c_str(), state);
    if (!st.fatal.empty())
        ImGui::TextColored(kBad, "%s", st.fatal.c_str());
    if (st.state == LiveState::Running)
        ImGui::Text("mode %s on pid %ld", st.mode.c_str(), st.pid);

    if (!st.last_err.empty())
        ImGui::TextColored(kBad, "refused: %s", st.last_err.c_str());
    if (st.skip_code) {
        // NOT an error colour: a skip means the tracer worked and there was
        // nothing to report, and the measured reason is the whole payload.
        ImGui::TextColored(kMaybe, "skipped (%d): %s", st.skip_code,
                           st.skip_reason.c_str());
    }
    if (st.paused_dropped)
        ImGui::TextColored(kMaybe,
                           "%llu event(s) dropped while paused — this "
                           "recording is truncated",
                           (unsigned long long)st.paused_dropped);
    if (s.session.malformed_lines())
        ImGui::TextColored(kMaybe, "%llu unparseable line(s) from the host",
                           (unsigned long long)s.session.malformed_lines());

    // What --auto chose, and on what evidence. The weaker label is not
    // optional chrome: without it the door implies it measured something it
    // did not.
    for (const LiveNote &n : s.session.notes()) {
        AutoPick p;
        if (n.kind != "session" || !parse_auto_pick(n.body, &p))
            continue;
        std::string walk = pick_walk_note(p);
        if (!walk.empty())
            ImGui::TextColored(kMaybe, "%s", walk.c_str());
        ImGui::TextColored(pick_is_weak_evidence(p) ? kMaybe : kGood, "%s",
                           pick_evidence_label(p).c_str());
    }

    size_t nrec = s.session.recordings().size();
    if (const Recording *g = s.session.growing())
        ImGui::Text("capturing: %llu event(s) so far",
                    (unsigned long long)g->event_count());
    if (nrec)
        ImGui::Text("%zu completed recording(s) this session", nrec);
    for (const Recording &r : s.session.recordings())
        if (r.torn)
            ImGui::TextColored(
                kBad, "TORN recording — the host stopped before "
                      "writing its footer; this capture is incomplete");
}

// The patch bay: one jack per target, and what is in it.
void draw_patch_bay(InspectState &s) {
    ImGui::SeparatorText("patch bay — one ptrace jack per target");
    for (LiveMode m : all_modes()) {
        if (ImGui::RadioButton(mode_name(m), s.want == m))
            s.want = m;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", mode_jack_reason(m));
        ImGui::SameLine();
    }
    ImGui::NewLine();

    BudgetDecision d = budget_can_start(s.want, s.active);
    if (!d.allowed) {
        ImGui::TextColored(kMaybe, "%s", budget_blocked_label(d).c_str());
        ImGui::TextWrapped("%s", d.reason.c_str());
    }

    const bool can = s.host_started && s.selected_pid > 0;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Start"))
        inspect_request_start(s);
    ImGui::EndDisabled();
    if (!can) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", !s.host_started ? "connect a serve host first"
                                                  : "select a process first");
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        s.session.send_stop();
        s.active.clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Pause"))
        s.session.send_pause(true);
    ImGui::SameLine();
    if (ImGui::Button("Resume"))
        s.session.send_pause(false);

    if (s.swap_pending) {
        ImGui::Separator();
        ImGui::TextColored(kMaybe,
                           "the %s view holds the tracer. Swapping STOPS it.",
                           mode_name(s.swap_blocker));
        if (ImGui::Button("Stop it and start this one"))
            inspect_confirm_swap(s);
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            s.swap_pending = false;
    }
}

} // namespace

void draw_inspect_door(InspectState &s) {
    // Pumped once per frame; non-blocking.
    s.session.poll();

    ImGui::TextUnformatted(
        "Inspect — attach to a running process. The capture host is the "
        "`asmspy --serve` subprocess, so nothing here links a tracer.");
    ImGui::Spacing();

    if (!s.host_started) {
        ImGui::InputText("asmspy path (blank = $PATH, then ./build/asmspy)",
                         s.asmspy_path, sizeof s.asmspy_path);
        ImGui::InputText("ssh host (blank = local)", s.ssh_host,
                         sizeof s.ssh_host);
        std::string found = resolve_asmspy_path();
        ImGui::TextDisabled("resolved: %s",
                            found.empty() ? "(none found)" : found.c_str());
        if (ImGui::Button("Connect"))
            inspect_connect(s);
        if (!s.host_error.empty())
            ImGui::TextColored(kBad, "%s", s.host_error.c_str());
    } else {
        draw_patch_bay(s);
        draw_status(s);
    }

    ImGui::SeparatorText("processes");
    if (!s.scanned)
        inspect_scan(s);
    if (ImGui::Button("Rescan"))
        inspect_scan(s);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu process(es); ptrace_scope=%d", s.rows.size(),
                        read_yama_scope());

    if (ImGui::BeginTable("procs", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("pid");
        ImGui::TableSetupColumn("comm");
        ImGui::TableSetupColumn("attach");
        ImGui::TableSetupColumn("why / remedy");
        ImGui::TableHeadersRow();
        for (const ProcRow &r : s.rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[64];
            std::snprintf(lbl, sizeof lbl, "%ld##p%ld", r.pid, r.pid);
            if (ImGui::Selectable(lbl, s.selected_pid == r.pid,
                                  ImGuiSelectableFlags_SpanAllColumns))
                s.selected_pid = r.pid;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.comm.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(verdict_colour(r.verdict.verdict), "%s",
                               verdict_word(r.verdict.verdict));
            ImGui::TableNextColumn();
            // The reason is FIRST-CLASS, in the row — not a toast that
            // disappears and not an error the user has to provoke. That is the
            // door's whole promise: see why not.
            ImGui::TextUnformatted(r.verdict.why.c_str());
            if (!r.verdict.remedy.empty())
                ImGui::TextDisabled("-> %s", r.verdict.remedy.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace asmdesk
