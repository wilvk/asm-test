// inspect_door.cpp — the Inspect door's ImGui half (07-serve-live-host.md
// T4/T5). Same split as the other doors: everything decidable lives in the
// pure modules (live/inspect.h for attachability + evidence, live/budget.h for
// the jack), which test_inspect and test_budget drive headlessly. This file is
// the drawing and the wiring, and holds no rule of its own.
#include <cstdio>
#include <cstring>

#include "imgui.h"

#include "ui/doors.h"
#include "views/views_draw.h"

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

// The live views over whatever this session has produced: the recording still
// growing, or the last completed one. Rebuilt only when the event count moves —
// the builders are pure and cheap, but per-frame work on a live stream is how a
// UI starts costing the thing it is watching.
void draw_live_views(InspectState &s) {
    const Recording *live = s.session.growing();
    const std::vector<Recording> &done = s.session.recordings();
    if (live == nullptr && !done.empty())
        live = &done.back();
    if (live == nullptr) {
        ImGui::TextDisabled(
            "no capture yet — start a mode above and its view appears here");
        return;
    }
    uint64_t n = live->event_count();
    if (!s.observer.built || n != s.observed_events ||
        done.size() != s.observed_recordings) {
        // The lifecycle a live host keeps OUTSIDE the recording (07-T3): the
        // views need the `started` params echo and the skip, and a live session
        // is the one place they are not inline.
        std::vector<nlohmann::json> bodies;
        for (const LiveNote &note : s.session.notes())
            bodies.push_back(note.body);
        ObsLifecycle lc = observer_lifecycle_from(bodies);
        observer_build(s.observer, *live, &lc);
        s.observed_events = n;
        s.observed_recordings = done.size();
    }
    // The tree filter's Start button hands back a command; sending it is the
    // door's job, not the view's.
    if (!s.observer.tree.rows.empty() || s.want == LiveMode::Tree) {
        std::string cmd =
            draw_obs_tree(s.observer.tree, s.observer, s.selected_pid);
        if (!cmd.empty()) {
            s.session.send(cmd);
            s.active.clear();
            s.active.push_back(LiveMode::Tree);
        }
        ImGui::Separator();
    }
    draw_observer(s.observer, *live, "live-session", nullptr);
}

// The PT-replay slice: a def-use slice with ZERO single-steps of the target.
// The gate has two levels and the UI must not blur them — capture needs PT
// silicon, replay needs only the emulator — so a host without PT still gets a
// working button for a path somebody else's box recorded.
void draw_pt_slice(InspectState &s) {
    ImGui::SeparatorText("PT slice — zero single-steps of the target");
    ImGui::TextWrapped("%s", ptslice_disclosure());
    PtSliceGate g = ptslice_gate(ptslice_facts());
    if (!g.reason.empty())
        // VERBATIM, and it is the library's own sentence: "unavailable" alone
        // cannot tell a user whether to change a sysctl or buy a CPU.
        ImGui::TextColored(kMaybe, "%s", g.reason.c_str());
    if (!g.can_capture)
        ImGui::TextDisabled("live PT capture is unavailable here; a RECORDED "
                            "path still replays");

    const Recording *live = s.session.growing();
    const std::vector<Recording> &done = s.session.recordings();
    if (live == nullptr && !done.empty())
        live = &done.back();
    if (live == nullptr)
        return;

    ImGui::BeginDisabled(!g.can_replay);
    if (ImGui::Button("Replay this session's PT path")) {
        s.ptslice = ptslice_run(ptslice_input_from(*live));
        s.ptslice_ran = true;
    }
    ImGui::EndDisabled();
    if (!s.ptslice_ran)
        return;
    if (!s.ptslice.reason.empty())
        ImGui::TextColored(s.ptslice.ok ? kMaybe : kBad, "%s",
                           s.ptslice.reason.c_str());
    if (!s.ptslice.ok)
        return;
    ImGui::Text("%llu step(s) replayed over a %llu-offset path",
                (unsigned long long)s.ptslice.steps,
                (unsigned long long)s.ptslice.path_len);
    if (s.ptslice.diverged)
        ImGui::TextColored(kBad,
                           "the replay DIVERGED from the recorded path at step "
                           "%llu — everything after it is not this run",
                           (unsigned long long)s.ptslice.diverged_at);
    // No new rendering: the same slice explorer a replayed recording gets.
    Streams st;
    st.df = s.ptslice.df;
    st.truncated = s.ptslice.truncated;
    st.backend = "pt-replay";
    draw_slice_view(dt_slice_view_build(st, std::nullopt));
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
        ImGui::SeparatorText("live views");
        draw_live_views(s);
        draw_pt_slice(s);
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
