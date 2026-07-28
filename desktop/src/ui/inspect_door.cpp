// inspect_door.cpp — the Inspect door's ImGui half (07-serve-live-host.md
// T4/T5). Same split as the other doors: everything decidable lives in the
// pure modules (live/inspect.h for attachability + evidence, live/budget.h for
// the jack), which test_inspect and test_budget drive headlessly. This file is
// the drawing and the wiring, and holds no rule of its own.
#include <cfloat>
#include <cstdio>
#include <cstring>

#include <sys/utsname.h> // uname — best-effort target arch for the arm64 gate

#include "imgui.h"

#include "ImGuiFileDialog.h" // pure-ImGui save dialog (14 T7)
#include "live/end_state.h"  // end_cause — the session-end placard (23 T2)
#include "ui/doors.h"
#include "ui/filter.h"   // dt_filter_bar / dt_filter_match — searchable procs (24 T4)
#include "ui/honesty.h"  // the graded honesty vocabulary (23 T1)
#include "ui/progress.h"
#include "ui/theme.h"
#include "views/views_draw.h"

namespace asmdesk {

void inspect_scan(InspectState &s) {
    s.rows = list_processes();
    s.scanned = true;
    // Best-effort target arch for the arm64 blocking-syscall gate (T5): the
    // tracee's true ELF arch needs a probe, so v1 uses the host's — the tracer
    // and target share a machine in the common case, and the annotation is
    // stated, never a hidden refusal, so an over-broad arm64 warning is safe.
    if (s.target_arch.empty()) {
        struct utsname u;
        if (uname(&u) == 0)
            s.target_arch = u.machine;
    }
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

void inspect_disconnect(InspectState &s) {
    if (!s.host_started)
        return;
    // shutdown() drains the host's exit cleanly — a capture in flight gets its
    // terminal event and footer, so it lands as a complete recording rather
    // than torn — then reaps. reset() drops that recording and every note with
    // it: a reconnect is a NEW session, and a deck still showing the last
    // host's capture would be attributing it to a host that is gone.
    s.session.shutdown();
    s.session.reset();
    s.host_started = false;
    s.host_error.clear();
    // The beliefs that only held while a host was up: the jack contents, an
    // armed swap, and the observer/PT-slice built over the last session. The
    // /proc scan (rows/selected_pid) and the typed asmspy path / ssh host
    // survive — reconnecting to the same target must not mean re-entering it.
    s.active.clear();
    s.swap_pending = false;
    s.operator_paused = false;
    s.has_queued = false;
    s.stream_op = LongOp{};
    s.observer.built = false;
    s.observed_events = 0;
    s.observed_recordings = 0;
    s.ptslice_ran = false;
}

bool inspect_request_start(InspectState &s) {
    // The perturbation gate (T5, F22): a single-stepping mode dirties the traced
    // page, perturbs timing, and on arm64 can kill a target blocked in a syscall.
    // The FIRST Start only arms the confirm (like the swap below, and the syscall
    // reveal-all); only a confirmed start proceeds. `sample` (out of band) skips
    // the gate. The confirm is one-shot: inspect_confirm_perturb sets the flag,
    // calls back in, and clears it.
    if (mode_uses_ptrace(s.want) && !s.perturb_confirmed) {
        s.perturb_pending = true;
        s.perturb_reason = mode_perturb_warning(s.want, s.target_arch);
        return false;
    }
    s.perturb_pending = false;

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
    s.session.send_start(mode_name(s.want), s.selected_pid,
                         inspect_start_params(s));
    s.active.push_back(s.want);
    return true;
}

// The `start` params for the current want: a scoped region for trace/dataflow (a
// func name or base+len, parse_region_spec), empty for every whole-process mode
// and for `auto` (which finds its own region). Sending the region here — rather
// than making the door build the JSON inline — keeps the one place a start is
// fired honest about what it sends.
nlohmann::json inspect_start_params(const InspectState &s) {
    nlohmann::json params = nlohmann::json::object();
    // The register ring (--steps, doc 26): the dataflow single-step engines
    // (dataflow + auto) can carry a per-step register file so the live Scrubber
    // time-travels. trace / whole-process modes have no such ring.
    if ((s.want == LiveMode::Dataflow || s.want == LiveMode::Auto) && s.steps)
        params["steps"] = true;
    if (!mode_needs_region(s.want))
        return params;
    std::string spec(s.region);
    uint64_t base = 0, len = 0;
    if (parse_region_spec(spec, &base, &len)) {
        params["base"] = base;
        params["len"] = len;
    } else if (!spec.empty()) {
        params["func"] = spec;
    }
    return params;
}

void inspect_attach_full_detail(InspectState &s, long pid) {
    s.selected_pid = pid;
    // `auto` is the fullest detail an UN-NAMED target admits: it samples to pick
    // the hottest function and data-flows it. Arm the register ring too, so the
    // Scrubber lights. Pin want_defaulted so the patch bay's least-perturbing
    // default cannot override the choice on its first draw.
    s.want = LiveMode::Auto;
    s.want_defaulted = true;
    s.steps = true;
    s.want_open_capture = true; // the confirm / status / views land in that pane
    if (s.host_started)
        inspect_request_start(s); // arms the perturb confirm for the single-step
    else
        s.want_open_connect = true; // no host yet — reveal Connect first
}

void inspect_confirm_perturb(InspectState &s) {
    if (!s.perturb_pending)
        return;
    // The user accepted the page-dirty / timing / arm64-kill consequence. Re-run
    // the start through the SAME path with the one-shot confirm set, so the
    // budget/swap gate below still applies (a confirmed perturb is not a
    // confirmed swap).
    s.perturb_confirmed = true;
    inspect_request_start(s);
    s.perturb_confirmed = false;
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

// The verdict axis, from the shared palette (ui/theme.h T1). kBad now equals the
// app's ONE refusal red (was 0.90 here, 0.95 in the placards — the F14 split is
// gone); good/maybe are the shared accessors too, so every "attachable / NOT /
// maybe" reads the same colour it does in any other pane.
const ImVec4 kGood = dt_good_col();
const ImVec4 kBad = dt_bad_col();
const ImVec4 kMaybe = dt_maybe_col();

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
        // A skip grades NEUTRAL (schema:98/544): the tracer worked and had
        // nothing to report, so it is a quiet chip, never an amber banner. The
        // measured reason is the whole payload and still renders in full (D7).
        char buf[512];
        std::snprintf(buf, sizeof buf, "skipped (%d): %s", st.skip_code,
                      st.skip_reason.c_str());
        draw_honesty_chip(buf, HonestyTier::Neutral);
    }
    if (st.paused_dropped) {
        // paused_dropped -> CAUTION (a usable prefix with a recorded gap).
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "%llu event(s) dropped while paused — this recording is "
                      "truncated (usable, but incomplete)",
                      (unsigned long long)st.paused_dropped);
        draw_honesty_chip(buf, HonestyTier::Caution);
    }
    if (s.session.malformed_lines()) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%llu unparseable line(s) from the host",
                      (unsigned long long)s.session.malformed_lines());
        draw_honesty_chip(buf, HonestyTier::Caution);
    }

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
    if (const Recording *g = s.session.growing()) {
        ImGui::Text("capturing: %llu event(s) so far",
                    (unsigned long long)g->event_count());
        // The uniform busy signal (23 T4): a growing live recording is unbounded
        // — no `end` footer, no honest total — so the LongOp gets the
        // INDETERMINATE bar (a percentage would be a fabricated total, 14 T3),
        // plus elapsed + a Cancel that stops the capture (leaving the last good
        // recording, never a half-built model). `now`/`started_at` are injected
        // from ImGui's clock so the pure elapsed() needs no global.
        s.stream_op.active = true;
        s.stream_op.has_total = false;
        s.stream_op.label = "streaming";
        s.stream_op.now = ImGui::GetTime();
        if (s.stream_op.started_at == 0.0)
            s.stream_op.started_at = s.stream_op.now;
        draw_progress(s.stream_op);
        if (s.stream_op.cancel_requested) {
            s.session.send_stop();
            s.active.clear();
            s.stream_op.cancel_requested = false;
        }
    } else {
        s.stream_op.active = false;
        s.stream_op.started_at = 0.0; // re-arm the elapsed clock for the next one
    }
    if (nrec)
        ImGui::Text("%zu completed recording(s) this session", nrec);
    // Each completed torn recording stays a loud INTEGRITY banner (non-collapsible)
    // — a torn capture is the one signal that means "do not trust the tail".
    for (const Recording &r : s.session.recordings())
        if (r.torn)
            draw_honesty_banner("TORN recording — the host stopped before "
                                "writing its footer; this capture is incomplete",
                                HonestyTier::Integrity);

    // The persistent, cause-distinguished END-OF-SESSION placard (23 T2, F20).
    // Co-located with the last events, it fans the single collapsed `Ended` into
    // its four causes and — for a protocol mismatch — carries the verbatim
    // one-line fix. It persists across frames (unlike a toast) until the next
    // Connect/Start. Graded through T1's components: torn/mismatch are integrity,
    // a clean stop is neutral.
    if (st.state == LiveState::Ended) {
        EndCause cause = end_cause(end_facts_of(s.session));
        ImGui::SeparatorText(end_cause_title(cause));
        HonestyTier tier = end_cause_is_integrity(cause) ? HonestyTier::Integrity
                                                         : HonestyTier::Neutral;
        std::string msg = end_cause_message(cause);
        draw_honesty_banner(msg.c_str(), tier);
        std::string fix = end_cause_fix(cause);
        if (!fix.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGood);
            ImGui::TextWrapped("%s", fix.c_str());
            ImGui::PopStyleColor();
        }
    }
}

// The patch bay: one jack per target, and what is in it.
void draw_patch_bay(InspectState &s) {
    ImGui::SeparatorText("patch bay — one ptrace jack per target");
    for (LiveMode m : all_modes()) {
        // arm64 blocking-syscall hazard (T5): a single-stepped thread inside a
        // blocking syscall survives DETACH on arm64 and dies ~300ms later
        // (SPSR.SS), and teardown cannot undo it. Grey (BeginDisabled) the
        // single-step modes there — never a hidden refusal, always a stated one,
        // with the reason in the tooltip.
        const bool hazard = mode_arm64_blocking_hazard(m, s.target_arch);
        if (hazard)
            ImGui::BeginDisabled(true);
        if (ImGui::RadioButton(mode_name(m), s.want == m))
            s.want = m;
        if (hazard)
            ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            if (hazard)
                ImGui::SetTooltip("%s\n\narm64: %s", mode_jack_reason(m),
                                  mode_perturb_warning(m, s.target_arch).c_str());
            else
                ImGui::SetTooltip("%s", mode_jack_reason(m));
        }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    // Least-perturbing default (T5): steer the picker off a single-step mode
    // toward the lightest substrate the host supports. `sample_available` is set
    // by the shell from the capability probe; false under the null backend, where
    // Log is already the default.
    if (!s.want_defaulted) {
        s.want_defaulted = true;
        s.want = budget_least_perturbing(s.sample_available);
    }

    // The scoped-region input (the dataflow/trace region gap). trace and dataflow
    // single-step ONE region, so the serve host requires a func name or 0xADDR:LEN
    // (serve_resolve_region) — without one, `start` is rejected. Shown ONLY for
    // those modes; `auto` finds its own region via the sampler, so it needs none.
    const bool needs_region = mode_needs_region(s.want);
    if (needs_region) {
        ImGui::InputTextWithHint("region##cap",
                                 "function name   or   0xADDR:LEN", s.region,
                                 sizeof s.region);
        ImGui::TextDisabled(
            "%s single-steps ONE region — name a function or an address range, "
            "or pick `auto` to trace the hottest function automatically.",
            mode_name(s.want));
    }
    const bool has_region = !needs_region || s.region[0] != '\0';

    // The register ring (--steps, doc 26): the dataflow single-step engines
    // (dataflow + auto) can carry a per-step register file so the live Scrubber
    // time-travels. Off by default (it is extra, perturbing capture); the
    // full-detail attach (double-click a process) arms it.
    if (s.want == LiveMode::Dataflow || s.want == LiveMode::Auto) {
        ImGui::Checkbox("record the register ring (--steps → live Scrubber)",
                        &s.steps);
    }

    // The Queue path (23 T3): a queued want starts the MOMENT the jack frees —
    // never an auto-swap, because budget_queue_ready is true only when the jack is
    // genuinely free (the blocker stopped/ended). The one-ptrace-jack invariant is
    // never bypassed. Polled here, before we render the state below.
    if (s.has_queued && budget_queue_ready(s.queued_want, s.active)) {
        s.session.send_start(mode_name(s.queued_want), s.selected_pid);
        s.active.push_back(s.queued_want);
        s.has_queued = false;
    }

    BudgetDecision d = budget_can_start(s.want, s.active);
    // BUDGET BLOCK (F23): "BLOCKED — jack held by <session> on <target>" — a
    // FACT, read BLOCKED not "paused", so it never reads as the operator pause.
    if (!d.allowed) {
        ImGui::TextColored(kMaybe, "%s on pid %ld",
                           budget_blocked_label(d).c_str(), s.selected_pid);
        ImGui::TextWrapped("%s", d.reason.c_str());
    }

    const bool can = s.host_started && s.selected_pid > 0 && has_region;
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Start"))
        inspect_request_start(s);
    ImGui::EndDisabled();
    if (!can) {
        ImGui::SameLine();
        const char *why = !s.host_started  ? "connect a serve host first"
                          : s.selected_pid <= 0 ? "select a process first"
                                              : "name a region above, or pick "
                                                "`auto`";
        ImGui::TextDisabled("%s", why);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        s.session.send_stop();
        s.active.clear();
        s.operator_paused = false;
    }
    ImGui::SameLine();
    // OPERATOR PAUSE (F23), split from the budget block: this hold's ONLY
    // recovery is Resume, and the state says so — "PAUSED (you)".
    if (ImGui::Button("Pause")) {
        s.session.send_pause(true);
        s.operator_paused = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Resume")) {
        s.session.send_pause(false);
        s.operator_paused = false;
    }
    if (s.operator_paused)
        ImGui::TextColored(kMaybe,
                           "PAUSED (you) — emission is suspended (tracing is "
                           "not); the only recovery is Resume.");

    // The perturbation confirm (T5): armed by the first Start on a single-step
    // mode, it states the concrete consequence VERBATIM and only the second
    // click fires. Drawn before the swap confirm because it gates first.
    if (s.perturb_pending) {
        ImGui::Separator();
        ImGui::TextColored(kBad, "%s", s.perturb_reason.c_str());
        if (ImGui::Button("Arm it anyway"))
            inspect_confirm_perturb(s);
        ImGui::SameLine();
        if (ImGui::Button("Cancel##perturb"))
            s.perturb_pending = false;
    }

    // The budget block's THREE explicit recoveries (F23): Swap (a named two-step
    // confirm), Queue (a visible cancellable chip), Cancel. Never an auto-swap.
    if (!d.allowed) {
        ImGui::Separator();
        if (ImGui::Button("Swap")) {
            // Arm the two-step confirm — it names WHAT detaches. Never silent.
            s.swap_pending = true;
            s.swap_blocker = d.blocker;
            s.swap_reason = d.reason;
        }
        ImGui::SameLine();
        if (ImGui::Button("Queue")) {
            s.has_queued = true;
            s.queued_want = s.want;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel##block")) {
            s.swap_pending = false;
            s.has_queued = false;
        }
    }

    // The Swap confirm (two-step): naming exactly what detaches. Never auto-swap.
    if (s.swap_pending) {
        ImGui::Separator();
        ImGui::TextColored(kMaybe,
                           "Stop the %s capture on pid %ld and start %s?",
                           mode_name(s.swap_blocker), s.selected_pid,
                           mode_name(s.want));
        if (ImGui::Button("Stop it and start this one"))
            inspect_confirm_swap(s);
        ImGui::SameLine();
        if (ImGui::Button("Cancel##swap"))
            s.swap_pending = false;
    }

    // The Queue chip: a visible, cancellable record of what will start when the
    // jack frees, with a one-line note of what Queue does. It disappears when the
    // queued mode fires (above) or the user cancels it.
    if (s.has_queued) {
        char chip[192];
        std::snprintf(chip, sizeof chip,
                      "Queued: %s will start automatically when the %s jack "
                      "frees",
                      mode_name(s.queued_want), mode_name(d.blocker));
        draw_honesty_chip(chip, HonestyTier::Neutral);
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel queue"))
            s.has_queued = false;
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
            draw_obs_tree(s.observer.tree, s.observer, s.selected_pid,
                          "live-session", nullptr);
        if (!cmd.empty()) {
            s.session.send(cmd);
            s.active.clear();
            s.active.push_back(LiveMode::Tree);
        }
        ImGui::Separator();
    }
    draw_observer(s.observer, *live, "live-session", nullptr);
}

// Save this session's capture to a .asmtrace file, and offer to open the saved
// file in the Loom. The live host keeps recordings in memory; this is the one
// action that puts one on disk — in the same NDJSON a `--record` run would have
// written. Saving the growing recording is allowed but writes a partial (torn)
// file, so the growing case says so. "Open in Loom" cannot reach the Workspace
// from here, so it posts a request the shell fulfils (s.open_request).
void draw_save_capture(InspectState &s) {
    ImGui::SeparatorText("save capture");
    const Recording *live = s.session.growing();
    const std::vector<Recording> &done = s.session.recordings();
    const bool growing = live != nullptr;
    if (live == nullptr && !done.empty())
        live = &done.back();
    if (live == nullptr) {
        ImGui::TextDisabled(
            "nothing captured yet — start a mode to record one");
        return;
    }
    ImGui::InputText("path##save", s.save_path, sizeof s.save_path);
    ImGui::SameLine();
    // Pure-ImGui save picker (14-quick-wins.md T7): fills the path field, so the
    // manual field stays as a fallback / for scripted paths. Confirm-overwrite is
    // on, and the default name is the stray-safe "capture.asmtrace".
    if (ImGui::Button("Browse…##save")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ".";
        cfg.fileName = "capture.asmtrace";
        cfg.flags =
            ImGuiFileDialogFlags_Modal | ImGuiFileDialogFlags_ConfirmOverwrite;
        ImGuiFileDialog::Instance()->OpenDialog("dlg_save", "Save capture as…",
                                                ".asmtrace", cfg);
    }
    if (ImGuiFileDialog::Instance()->Display(
            "dlg_save", ImGuiWindowFlags_NoCollapse, ImVec2(520, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            std::snprintf(s.save_path, sizeof s.save_path, "%s", p.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }
    if (growing)
        ImGui::TextColored(kMaybe,
                           "the capture is still running — saving now writes a "
                           "partial (torn) recording; Stop first for a "
                           "complete one");
    if (ImGui::Button("Save .asmtrace")) {
        std::string err;
        if (save_recording_file(*live, s.save_path, err)) {
            s.saved_ok = true;
            s.saved_path = s.save_path;
            s.saved_statistical = live->statistical();
            s.save_status = "saved " + std::to_string(live->event_count()) +
                            " event(s) to " + s.saved_path;
        } else {
            s.saved_ok = false;
            s.save_status = err;
        }
    }
    if (!s.save_status.empty())
        ImGui::TextColored(s.saved_ok ? kGood : kBad, "%s",
                           s.save_status.c_str());
    // Offer the Loom only for a saved file the Loom can actually weave: an
    // exact recording with per-step values. A statistical or trace-only capture
    // would open and immediately show the Loom's refusal, so say why here
    // instead of sending the user to a dead end.
    if (s.saved_ok) {
        if (s.saved_statistical)
            ImGui::TextDisabled("(a statistical capture cannot be woven into a "
                                "Loom — it needs exact per-step values)");
        else if (ImGui::Button("Open in Loom"))
            s.open_request = s.saved_path;
    }
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

// --- Connect pane: the serve-host connection ------------------------------
void draw_connect_pane(InspectState &s) {
    ImGui::TextWrapped(
        "Attach to a running process. The capture host is the `asmspy --serve` "
        "subprocess, so nothing here links a tracer.");
    ImGui::Spacing();

    if (!s.host_started) {
        // Pre-fill the asmspy path ONCE with what a blank would resolve to, so the
        // field shows the concrete exe (still editable) rather than an empty box —
        // the operator sees exactly which binary Connect will spawn.
        if (!s.asmspy_prefilled) {
            s.asmspy_prefilled = true;
            if (s.asmspy_path[0] == '\0') {
                std::string found = resolve_asmspy_path();
                std::snprintf(s.asmspy_path, sizeof s.asmspy_path, "%s",
                              found.c_str());
            }
        }
        ImGui::InputText("asmspy path", s.asmspy_path, sizeof s.asmspy_path);
        ImGui::InputTextWithHint("ssh host", "blank = local", s.ssh_host,
                                 sizeof s.ssh_host);
        ImGui::TextDisabled(
            "blank path resolves $PATH, then ./build/asmspy. asmspy is a Linux "
            "tracer; from another OS set an ssh host (`ssh <host> asmspy "
            "--serve`).");
        if (ImGui::Button("Connect"))
            inspect_connect(s);
        if (!s.host_error.empty())
            ImGui::TextColored(kBad, "%s", s.host_error.c_str());
    } else {
        const LiveStatus &st = s.session.status();
        ImGui::Text("host: %s", st.command.c_str());
        // The way back to the Connect form. Without it host_started latches for
        // the life of the process, and re-entering connection details (a
        // different asmspy, a different ssh host) means relaunching the app.
        if (ImGui::Button("Disconnect"))
            inspect_disconnect(s);
        ImGui::SameLine();
        ImGui::TextDisabled("stop the serve host and return to Connect");
    }
}

// --- Processes pane: the searchable /proc target picker -------------------
void draw_processes_pane(InspectState &s) {
    // A host with no /proc lists nothing, and "0 process(es); ptrace_scope=-1"
    // reads as "nothing running, no restrictions" — two measurements neither of
    // which was made. State the absence instead, and draw no table: an empty one
    // would be the same claim in a different shape.
    if (const char *why = local_inspect_unavailable(); *why) {
        ImGui::TextWrapped("local inspection unavailable: %s", why);
        ImGui::TextDisabled(
            "remote capture still works — set an ssh host in Connect.");
        return;
    }
    if (!s.scanned)
        inspect_scan(s);
    if (ImGui::Button("Rescan"))
        inspect_scan(s);
    ImGui::SameLine();
    ImGui::TextDisabled("ptrace_scope=%d", read_yama_scope());

    // Type-to-narrow (24 T4's shared filter) over pid / comm / cmdline — the
    // /proc list is the one client-side table that never had a filter (doc 16's
    // framing). Build the haystack once; the matcher + "showing N of M" are pure.
    std::vector<std::string> hay;
    hay.reserve(s.rows.size());
    for (const ProcRow &r : s.rows)
        hay.push_back(std::to_string(r.pid) + " " + r.comm + " " + r.cmdline);
    std::string q = dt_filter_bar(s.proc_filter, hay, "filter");
    ImGui::TextDisabled("double-click a row to attach & trace at full detail; "
                        "right-click for more.");

    if (ImGui::BeginTable("procs", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("pid");
        ImGui::TableSetupColumn("comm");
        ImGui::TableSetupColumn("attach");
        ImGui::TableSetupColumn("why / remedy");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.rows.size(); ++i) {
            if (!dt_filter_match(q, hay[i]))
                continue;
            const ProcRow &r = s.rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char lbl[64];
            std::snprintf(lbl, sizeof lbl, "%ld##p%ld", r.pid, r.pid);
            if (ImGui::Selectable(lbl, s.selected_pid == r.pid,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                s.selected_pid = r.pid;
                // Double-click = attach & trace at FULL detail (auto + the register
                // ring); a single click only selects the target.
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    inspect_attach_full_detail(s, r.pid);
            }
            // Right-click the row: attach at full detail, trace a named function,
            // or reveal the Connect pane. BeginPopupContextItem() with no id binds
            // to the row's Selectable above (its ##p<pid> makes each row unique).
            if (ImGui::BeginPopupContextItem()) {
                ImGui::TextDisabled("pid %ld — %s", r.pid, r.comm.c_str());
                ImGui::Separator();
                if (ImGui::MenuItem("Attach & trace (full detail)"))
                    inspect_attach_full_detail(s, r.pid);
                if (ImGui::MenuItem("Trace a function… (dataflow)")) {
                    s.selected_pid = r.pid;
                    s.want = LiveMode::Dataflow;
                    s.want_defaulted = true;
                    s.want_open_capture = true; // name the region in Live capture
                    if (!s.host_started)
                        s.want_open_connect = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open Connect pane"))
                    s.want_open_connect = true;
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.comm.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(verdict_colour(r.verdict.verdict), "%s",
                               verdict_word(r.verdict.verdict));
            ImGui::TableNextColumn();
            // The reason is FIRST-CLASS, in the row — not a toast that disappears
            // and not an error the user has to provoke. That is the door's whole
            // promise: see why not.
            ImGui::TextUnformatted(r.verdict.why.c_str());
            if (!r.verdict.remedy.empty())
                ImGui::TextDisabled("-> %s", r.verdict.remedy.c_str());
        }
        ImGui::EndTable();
    }
}

// --- Live capture pane: the patch bay + session + views + save + PT slice --
void draw_capture_pane(InspectState &s) {
    if (!s.host_started) {
        ImGui::TextDisabled(
            "connect a serve host (the Connect pane) to capture.");
        return;
    }
    if (s.selected_pid <= 0)
        ImGui::TextDisabled(
            "pick a process in the Processes pane, then arm a mode below.");
    draw_patch_bay(s);
    draw_status(s);
    ImGui::SeparatorText("live views");
    draw_live_views(s);
    draw_save_capture(s);
    draw_pt_slice(s);
}

void draw_inspect_door(InspectState &s) {
    // The single-window composition (windowed shell + render-only viewer): the
    // three panes stacked, each under its own separator. The docked shell instead
    // Begins draw_connect_pane / draw_processes_pane / draw_capture_pane in three
    // real dockable windows. Poll once per frame here; the docked path polls too.
    s.session.poll();
    ImGui::SeparatorText("Connect");
    draw_connect_pane(s);
    ImGui::SeparatorText("Processes");
    draw_processes_pane(s);
    ImGui::SeparatorText("Live capture");
    draw_capture_pane(s);
    // The cross-pane reveal requests are docked-shell only — here all three are
    // already in the one Inspect tab, so consume (clear) them without action.
    s.want_open_connect = false;
    s.want_open_capture = false;
}

} // namespace asmdesk
