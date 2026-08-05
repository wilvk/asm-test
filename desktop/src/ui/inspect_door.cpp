// inspect_door.cpp — the Inspect door's ImGui half (07-serve-live-host.md
// T4/T5). Same split as the other doors: everything decidable lives in the
// pure modules (live/inspect.h for attachability + evidence, live/budget.h for
// the jack), which test_inspect and test_budget drive headlessly. This file is
// the drawing and the wiring, and holds no rule of its own.
#include <algorithm> // stable_sort — the free column sort over the /proc rows
#include <cctype>    // isspace — doc 45 T3's launch-args splitter
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <strings.h> // strcasecmp — case-insensitive column sort of comm / why

#include <sys/utsname.h> // uname — best-effort target arch for the arm64 gate
#include <unistd.h>      // sysconf(_SC_CLK_TCK) — jiffies -> %CPU in the activity cell

#include "imgui.h"

#include "ImGuiFileDialog.h" // pure-ImGui save dialog (14 T7)
#include "live/end_state.h"  // end_cause — the session-end placard (23 T2)
#include "ui/doors.h"
#include "ui/fidelity.h" // the graded fidelity vocabulary (23 T1)
#include "ui/filter.h" // dt_filter_bar / dt_filter_match — searchable procs (24 T4)
#include "ui/flow.h"
#include "ui/progress.h"
#include "ui/theme.h"
#include "views/views_draw.h"

namespace asmdesk {

void inspect_scan(InspectState &s) {
    s.rows = list_processes(s.sample_cpu);
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
    if (s.record_session)
        spec.record_path = s.record_path;
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
    s.seen_sessions_ended = 0; // 39 T5: reset() zeroes the status counter too
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
    // An illegal tree filter must never reach the wire — the deleted
    // obs_tree_start_command self-guarded exactly this way (it returned "" on a
    // bad filter). The patch bay already greys Start on tree_err, so this is the
    // backstop for every OTHER caller into this function (the perturb "Start
    // anyway", any future path): the guarantee never rests on a UI gate alone.
    if (s.want == LiveMode::Tree &&
        !obs_tree_filter_error(s.observer.filter).empty())
        return false;
    // The perturbation gate (T5, F22), now scoped to the ONE unrecoverable case.
    // A single-stepping mode always dirties the traced page and perturbs timing —
    // a nuisance, and no longer worth a second click (Start just starts). But on
    // arm64 single-stepping a thread blocked in a syscall TERMINATES the target
    // and detach cannot undo it: that is data loss, so it keeps the pre-commit
    // confirm. The confirm is one-shot: inspect_confirm_perturb sets the flag,
    // calls back in, and clears it. `sample` and every non-arm64 mode skip it.
    if (mode_arm64_blocking_hazard(s.want, s.target_arch) &&
        !s.perturb_confirmed) {
        s.perturb_pending = true;
        s.perturb_reason = mode_perturb_warning(s.want, s.target_arch);
        s.perturb_via_launch = false;
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
    s.awaiting_started = true; // 39 T5: a start is in flight until `started`
    return true;
}

// docs/internal/archive/gui/45-launch-and-window-target.md T3: split the Launch
// pane's free-text "arguments" field on whitespace into argv[1..] — v1 has
// no quoting grammar (Non-goals), so a value containing a space is a rare
// case left for a follow-up rather than a reason to hold up the rest.
namespace {
std::vector<std::string> split_launch_args(const char *s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char *p = s; *p; p++) {
        if (std::isspace((unsigned char)*p)) {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += *p;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}
} // namespace

// docs/internal/gui/45 T3: `launch`'s counterpart to inspect_request_start —
// same tree-filter/perturb/budget gates (a launch is still subject to the
// one-ptrace-jack rule and the arm64 single-step hazard), but sends
// `launch` with no pid, and arms launch_awaiting_pid instead of assuming one.
// A busy jack refuses cleanly rather than offering a swap (unlike an attach,
// there is no "already-selected target" a launch's swap would even name —
// see doors.h). Returns false and does nothing to session state if
// s.launch_cmd is blank; the Launch pane's button gates on this too.
bool inspect_request_launch(InspectState &s) {
    if (!s.launch_cmd[0])
        return false;
    if (s.want == LiveMode::Tree &&
        !obs_tree_filter_error(s.observer.filter).empty())
        return false;
    if (mode_arm64_blocking_hazard(s.want, s.target_arch) &&
        !s.perturb_confirmed) {
        s.perturb_pending = true;
        s.perturb_reason = mode_perturb_warning(s.want, s.target_arch);
        s.perturb_via_launch = true;
        return false;
    }
    s.perturb_pending = false;

    BudgetDecision d = budget_can_start(s.want, s.active);
    if (!d.allowed)
        return false;

    std::vector<std::string> argv;
    argv.emplace_back(s.launch_cmd);
    for (auto &a : split_launch_args(s.launch_args))
        argv.push_back(a);
    s.session.send_launch(mode_name(s.want), argv, s.launch_cwd,
                          inspect_start_params(s));
    s.active.push_back(s.want);
    s.awaiting_started = true;
    s.launch_awaiting_pid = true;
    return true;
}

void inspect_launch_full_detail(InspectState &s) {
    s.want_open_capture = true; // the confirm / status / views land in that pane
    // No host yet: connect from the saved Settings, same as an attach — only
    // if that fails do we reveal Connect, so its host_error is actionable.
    if (!s.host_started)
        inspect_connect(s);
    if (s.host_started)
        inspect_request_launch(s);
    else
        s.want_open_connect = true;
}

// The `start` params for the current want: a scoped region for trace/dataflow (a
// func name or base+len, parse_region_spec), empty for every whole-process mode
// and for `auto` (which finds its own region). Sending the region here — rather
// than making the door build the JSON inline — keeps the one place a start is
// fired truthful about what it sends.
nlohmann::json inspect_start_params(const InspectState &s) {
    nlohmann::json params = nlohmann::json::object();
    // The register ring (--steps, doc 26): the dataflow single-step engines
    // (dataflow + auto) can carry a per-step register file so the live Scrubber
    // time-travels. trace / whole-process modes have no such ring.
    if ((s.want == LiveMode::Dataflow || s.want == LiveMode::Auto) && s.steps)
        params["steps"] = true;
    // 35 T4: continuous re-arm, only for the dataflow single-step engines.
    if ((s.want == LiveMode::Dataflow || s.want == LiveMode::Auto) &&
        s.continuous)
        params["continuous"] = true;
    // 39 T3: the `auto` sample window (ms). Only `auto` samples for a region;
    // dataflow/trace name theirs. Sent only when the operator set a non-default
    // value (window_ms > 0), so an untouched capture is byte-identical and the
    // host applies its own default (AUTO_WINDOW_MS).
    if (s.want == LiveMode::Auto && s.window_ms > 0)
        params["ms"] = s.window_ms;
    // The tree filter (depth/focus/module/tid/follow) rides this same Start — the
    // patch bay owns tree config now, so the one Start carries it. obs_tree_start_
    // params omits any field left at its default, exactly as the wire expects. To
    // NARROW a running tree, Stop it first: the host is one-jack and refuses a
    // start while a session runs, so the patch bay budget-blocks a re-Start.
    if (s.want == LiveMode::Tree) {
        nlohmann::json tp = obs_tree_start_params(s.observer.filter);
        for (auto it = tp.begin(); it != tp.end(); ++it)
            params[it.key()] = it.value();
        return params;
    }
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

const std::vector<LiveMode> &inspect_sweep_legs(const InspectState &s) {
    // While a sweep runs the shape is the LATCHED one — an auto-led sweep fills
    // `region` in from its own pick, and re-reading it here would swap the leg
    // list mid-sequence. Idle, it previews what pressing the button would run.
    return sweep_legs(s.sweep_running ? s.sweep_have_region
                                      : s.region[0] != '\0');
}

// The InspectState-shaped view of the pure rule in live/budget.h. Kept a thin
// forward on purpose: the decision is what can be wrong, and it is tested there
// against no live target at all.
std::string inspect_sweep_blocked(const InspectState &s) {
    return sweep_blocked(s.host_started, s.selected_pid > 0,
                         s.record_session && s.record_path[0] != '\0',
                         s.sweep_running, !s.active.empty());
}

void inspect_sweep_start(InspectState &s) {
    if (!inspect_sweep_blocked(s).empty())
        return;
    s.sweep_at = 0;
    s.sweep_running = true;
    // Latch the shape now: everything below reads it, and the auto-led leg is
    // about to change the field it was derived from.
    s.sweep_have_region = s.region[0] != '\0';
    s.sweep_note = s.sweep_have_region
                       ? "sweep armed"
                       : "sweep armed — no region named, so the `auto` leg "
                         "samples for one and the `trace` leg behind it "
                         "single-steps whatever it picks";
}

void inspect_sweep_cancel(InspectState &s) {
    // Deliberately does NOT stop a running leg: that is the Stop button's job,
    // and killing a capture from here would discard events the operator never
    // asked to lose. This only stops the sweep from arming the NEXT one.
    s.sweep_running = false;
    s.sweep_at = 0;
    s.sweep_note = "sweep cancelled — the running leg (if any) was left alone";
}

// The auto leg's hand-off. A scoped leg single-steps ONE region and the serve
// host refuses it without one; in an auto-led sweep that region is whatever the
// sampler picked, which arrives on the wire as a `session state:"pick"` event.
// Adopt the LAST real pick (the host walks past candidates that never re-enter,
// and re-emits on each, so the last one is the region actually captured) into
// the region field — where the operator can SEE what the next leg will step.
// Returns the adopted spec, or "" when no pick carried a region.
std::string inspect_sweep_pick_region(const std::vector<LiveNote> &notes) {
    std::string spec;
    for (const LiveNote &n : notes) {
        AutoPick p;
        if (n.kind != "session" || !parse_auto_pick(n.body, &p))
            continue;
        const std::string r = pick_region_spec(p);
        if (!r.empty())
            spec = r; // keep walking: the LAST real pick is the captured one
    }
    return spec;
}

static std::string inspect_sweep_adopt_pick(InspectState &s) {
    const std::string spec = inspect_sweep_pick_region(s.session.notes());
    if (spec.empty() || spec.size() >= sizeof s.region)
        return std::string();
    std::snprintf(s.region, sizeof s.region, "%s", spec.c_str());
    return spec;
}

bool inspect_sweep_poll(InspectState &s) {
    if (!s.sweep_running)
        return false;
    const std::vector<LiveMode> &legs = inspect_sweep_legs(s);
    if (s.sweep_at >= legs.size()) {
        s.sweep_running = false;
        s.sweep_note =
            "sweep complete — every leg captured. Disconnect, then File > Open "
            "the recorded file: that ONE recording carries the call tree, the "
            "region trace and the wide register writes together, so the 3D "
            "pane offers the address plane, the invocation stack, the module "
            "excursion ribbon and the SIMD lane prism. The divergence "
            "worldline needs a SECOND recording — sweep again and attach it "
            "with `d`.";
        return false;
    }
    const LiveMode leg = legs[s.sweep_at];
    // The SAME jack rule the Queue obeys — a sweep may never bypass the budget.
    if (s.awaiting_started || !budget_queue_ready(leg, s.active))
        return false;

    // An auto-led sweep's scoped leg inherits the region the auto leg picked.
    // If the auto leg found none — the sampler was refused (perf locked down),
    // or every window was idle — there is nothing to single-step, so the sweep
    // STOPS here and says which leg it stopped at and why. Firing the leg anyway
    // would send a start the host refuses, and report it as the operator's
    // mistake rather than as the measured fact it is.
    if (mode_needs_region(leg) && s.region[0] == '\0' &&
        inspect_sweep_adopt_pick(s).empty()) {
        const std::string why = s.session.status().skip_reason;
        s.sweep_running = false;
        s.sweep_note =
            "sweep stopped before its `" + std::string(mode_name(leg)) +
            "` leg: the `auto` leg picked no region, so there is nothing for a "
            "scoped leg to single-step" +
            (why.empty() ? std::string() : " — the host said: " + why) +
            ". The legs that needed no region did run. Name a region and sweep "
            "again to add the invocation stack.";
        return false;
    }

    // `want` is what inspect_start_params reads, so set it before building.
    s.want = leg;
    inspect_apply_continuous_default(s);
    // A sweep must not re-arm: a continuous leg never ends on its own, so the
    // sweep would stall on it forever.
    s.continuous = false;
    nlohmann::json params = inspect_start_params(s);
    // BOUNDED. A hand-driven capture runs until the operator stops it; a
    // sequence cannot, or its first leg is also its last.
    params["max"] = s.sweep_max;
    s.session.send_start(mode_name(leg), s.selected_pid, params);
    s.active.push_back(leg);
    s.awaiting_started = true;
    s.sweep_at++;
    s.sweep_note = "sweep: leg " + std::to_string(s.sweep_at) + "/" +
                   std::to_string(legs.size()) + " (" + mode_name(leg) + ")";
    return true;
}

// `continuous` defaults PER MODE: on for `auto`, off for everything else. An
// `auto` capture finds its own region from a sample window, so one invocation of
// it is usually over before the operator has looked at anything — "keep watching
// this process" is what picking `auto` means. A named `dataflow` region keeps the
// one-invocation default it always had. Re-applied on each entry into a mode so
// arriving at `auto` from another pick still gets it, and never once the operator
// has moved the checkbox (continuous_touched) — a default must not fight a choice.
void inspect_apply_continuous_default(InspectState &s) {
    if (s.continuous_touched || s.want == s.continuous_defaulted_for)
        return;
    s.continuous_defaulted_for = s.want;
    s.continuous = (s.want == LiveMode::Auto);
}

void inspect_reconcile_self_end(InspectState &s, const LiveStatus &st) {
    // A start the desktop issued resolves once the host confirms it (Running) or
    // the host is gone — clear the in-flight guard then. NOT keyed on skip_code:
    // that field is STICKY (set on a `skip`, cleared only by the next `started`),
    // and a Swap's swapped-OUT session can itself skip — a dataflow/auto blocker
    // that saw 0 passes returns NEVER_RAN, which serve reports as `skip` — so
    // clearing the guard on skip_code would free the swap's newly-armed session in
    // the same Idle split-frame the guard exists to protect. The real cost: a
    // fresh start that is itself SKIPPED at the door (an i386 tracee, a sampler
    // self-skip) leaves the guard set until the next resolving event, so its jack
    // reads held until a Stop/Disconnect — the pre-39 behaviour for a skip, never
    // freed automatically, so this is not a regression, only an unclosed edge.
    if (st.state != LiveState::Idle)
        s.awaiting_started = false;
    // docs/internal/gui/45 T3: a `launch` sent no pid — the host forks one and
    // names it in THIS "started" reply (LiveStatus::pid, set by
    // LiveSession::feed_line the moment state flips to Running). An attach
    // already knows its pid before it ever calls inspect_reconcile_self_end,
    // so this adopts it ONLY for the launch case (launch_awaiting_pid), never
    // overwriting an attach's selected_pid with something it did not ask for.
    if (st.state == LiveState::Running && s.launch_awaiting_pid) {
        s.selected_pid = st.pid;
        s.launch_awaiting_pid = false;
    }
    if (st.sessions_ended == s.seen_sessions_ended)
        return; // no NEW terminal event since we last reconciled
    s.seen_sessions_ended = st.sessions_ended; // consume the delta
    // Free the ptrace jack ONLY for a genuine self-end: the host is idle AND the
    // desktop has no start in flight. A SWAP sends stop+start together, so the
    // old session's terminal event must not free the newly-armed session before
    // its `started` lands (awaiting_started bridges that gap; keying on
    // state==Idle alone would misfire if the two events split across frames). A
    // user-driven Stop/Swap already updated `active`, so re-freeing — or freeing
    // an empty set — here is harmless. The serve host runs ONE ptrace capture at
    // a time, so a terminal event frees whatever ptrace mode held the jack; free
    // views were never on it.
    if (st.state == LiveState::Idle && !s.awaiting_started)
        s.active.erase(std::remove_if(s.active.begin(), s.active.end(),
                                      [](LiveMode m) {
                                          return mode_uses_ptrace(m);
                                      }),
                       s.active.end());
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
    // The start fires from here, before the capture pane draws — so apply the
    // per-mode `continuous` default now, or this start would send one value while
    // the checkbox the user sees a frame later shows another.
    inspect_apply_continuous_default(s);
    s.want_open_capture = true; // the confirm / status / views land in that pane
    // No host yet: connect from the saved Settings (asmspy path / ssh host) rather
    // than bouncing the user to the Connect pane. Only if that connect fails do we
    // reveal Connect, so its host_error is where the user can act on it.
    if (!s.host_started)
        inspect_connect(s);
    if (s.host_started)
        inspect_request_start(s); // arms the perturb confirm for the single-step
    else
        s.want_open_connect = true; // connect failed — reveal Connect + its error
}

void inspect_confirm_perturb(InspectState &s) {
    if (!s.perturb_pending)
        return;
    // The user accepted the page-dirty / timing / arm64-kill consequence. Re-run
    // the start through the SAME path with the one-shot confirm set, so the
    // budget/swap gate below still applies (a confirmed perturb is not a
    // confirmed swap). doc 45 T3: perturb_via_launch says which path armed it.
    s.perturb_confirmed = true;
    if (s.perturb_via_launch)
        inspect_request_launch(s);
    else
        inspect_request_start(s);
    s.perturb_confirmed = false;
}

void inspect_confirm_swap(InspectState &s) {
    if (!s.swap_pending)
        return;
    // Same wire-safety backstop as inspect_request_start, kept SYMMETRIC so the
    // guarantee never rests on the can-gated button alone: never stop the blocker's
    // capture to start an illegal tree filter (which would then bounce, leaving the
    // jack empty). Leave swap_pending up so the confirm — and the inline filter
    // error — stay visible until the filter is fixed or the swap is cancelled.
    if (s.want == LiveMode::Tree &&
        !obs_tree_filter_error(s.observer.filter).empty())
        return;
    // Stopping someone else's capture is a real consequence, which is why this
    // is a separate, explicit action rather than something inspect_request_start
    // does on the user's behalf.
    s.session.send_stop();
    s.active.clear();
    s.swap_pending = false;
    // Carry the SAME params the direct Start would (region / tree filter / --steps).
    // The swap fires s.want — exactly what the confirm names ("start <want>?") — so
    // inspect_start_params(s) is the right snapshot. Dropping it here is how a
    // swapped-in tree used to lose its whole filter and capture the world instead.
    s.session.send_start(mode_name(s.want), s.selected_pid,
                         inspect_start_params(s));
    s.active.push_back(s.want);
    s.awaiting_started = true; // 39 T5: the swap's new start is in flight
}

void inspect_arm_queue(InspectState &s) {
    s.has_queued = true;
    s.queued_want = s.want;
    // Snapshot the params NOW. The queue is locked to queued_want and fires later
    // when the jack frees; re-reading the picker at fire time would attach whatever
    // mode/region/filter the user has since moved to. This freezes what was queued.
    s.queued_params = inspect_start_params(s);
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

    if (!st.last_err.empty()) {
        ImGui::TextColored(kBad, "refused: %s", st.last_err.c_str());
        // A refusal that names a gate (a stale asmspy -> `make cli`) gets its fix.
        draw_command_hint("refused-cmd", remedy_command(st.last_err));
    }
    if (st.skip_code) {
        // A skip grades NEUTRAL (schema:98/544): the tracer worked and had
        // nothing to report, so it is a quiet chip, never an amber banner. The
        // measured reason is the whole payload and still renders in full (D7).
        char buf[512];
        std::snprintf(buf, sizeof buf, "skipped (%d): %s", st.skip_code,
                      st.skip_reason.c_str());
        draw_fidelity_chip(buf, FidelityTier::Neutral);
        // A perf-gated skip (IBS/sampler: "needs perf_event_paranoid<=2 ...")
        // carries the sysctl fix; a plain idle-window skip carries none.
        draw_command_hint("skip-cmd", remedy_command(st.skip_reason));
    }
    if (st.paused_dropped) {
        // paused_dropped -> CAUTION (a usable prefix with a recorded gap).
        char buf[192];
        std::snprintf(buf, sizeof buf,
                      "%llu event(s) dropped while paused — this recording is "
                      "truncated (usable, but incomplete)",
                      (unsigned long long)st.paused_dropped);
        draw_fidelity_chip(buf, FidelityTier::Caution);
    }
    if (s.session.malformed_lines()) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "%llu unparseable line(s) from the host",
                      (unsigned long long)s.session.malformed_lines());
        draw_fidelity_chip(buf, FidelityTier::Caution);
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
        // An idle-window retry (39 T3) is neither strong nor weak EVIDENCE — it
        // is a "nothing this window, re-sampling" note, so it grades CAUTION, not
        // the green of a real entry pick.
        bool cautious = pick_is_idle_window(p) || pick_is_weak_evidence(p);
        ImGui::TextColored(cautious ? kMaybe : kGood, "%s",
                           pick_evidence_label(p).c_str());
    }

    size_t nrec = s.session.recordings().size();
    if (const Recording *g = s.session.growing()) {
        ImGui::Text("capturing: %llu event(s) so far",
                    (unsigned long long)g->event_count());
        // The uniform busy signal (23 T4): a growing live recording is unbounded
        // — no `end` footer, no real total — so the LongOp gets the
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
            draw_fidelity_banner("TORN recording — the host stopped before "
                                "writing its footer; this capture is incomplete",
                                FidelityTier::Integrity);

    // The persistent, cause-distinguished END-OF-SESSION placard (23 T2, F20).
    // Co-located with the last events, it fans the single collapsed `Ended` into
    // its four causes and — for a protocol mismatch — carries the verbatim
    // one-line fix. It persists across frames (unlike a toast) until the next
    // Connect/Start. Graded through T1's components: torn/mismatch are integrity,
    // a clean stop is neutral.
    if (st.state == LiveState::Ended) {
        EndCause cause = end_cause(end_facts_of(s.session));
        ImGui::SeparatorText(end_cause_title(cause));
        FidelityTier tier = end_cause_is_integrity(cause) ? FidelityTier::Integrity
                                                         : FidelityTier::Neutral;
        std::string msg = end_cause_message(cause);
        draw_fidelity_banner(msg.c_str(), tier);
        std::string fix = end_cause_fix(cause);
        if (!fix.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kGood);
            ImGui::TextWrapped("%s", fix.c_str());
            ImGui::PopStyleColor();
            // The protocol-mismatch fix names `make cli` — offer it copy-ready.
            draw_command_hint("endfix-cmd", remedy_command(fix));
        }
    }
}

// Join a mode's visualizations into one "Slice, Loom, …" line (R11).
static std::string viz_line(LiveMode m) {
    std::string o;
    for (const char *v : mode_visualizations(m))
        o += (o.empty() ? "" : ", ") + std::string(v);
    // An empty list is a FACT about the mode, not a missing string: `stream`
    // records events no view in this build reads. Say that, rather than trail
    // off after "shows:" — a blank there reads as a bug in the picker.
    if (o.empty())
        o = "nothing — this capture records events no view in this build reads";
    return o;
}

// The patch bay: one jack per target, and what is in it.
void draw_patch_bay(InspectState &s) {
    ImGui::SeparatorText("patch bay — one ptrace jack per target");
    // `auto` leads the picker (R11): patch_bay_order() is the reading order, with
    // the fullest self-scoping choice first — distinct from the wire order.
    //
    // The jacks wrap. This pane is the docked right rail (0.28 of the window by
    // default), and a bare SameLine() ran the later jacks — `dataflow`, the
    // widest names — straight off it, where nothing could scroll them back.
    bool first_jack = true;
    for (LiveMode m : patch_bay_order()) {
        if (!first_jack)
            flow_same_line(flow_radio_w(mode_name(m)));
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
            // The tooltip names what the jack does AND what the mode lets you see
            // (R11), so a pick is not a leap of faith.
            std::string viz = viz_line(m);
            if (hazard)
                ImGui::SetTooltip("%s\n\nshows: %s\n\narm64: %s",
                                  mode_jack_reason(m), viz.c_str(),
                                  mode_perturb_warning(m, s.target_arch).c_str());
            else
                ImGui::SetTooltip("%s\n\nshows: %s", mode_jack_reason(m),
                                  viz.c_str());
        }
        first_jack = false;
    }
    // For the SELECTED mode, spell the visualizations inline (R11) — the tooltip is
    // per-mode discovery; this is the standing answer to "what will I get?".
    ImGui::TextDisabled("shows: %s", viz_line(s.want).c_str());
    // Least-perturbing default (T5): steer the picker off a single-step mode
    // toward the lightest substrate the host supports. `sample_available` is set
    // by the shell from the capability probe; false under the null backend, where
    // Log is already the default.
    if (!s.want_defaulted) {
        s.want_defaulted = true;
        s.want = budget_least_perturbing(s.sample_available);
    }
    // `continuous` follows the mode until the operator sets it themselves: an
    // `auto` capture re-arms by default, a named region does not.
    inspect_apply_continuous_default(s);

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
    // time-travels. On by default: the GETREGS it rides on already happens every
    // step, so arming the ring is a bounded-memory retain, not extra perturbation.
    // The checkbox lets a session opt back out.
    if (s.want == LiveMode::Dataflow || s.want == LiveMode::Auto) {
        ImGui::Checkbox("record the register ring (--steps → live Scrubber)",
                        &s.steps);
        // 35 T4: keep re-arming the same region until Stop, into one growing
        // recording (the Scrubber follows the latest invocation live). One start
        // fires; the once-per-session perturb confirm is not re-asked per pass.
        // ON by default for `auto` (inspect_apply_continuous_default, called
        // above) — until the operator moves it, which pins their choice.
        if (ImGui::Checkbox(
                "continuous — re-arm and keep capturing until Stop (35)",
                &s.continuous))
            s.continuous_touched = true;
        // 39 T3: the `auto` sample window (ms) — how long the out-of-band sampler
        // watches before ranking a region. Only `auto` samples; a named dataflow
        // region needs no window. 0 leaves the host default (400 ms) and sends no
        // `ms`; an empty window is retried, not a verdict (the host re-samples a
        // bounded number of times), so a wider window is a knob, not a fix.
        if (s.want == LiveMode::Auto) {
            ImGui::SetNextItemWidth(160.0f);
            ImGui::InputInt("auto sample window (ms, 0 = host default 400)",
                            &s.window_ms);
            if (s.window_ms < 0)
                s.window_ms = 0;
            if (s.window_ms > 60000)
                s.window_ms = 60000; // mirror the host's 1..60000 ms bound
        }
    }

    // The tree filter (depth/focus/module/tid/follow): a `tree` capture's engine-
    // side bound on what it EMITS, edited here beside the mode picker exactly as
    // the region (trace/dataflow) and --steps/continuous (dataflow/auto) are. The
    // one Start below carries it (inspect_start_params). To narrow a RUNNING tree,
    // Stop first (one jack — a re-Start is budget-blocked otherwise). The call-tree
    // PREVIEW lives in the Observer deck's Tree tab. tree_err gates Start (and
    // Swap/Queue) while the filter is illegal.
    std::string tree_err;
    if (s.want == LiveMode::Tree)
        tree_err = draw_tree_filter(s.observer);

    // 39 T5: reconcile the patch bay's belief against sessions that ended on
    // their OWN (one-shot `auto`, hit `max`, target exited). Every `active`
    // mutation elsewhere is a user action, so NOTHING freed the jack for a
    // self-end — the pane held [Auto] forever and budget-refused every Start,
    // offering only a Swap whose stop the host then refused (the very knot 39
    // fixes end to end). Key on the terminal-event COUNT, not on state==Idle
    // (which also holds in the gap between a Start and its `started` reply, and
    // would spuriously clear a just-started session). Done HERE, before the queue
    // poll, so a queued want sees the freed jack in the same frame. The serve host
    // runs ONE ptrace capture at a time, so a terminal event frees whatever ptrace
    // mode held the jack; free views were never on it.
    inspect_reconcile_self_end(s, s.session.status());

    // The Queue path (23 T3): a queued want starts the MOMENT the jack frees —
    // never an auto-swap, because budget_queue_ready is true only when the jack is
    // genuinely free (the blocker stopped/ended). The one-ptrace-jack invariant is
    // never bypassed. Polled here, before we render the state below.
    if (s.has_queued && budget_queue_ready(s.queued_want, s.active)) {
        s.session.send_start(mode_name(s.queued_want), s.selected_pid,
                             s.queued_params);
        s.active.push_back(s.queued_want);
        s.awaiting_started = true; // 39 T5: the queued start is in flight
        s.has_queued = false;
    }

    // 59 T1: the substrate sweep, polled on the same freed jack and after the
    // Queue — an explicitly queued want is the operator's own next choice and
    // outranks an automated sequence.
    inspect_sweep_poll(s);

    BudgetDecision d = budget_can_start(s.want, s.active);
    // BUDGET BLOCK (F23): "BLOCKED — jack held by <session> on <target>" — a
    // FACT, read BLOCKED not "paused", so it never reads as the operator pause.
    if (!d.allowed) {
        ImGui::TextColored(kMaybe, "%s on pid %ld",
                           budget_blocked_label(d).c_str(), s.selected_pid);
        ImGui::TextWrapped("%s", d.reason.c_str());
    }

    // What stage are we at? Only buttons that can act at this stage stay enabled
    // (R4): Start needs a host + target (+ region); Stop needs a live/paused
    // capture; Pause needs a running-and-not-already-paused one; Resume needs an
    // operator pause. A greyed button never fires a command the serve loop would
    // just refuse.
    const LiveStatus &st = s.session.status();
    const bool running =
        s.session.growing() != nullptr || st.state == LiveState::Running;
    const bool can = s.host_started && s.selected_pid > 0 && has_region &&
                     tree_err.empty();
    const LiveControls ctl =
        live_controls(can, running, s.operator_paused, !s.active.empty());

    ImGui::BeginDisabled(!ctl.start);
    if (ImGui::Button("Start"))
        inspect_request_start(s);
    ImGui::EndDisabled();
    if (!ctl.start) {
        // The tree-filter case sits before the region one: a `tree` needs no
        // region, so has_region is already true — tree_err is the only thing left
        // that can hold Start. The inline red error says WHAT is wrong; this says
        // WHERE to fix it.
        const char *why = !s.host_started     ? "connect a serve host first"
                          : s.selected_pid <= 0 ? "select a process first"
                          : !tree_err.empty()   ? "fix the tree filter above"
                                                : "name a region above, or pick "
                                                  "`auto`";
        // Measured before it is drawn, and wrapped when it will not fit: in the
        // docked right rail this sentence is wider than the whole pane, let
        // alone what is left of Start's line.
        flow_same_line(flow_text_w(why));
        ImGui::TextDisabled("%s", why);
    }
    flow_same_line(flow_button_w("Stop"));
    ImGui::BeginDisabled(!ctl.stop);
    if (ImGui::Button("Stop")) {
        s.session.send_stop();
        s.active.clear();
        s.operator_paused = false;
    }
    ImGui::EndDisabled();
    flow_same_line(flow_button_w("Pause"));
    // OPERATOR PAUSE (F23), split from the budget block: this hold's ONLY
    // recovery is Resume, and the state says so — "PAUSED (you)".
    ImGui::BeginDisabled(!ctl.pause);
    if (ImGui::Button("Pause")) {
        s.session.send_pause(true);
        s.operator_paused = true;
    }
    ImGui::EndDisabled();
    flow_same_line(flow_button_w("Resume"));
    ImGui::BeginDisabled(!ctl.resume);
    if (ImGui::Button("Resume")) {
        s.session.send_pause(false);
        s.operator_paused = false;
    }
    ImGui::EndDisabled();
    if (s.operator_paused)
        ImGui::TextColored(kMaybe,
                           "PAUSED (you) — emission is suspended (tracing is "
                           "not); the only recovery is Resume.");

    // 59 T1: the substrate sweep. Each 3D substrate comes from a DIFFERENT
    // engine and the host runs one at a time, so no single capture can fill
    // more than one. This runs them back to back into one recording.
    ImGui::Separator();
    {
        const std::string why = inspect_sweep_blocked(s);
        // Derived from the same sweep_legs the poll walks, so the line under the
        // button can never describe a sequence other than the one that runs.
        const std::string plan = sweep_plan(
            s.sweep_running ? s.sweep_have_region : s.region[0] != '\0',
            s.sweep_max);
        ImGui::BeginDisabled(!why.empty());
        if (ImGui::Button("Capture every substrate"))
            inspect_sweep_start(s);
        ImGui::EndDisabled();
        flow_same_line(s.sweep_running ? flow_button_w("Cancel sweep")
                                       : flow_text_w(plan.c_str()));
        if (s.sweep_running) {
            if (ImGui::Button("Cancel sweep"))
                inspect_sweep_cancel(s);
        } else {
            ImGui::TextDisabled("%s", plan.c_str());
        }
        if (!why.empty())
            ImGui::TextDisabled("%s", why.c_str());
        if (!s.sweep_note.empty())
            ImGui::TextColored(s.sweep_running ? kMaybe : kGood, "%s",
                               s.sweep_note.c_str());
        ImGui::TextDisabled(
            "The divergence worldline is not among them: it compares two "
            "RECORDINGS, so no capture sequence can produce it. Sweep twice "
            "and attach the second file as B (`d`).");
    }

    // The arm64 kill confirm (T5), now the ONLY pre-commit gate: it fires just for
    // the case that can silently TERMINATE the target (a single-stepped thread in
    // a blocking syscall on arm64, which detach cannot undo). Every other Start
    // began immediately. Only the second click proceeds.
    if (s.perturb_pending) {
        ImGui::Separator();
        ImGui::TextColored(kBad, "%s", s.perturb_reason.c_str());
        // "Start anyway" FIRES the start, so it obeys the same config gate as Start
        // itself — a filter edited to illegal while this confirm is up must not
        // slip through (inspect_request_start backstops it too, but a greyed button
        // is the faithful signal).
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Start anyway (accept the risk)"))
            inspect_confirm_perturb(s);
        ImGui::EndDisabled();
        flow_same_line(flow_button_w("Cancel##perturb"));
        if (ImGui::Button("Cancel##perturb"))
            s.perturb_pending = false;
    }

    // The budget block's THREE explicit recoveries (F23): Swap (a named two-step
    // confirm), Queue (a visible cancellable chip), Cancel. Never an auto-swap.
    if (!d.allowed) {
        ImGui::Separator();
        // Swap and Queue both START the configured capture (now, or when the jack
        // frees), so a config that could not be Started — illegal tree filter,
        // missing region — must not be Swappable/Queueable either: gate them on the
        // same `can`. Queue in particular SNAPSHOTS the params here, so gating the
        // arm is what keeps the frozen snapshot legal. Cancel stays live so a
        // blocked user is never trapped.
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Swap")) {
            // Arm the two-step confirm — it names WHAT detaches. Never silent.
            s.swap_pending = true;
            s.swap_blocker = d.blocker;
            s.swap_reason = d.reason;
        }
        flow_same_line(flow_button_w("Queue"));
        if (ImGui::Button("Queue"))
            inspect_arm_queue(s);
        ImGui::EndDisabled();
        flow_same_line(flow_button_w("Cancel##block"));
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
        // The actual fire point — gate it on `can` too, since the filter can be
        // edited illegal between arming the confirm and clicking it.
        ImGui::BeginDisabled(!can);
        if (ImGui::Button("Stop it and start this one"))
            inspect_confirm_swap(s);
        ImGui::EndDisabled();
        flow_same_line(flow_button_w("Cancel##swap"));
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
        draw_fidelity_chip(chip, FidelityTier::Neutral);
        flow_same_line(flow_small_button_w("Cancel queue"));
        if (ImGui::SmallButton("Cancel queue"))
            s.has_queued = false;
    }
}

// Lazily (re)build the InspectState observer over the session's growing (or last
// completed) recording and return it — the seam the live views build on. Rebuilt
// only when the event count moves: the builders are pure and cheap, but per-frame
// work on a live stream is how a UI starts costing the thing it is watching.
// Returns nullptr when there is nothing captured.
const Recording *live_observer_build(InspectState &s) {
    const Recording *live = s.session.growing();
    const std::vector<Recording> &done = s.session.recordings();
    if (live == nullptr && !done.empty())
        live = &done.back();
    if (live == nullptr)
        return nullptr;
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
    return live;
}

// The live views over whatever this session has produced (the windowed stack; the
// docked shell renders the deck in its own Observer pane). The tree-capture filter
// moved to the patch bay (draw_tree_filter, beside the mode picker), so the live
// views are pure views now — no capture control lives here.
void draw_live_views(InspectState &s) {
    const Recording *live = live_observer_build(s);
    if (live == nullptr) {
        ImGui::TextDisabled(
            "no capture yet — start a mode above and its view appears here");
        return;
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
    flow_same_line(flow_button_w("Browse…##save"));
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

// The full single-window stack (the windowed shell's Inspect tab + the render-
// only viewer): the patch bay, the session log, the live views, save and the PT
// slice, one under another. The DOCKED shell splits these across the Live-capture
// / Log / Save panes instead (draw_capture_pane below draws only the controls);
// this keeps the non-docked path exactly as it was so test_inspect / test_ui
// exercise the whole surface in one window.
void draw_capture_full(InspectState &s) {
    if (!s.host_started) {
        ImGui::TextDisabled(
            "connect a serve host (the Connect pane) to capture.");
        return;
    }
    if (s.selected_pid <= 0)
        ImGui::TextDisabled(
            "pick a process in the Processes pane, then pick a mode below.");
    draw_patch_bay(s);
    draw_status(s);
    ImGui::SeparatorText("live views");
    draw_live_views(s);
    draw_save_capture(s);
    // The PT slice is a docked pane of its own (draw_pt_slice_pane) and appears
    // ONLY on an Intel PT host; the single-window stack honours the same gate so
    // there is no surface that offers it off a PT host.
    if (inspect_pt_host_available())
        draw_pt_slice(s);
}

} // namespace

// Public entry points for the docked shell's split panes (the Log and Save tabs).
// They forward to the SAME anonymous bodies the windowed stack draws, so the two
// paths cannot drift.
void draw_session_status(InspectState &s) { draw_status(s); }
void draw_save_pane(InspectState &s) { draw_save_capture(s); }
void draw_pt_slice_pane(InspectState &s) { draw_pt_slice(s); }

// The PT slice pane's context gate: can a live Intel PT capture be started on
// this host? Only then is there ever a hardware-recorded path for the slice to
// replay — so the tab shows on a PT host and nowhere else. Pure verdict from
// ptslice_gate (tested with synthetic facts); this samples the real host.
bool inspect_pt_host_available() {
    return ptslice_gate(ptslice_facts()).can_capture;
}

// Is there a capture to act on (a growing one, or a completed one this session)?
// Gates the Save pane's context and the docked capture pane's 3D handoff.
bool inspect_has_capture(const InspectState &s) {
    return s.session.growing() != nullptr || !s.session.recordings().empty();
}

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
        ImGui::TextDisabled("To save these across launches, set them in File > "
                            "Settings; this pane is a per-session override.");
        // Record the WHOLE session (59 T1 / Task 8). Each capture is its own
        // in-memory Recording and Save writes exactly one, so this file is the
        // only way to get a call tree, a region trace and wide register writes
        // into ONE recording — which is what lets the 3D pane offer four
        // substrates at once when the file is reopened.
        ImGui::Checkbox("record the whole session to one file",
                        &s.record_session);
        if (s.record_session) {
            ImGui::InputTextWithHint("##record_path", "/path/to/session.asmtrace",
                                     s.record_path, sizeof s.record_path);
            ImGui::TextDisabled(
                "Every capture in this session is teed into that one file. "
                "Reopen it with File > Open and a single recording can host "
                "the address plane, the invocation stack, the module ribbon "
                "and the lane prism together. Local only — over ssh the file "
                "would be written on the remote host.");
            if (s.ssh_host[0] != '\0')
                ImGui::TextColored(kMaybe,
                                   "an ssh host is set: Connect will refuse "
                                   "this combination rather than write a file "
                                   "you cannot open");
        }
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
        flow_same_line(
            flow_text_w("stop the serve host and return to Connect"));
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
    // Sampling activity costs a ~150ms window, so it is gated on the "activity"
    // column being the sort (want_cpu_sort, set a frame earlier by the sort block
    // below). Honor a change in that intent here, before anything reads s.rows.
    if (s.want_cpu_sort != s.sample_cpu) {
        s.sample_cpu = s.want_cpu_sort;
        s.scanned = false; // fall through to the (re)scan just below
    }
    if (!s.scanned)
        inspect_scan(s);
    if (ImGui::Button("Rescan"))
        inspect_scan(s);
    const int yama = read_yama_scope();
    flow_same_line(flow_textf_w("ptrace_scope=%d", yama));
    ImGui::TextDisabled("ptrace_scope=%d", yama);
    if (s.sample_cpu) {
        flow_same_line(
            flow_text_w("· activity = %CPU/core over a ~150ms sample"));
        ImGui::TextDisabled("· activity = %%CPU/core over a ~150ms sample");
    }

    // Hide the definite refusals (on by default). Everything the picker could
    // only refuse — another user's process, a kernel thread, an i386 tracee, a
    // target something else already traces — is dropped from the table, and the
    // count of what that dropped is stated right here: a hidden row is a stated
    // absence, never a silent one (D7). `maybe` rows stay, because a fact we
    // could not read is not a refusal.
    size_t n_hidden = 0;
    for (const ProcRow &r : s.rows)
        if (r.verdict.verdict == Attach::No)
            ++n_hidden;
    ImGui::Checkbox("only attachable", &s.hide_unattachable);
    if (s.hide_unattachable && n_hidden) {
        flow_same_line(flow_textf_w("· %zu NOT-attachable row(s) hidden — "
                                    "untick to see them with their why / "
                                    "remedy",
                                    n_hidden));
        ImGui::TextDisabled("· %zu NOT-attachable row(s) hidden — untick to see "
                            "them with their why / remedy",
                            n_hidden);
    }

    // Type-to-narrow (24 T4's shared filter) over pid / comm / cmdline — the
    // /proc list is the one client-side table that never had a filter (doc 16's
    // framing). Build the haystack once; the matcher + "showing N of M" are pure.
    // The haystack covers only the rows the table can show, so "showing N of M"
    // counts what the filter narrowed, not what the attachability gate removed —
    // that count is its own line above.
    std::vector<std::string> hay(s.rows.size());
    std::vector<char> shown(s.rows.size(), 0);
    std::vector<std::string> hay_shown;
    for (size_t i = 0; i < s.rows.size(); ++i) {
        const ProcRow &r = s.rows[i];
        if (s.hide_unattachable && r.verdict.verdict == Attach::No)
            continue;
        shown[i] = 1;
        hay[i] = std::to_string(r.pid) + " " + r.comm + " " + r.cmdline;
        hay_shown.push_back(hay[i]);
    }
    std::string q = dt_filter_bar(s.proc_filter, hay_shown, "filter");
    ImGui::TextDisabled("double-click a row to attach & trace at full detail; "
                        "right-click for more.");

    // The picker is now a full table: columns Resize by dragging their edge,
    // Reorder by dragging their header, and Sort by clicking it. The sort is the
    // 24-T4 free-column-sort idiom — reorder our own VIEW indices only; the model
    // (list_processes(), pid-sorted) is never touched (D4/D7). The type-to-narrow
    // filter above still applies on top, over whichever order is showing.
    // Column indices, shared by the sort switch and the activity gate below.
    enum { kColPid = 0, kColComm, kColActivity, kColAttach, kColWhy };
    if (ImGui::BeginTable("procs", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable |
                              ImGuiTableFlags_Reorderable |
                              ImGuiTableFlags_Sortable)) {
        // pid is the default sort (ascending — the order the model already has),
        // and fixed-width so the numeric column does not stretch.
        ImGui::TableSetupColumn("pid", ImGuiTableColumnFlags_DefaultSort |
                                           ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("comm");
        // Clicking "activity" is what asks for the CPU sample (want_cpu_sort,
        // consumed at the top next frame); most-active-first is the useful order,
        // so it prefers to sort descending.
        ImGui::TableSetupColumn("activity",
                                ImGuiTableColumnFlags_PreferSortDescending |
                                    ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("attach");
        ImGui::TableSetupColumn("why / remedy");
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        // Reorder the row indices to match the clicked column. stable_sort keeps
        // ties in the model's pid order. pid sorts numerically; comm and the
        // reason sort case-insensitively; attach sorts by the Attach verdict enum
        // (Yes, No, Unknown), so attachable targets rise to the top ascending.
        std::vector<int> order(s.rows.size());
        for (size_t k = 0; k < order.size(); ++k)
            order[k] = static_cast<int>(k);
        // Default false so no-sort (unreachable given pid's DefaultSort, but
        // accurate) leaves the instant path; set true only while activity sorts.
        s.want_cpu_sort = false;
        if (ImGuiTableSortSpecs *ss = ImGui::TableGetSortSpecs();
            ss && ss->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs &c = ss->Specs[0];
            const bool asc = c.SortDirection == ImGuiSortDirection_Ascending;
            s.want_cpu_sort = (c.ColumnIndex == kColActivity);
            std::stable_sort(
                order.begin(), order.end(), [&](int a, int b) {
                    const ProcRow &ra = s.rows[static_cast<size_t>(a)];
                    const ProcRow &rb = s.rows[static_cast<size_t>(b)];
                    int cmp;
                    switch (c.ColumnIndex) {
                    case kColComm:
                        cmp = strcasecmp(ra.comm.c_str(), rb.comm.c_str());
                        break;
                    case kColActivity: // CPU used over the sample window
                        cmp = (ra.cpu < rb.cpu)   ? -1
                              : (ra.cpu > rb.cpu) ? 1
                                                  : 0;
                        break;
                    case kColAttach: // by attachability (the verdict enum)
                        cmp = static_cast<int>(ra.verdict.verdict) -
                              static_cast<int>(rb.verdict.verdict);
                        break;
                    case kColWhy:
                        cmp = strcasecmp(ra.verdict.why.c_str(),
                                         rb.verdict.why.c_str());
                        break;
                    default: // pid — numeric
                        cmp = (ra.pid < rb.pid)   ? -1
                              : (ra.pid > rb.pid) ? 1
                                                  : 0;
                        break;
                    }
                    return asc ? cmp < 0 : cmp > 0;
                });
        }

        // Turn the sampled jiffies into a %CPU of one core over the ~150ms
        // window (jiffies / (window_s * ticks_per_s) * 100). One busy core reads
        // ~100%, two ~200% — the same shape top shows.
        const double clk_tck = static_cast<double>(sysconf(_SC_CLK_TCK));
        for (int oi : order) {
            const size_t i = static_cast<size_t>(oi);
            if (!shown[i]) // hidden by the attachability gate above
                continue;
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
                    // Connect from saved Settings; reveal Connect only if it fails.
                    if (!s.host_started)
                        inspect_connect(s);
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
            // Activity is a MEASURED quantity: show "—" when this list was not
            // sampled (the instant pid/comm/attach path), never "0%", which would
            // claim a zero the code never took. A genuinely idle process in a
            // sampled list does read 0%, and that is accurate — it was measured.
            if (!s.sample_cpu) {
                ImGui::TextDisabled("—"); // an absence is never barred (#1)
            } else {
                const double pct =
                    clk_tck > 0 ? 100.0 * (double)r.cpu / (0.150 * clk_tck) : 0.0;
                // %CPU is a neutral ranking — a dim in-cell magnitude bar (#1),
                // number kept on top. The scale is fixed [0, 100%].
                dt_cell_magnitude_bar(dt_magnitude_frac(pct, 100.0),
                                      dt_dim_u32());
                ImGui::Text("%.0f%%", pct);
            }
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
            // When the gate has a one-line fix (scope 1 -> the yama sysctl),
            // offer it copy-pasteable right here in the disabled attach's row.
            draw_command_hint(("attach-cmd-" + std::to_string(r.pid)).c_str(),
                              remedy_command(r.verdict.remedy));
        }
        ImGui::EndTable();
    }
}

// --- Live capture pane: the patch bay CONTROLS only ------------------------
// The docked shell's Live-capture tab. The session log moved to the Log pane
// (draw_session_status), save-to-.asmtrace to the Save pane (draw_save_pane), and
// the PT slice to its own PT-host-gated pane (draw_pt_slice_pane); the live views
// render in their own docked panes (the doc-25 live_tab mirror). So this tab is
// now just the controls: pick a mode, scope it, Start. The 3D overview is reached
// the way every other view is — its own tab, or the `5` keyroute — so no pane
// carries a second door to it.
void draw_capture_pane(InspectState &s) {
    if (!s.host_started) {
        ImGui::TextDisabled(
            "connect a serve host (the Connect pane) to capture.");
        return;
    }
    if (s.selected_pid <= 0)
        ImGui::TextDisabled(
            "pick a process in the Processes pane, then pick a mode below.");
    draw_patch_bay(s);
}

// --- Launch pane: fork+exec a fresh target, traced from birth --------------
// docs/internal/archive/gui/45-launch-and-window-target.md T4. A command field
// (Browse… mirrors draw_save_pane's ImGuiFileDialog use, no extension
// filter), arguments, an optional working directory, a mode picker RESTRICTED
// to the whole-process modes (Non-goals: "the natural v1 focus" — trace/watch
// need a region/addr this pane does not collect; a client that already knows
// one can still reach them over the wire `launch` command directly), and
// "Launch & trace". Deliberately its OWN small mode picker rather than a
// share of draw_patch_bay: that widget is a full Start/Stop/swap/perturb
// control surface keyed on an EXISTING selected_pid, not a mode radio row —
// forcing a shared widget over that mismatch would cost more than it saves.
namespace {
const LiveMode kLaunchModes[] = {LiveMode::Log,   LiveMode::Stream,
                                 LiveMode::Graph, LiveMode::Tree,
                                 LiveMode::Procs, LiveMode::Sample};
bool is_launch_mode(LiveMode m) {
    for (LiveMode k : kLaunchModes)
        if (k == m)
            return true;
    return false;
}
} // namespace

void draw_launch_pane(InspectState &s) {
    ImGui::TextWrapped(
        "Start a NEW process and trace it from its first instruction — "
        "rather than pick one already running (the Processes pane). The "
        "command runs wherever the serve host does (locally, or on the ssh "
        "host set in Connect).");
    ImGui::Spacing();

    ImGui::InputTextWithHint("command", "path to the executable", s.launch_cmd,
                             sizeof s.launch_cmd);
    flow_same_line(flow_button_w("Browse…##launch"));
    if (ImGui::Button("Browse…##launch")) {
        IGFD::FileDialogConfig cfg;
        cfg.path = ".";
        ImGuiFileDialog::Instance()->OpenDialog("dlg_launch_cmd",
                                                "Choose a command to launch",
                                                nullptr, cfg);
    }
    if (ImGuiFileDialog::Instance()->Display(
            "dlg_launch_cmd", ImGuiWindowFlags_NoCollapse, ImVec2(520, 360))) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::string p = ImGuiFileDialog::Instance()->GetFilePathName();
            std::snprintf(s.launch_cmd, sizeof s.launch_cmd, "%s", p.c_str());
        }
        ImGuiFileDialog::Instance()->Close();
    }
    ImGui::InputTextWithHint("arguments", "space-separated (no quoting in v1)",
                             s.launch_args, sizeof s.launch_args);
    ImGui::InputTextWithHint("working directory", "blank = the server's own",
                             s.launch_cwd, sizeof s.launch_cwd);

    ImGui::Spacing();
    ImGui::TextDisabled("trace mode:");
    bool first_launch_mode = true;
    for (LiveMode m : kLaunchModes) {
        if (!first_launch_mode)
            flow_same_line(flow_radio_w(mode_name(m)));
        first_launch_mode = false;
        if (ImGui::RadioButton(mode_name(m), s.want == m))
            s.want = m;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", mode_jack_reason(m));
    }
    if (!is_launch_mode(s.want))
        // The picker defaults to whatever budget_least_perturbing chose
        // (T5), which can be `sample` or `log` — both in kLaunchModes — but
        // a stale `want` from a prior Capture-mode dataflow/auto/trace/watch
        // pick would otherwise show none of the radios above as selected.
        s.want = LiveMode::Log;

    if (!s.host_error.empty())
        ImGui::TextColored(dt_bad_col(), "%s", s.host_error.c_str());

    // The perturbation confirm (T5, F22), armed only for `stream`/`graph`/
    // `tree` on an arm64 target — reuses the SAME InspectState fields
    // inspect_request_start's confirm does; perturb_via_launch says this one
    // armed it, so Confirm resumes through inspect_request_launch.
    if (s.perturb_pending && s.perturb_via_launch) {
        ImGui::Separator();
        ImGui::TextColored(dt_maybe_col(), "%s", s.perturb_reason.c_str());
        if (ImGui::Button("Launch anyway"))
            inspect_confirm_perturb(s);
        flow_same_line(flow_button_w("Cancel##launch-perturb"));
        if (ImGui::Button("Cancel##launch-perturb"))
            s.perturb_pending = false;
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(s.launch_cmd[0] == '\0');
    if (ImGui::Button("Launch & trace"))
        inspect_launch_full_detail(s);
    ImGui::EndDisabled();
    if (s.launch_cmd[0] == '\0') {
        flow_same_line(flow_text_w("(enter a command first)"));
        ImGui::TextDisabled("(enter a command first)");
    }
}

void draw_inspect_door(InspectState &s) {
    // The single-window composition (windowed shell + render-only viewer): the
    // four panes stacked, each under its own separator. The docked shell instead
    // Begins draw_connect_pane / draw_processes_pane / draw_launch_pane and
    // splits the capture surface across the Live-capture / Log / Save panes;
    // here it stays one full stack (draw_capture_full) so the non-docked path
    // loses nothing. Poll once per frame.
    s.session.poll();
    ImGui::SeparatorText("Connect");
    draw_connect_pane(s);
    ImGui::SeparatorText("Processes");
    draw_processes_pane(s);
    ImGui::SeparatorText("Launch");
    draw_launch_pane(s);
    ImGui::SeparatorText("Live capture");
    draw_capture_full(s);
    // The cross-pane reveal requests are docked-shell only — here all three are
    // already in the one Inspect tab, so consume (clear) them without action.
    s.want_open_connect = false;
    s.want_open_capture = false;
}

} // namespace asmdesk
