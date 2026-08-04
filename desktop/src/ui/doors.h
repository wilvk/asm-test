// doors.h — the Learn door's ImGui half (06-doors-and-learning.md T4).
//
// Same split as views_draw.h and loom_draw.h: everything decidable lives in the
// pure walkthrough.h model, which test_walkthrough drives headlessly. This
// header is the drawing and the file discovery, nothing else.
#ifndef ASMDESK_UI_DOORS_H
#define ASMDESK_UI_DOORS_H

#include <functional>
#include <string>
#include <vector>

#include "author_vm.h"
#include "capview.h"
#include "doc/recording.h"
#include "live/budget.h"
#include "live/inspect.h"
#include "live/procinfo.h"
#include "live/ptslice.h"
#include "live/session.h"
#include "ui/filter.h"   // dt_filter_state — the shared type-to-narrow filter (24 T4)
#include "ui/progress.h" // LongOp — the uniform busy signal (23 T4)
#include "views/observer_draw.h"
#include "walkthrough.h"

namespace asmdesk {

// One card in the Learn door's list.
struct LearnCard {
    std::string path;
    std::string id;
    std::string title;
    int stops = 0;
    bool truncated = false;
    std::string provenance; // "emu-l0 / exact" — the chip on the card
    std::string error;      // non-empty when the file would not load
};

struct LearnState {
    std::vector<LearnCard> cards;
    bool scanned = false;
    std::string dir;
    std::string scan_error;
    int open_card = -1;
    wt_model player;
};

// Where the bundled walkthroughs live: $ASMTRACE_LEARN_DIR if set, else the
// compiled-in default (the committed corpus's walkthroughs directory). An
// override is what lets a packaged build ship them beside the binary.
std::string learn_dir();

// Scan `dir` for *.asmtrace and build the card list. A file that will not load
// still gets a card, carrying its loader error — a walkthrough that vanished is
// something the user must see, not something the door silently omits.
void learn_scan(LearnState &s, const std::string &dir);

// Draw the door. `go` is called with (recording path, step) when an anchored
// stop is selected, so the shell routes through 04's router rather than the
// door reaching into the views.
void draw_learn_door(LearnState &s,
                     const std::function<void(const std::string &, long)> &go);

} // namespace asmdesk

namespace asmdesk {

// The capability panel's state (06-doors-and-learning.md T6). `rows` is built
// once at open and on an explicit Refresh — never per frame, and never in the
// render-only viewer, which probes nothing.
struct CapState {
    std::vector<cap_row> rows;
    bool probed = false;
    bool native_only = false;
};

// The Author door's state. `source` is a std::string used as an ImGui text
// buffer, so it is reserved up front and never reallocated under the widget.
struct AuthorState {
    std::string source;
    int arch = 0;   // asm_arch_t; ASM_X86_64
    int syntax = 0; // asm_syntax_t; Intel is the header's default
    int nargs = 1;
    long args[6] = {2, 0, 0, 0, 0, 0};
    author_result_t result;
    size_t steps = 0;

    // The save path (18-breach-stops.md T3, F24). A run's output lived only here
    // and vanished on tab close; now it can be written to a real .asmtrace via
    // the same confirm-overwrite dialog the Inspect door uses. `image`/
    // `image_base` are the assembled bytes retained from the last run so the
    // saved recording carries the authored program (a `codeimage` event); `dirty`
    // marks an unsaved run (the tab title's `*` + the close guard); a successful
    // save clears it.
    std::vector<uint8_t> image;
    uint64_t image_base = 0;
    // The routine-identity hash of `image` (42 T1/T2), precomputed once here —
    // never in the draw path — so the Loom's reweave latch can compare it
    // against a recording's Streams::code_sha with a plain string equality.
    // Populated ONLY for an x86-64 run (the resume seam's scope, forks.h): an
    // arm64/other-arch image leaves this empty, which the latch already treats
    // as "nothing eligible" rather than needing its own arch check.
    std::string image_sha;
    // The entry args that ACTUALLY produced `image`/`image_sha` (42), frozen
    // alongside them at Run time. `args`/`nargs` above stay live, editable
    // ImGui widget state (the slider/inputs keep mutating them after a Run
    // with no re-Run) — a reweave must replay the args that made this exact
    // recording, not whatever the slider currently reads, or it silently
    // re-seeds entry registers with values that never produced it.
    long frozen_args[6] = {0, 0, 0, 0, 0, 0};
    int frozen_nargs = 0;
    bool dirty = false;
    char save_path[1024] = "authored.asmtrace";
    std::string save_status;
    bool saved_ok = false;

    // R5 T3 (32-per-guest-value-producer.md): the last run's value fabric,
    // held across frames like `image` above so a later Save can materialise
    // it into the recording. Populated only by an arm64 (per-guest producer)
    // run; empty (vf.ran == false) after an x86-64 run or before any Run.
    author_valuefabric_t vf;

    AuthorState() {
        source.reserve(64 * 1024);
        source = "mov rax, rdi\nimul rax, rdi\nret\n";
    }
};

void draw_author_door(AuthorState &s);

void cap_probe(CapState &s);
// `loaded` is used only by the render-only build, which shows the RECORDING's
// provenance in place of a host probe.
void draw_capability_panel(CapState &s, const Recording *loaded);

// --- the Inspect door (07-serve-live-host.md T4/T5) ------------------------
// The live front door: pick a process, see WHY NOT when you cannot, and land on
// a hot function via `--auto` with accurate evidence labels.
//
// It is in BOTH binaries, and that is the point of D9 rather than an oversight:
// the door links no engine at all. It reads /proc itself to list processes, and
// it captures by spawning `asmspy --serve` as a subprocess — so the
// render-only viewer hosts live sessions while its `ldd` stays engine-free.
struct InspectState {
    LiveSession session;
    bool host_started = false;
    std::string host_error;
    // The asmspy the host will spawn; blank = resolve $PATH then ./build.
    char asmspy_path[512] = {0};
    char ssh_host[256] = {0};

    // Pre-fill the asmspy path with resolve_asmspy_path() once, so the Connect
    // pane shows the concrete resolved exe (editable) rather than a blank field.
    bool asmspy_prefilled = false;

    std::vector<ProcRow> rows;
    bool scanned = false;
    long selected_pid = 0;
    // The Processes pane's type-to-narrow filter (24 T4's shared idiom) over
    // pid / comm / cmdline.
    dt_filter_state proc_filter;

    // The Process details pane's probe runner. It spawns `asmspy --info` —
    // which never attaches — so it needs no host, no budget and no jack, and
    // is driven purely by selected_pid.
    ProcInfoRunner details;

    // Hide the rows whose verdict is a definite Attach::No — a target the picker
    // could only refuse. ON by default: the useful list is the one you can act
    // on, and a machine's /proc is mostly other users' processes and kernel
    // threads. NOT a silent omission (D7): the hidden count and its reason are
    // stated beside the checkbox, and one click brings them back with their
    // per-row why / remedy intact. `maybe` (Attach::Unknown) rows are NEVER
    // hidden — "the deciding fact cannot be read from outside" is not a refusal.
    bool hide_unattachable = true;

    // The "activity" column ranks processes by CPU used over a short window, but
    // that sample is not free (list_processes(sample_cpu) briefly sleeps), so it
    // is gated on that column being the active sort: pid / comm / attach stay
    // instant. sample_cpu = the current rows carry a CPU sample; want_cpu_sort =
    // the table's sort column is "activity" (set by the sort block each frame,
    // read at the top of the next to (re)scan when the intent flips).
    bool sample_cpu = false;
    bool want_cpu_sort = false;

    // The scoped region for trace/dataflow (mode_needs_region): a func NAME or
    // "0xADDR:LEN". Sent as the `start` params (parse_region_spec); ignored by the
    // whole-process modes and `auto` (which finds its own region).
    char region[256] = {0};

    // docs/internal/archive/gui/45-launch-and-window-target.md T3: the Launch pane's
    // form — a command to fork+exec and trace from birth, instead of a pid
    // picked from a table. Fixed buffers (not std::string) to match every
    // other ImGui-editable text field in this struct (asmspy_path/ssh_host
    // above, region). `launch_cmd` is argv[0] (browsable via ImGuiFileDialog,
    // T4); `launch_args` is a single shell-like string split on whitespace
    // into argv[1..] when the launch fires (no quoting grammar in v1 — an
    // argument needing one is a rare enough case to not hold up the rest);
    // `launch_cwd` is optional (empty = inherit the server's cwd).
    char launch_cmd[512] = {0};
    char launch_args[512] = {0};
    char launch_cwd[512] = {0};
    // Set by inspect_launch_full_detail when it sends `launch`: there is no
    // pid to adopt until the `session started` reply names one, unlike an
    // attach (inspect_attach_full_detail already knows its pid before
    // sending). Consumed (and selected_pid adopted from LiveStatus::pid) by
    // inspect_reconcile_self_end the frame the host confirms Running.
    bool launch_awaiting_pid = false;

    // The per-step register RING (--steps, doc 26): armed for the dataflow single-
    // step engines (dataflow / auto) so the live Scrubber time-travels registers.
    // ON by default for those two modes: PTRACE_GETREGS already runs every step
    // regardless of this flag (dataflow_ptrace.c's cost model; doc 26 "not
    // hardware-gated... the reg file is already read") — arming the ring only
    // retains that value into a bounded buffer, not new perturbation. Set by the
    // "full detail" attach and a capture-pane checkbox, either of which can still
    // turn it off; sent as `steps:true` in the start params.
    bool steps = true;

    // CONTINUOUS capture (35 T4): the dataflow/auto engine re-arms the same scoped
    // region and keeps capturing until Stop, appending each invocation into one
    // growing recording delimited by `df_invocation` markers (the Scrubber shows
    // the latest pass, refreshing live). A capture-pane checkbox; sent as
    // `continuous:true`. Off by default (one invocation, then done). The
    // once-per-session perturb confirm already covers the whole session, so
    // continuous does NOT re-confirm per pass.
    //
    // Defaulted PER MODE by inspect_apply_continuous_default: ON for `auto`, off
    // for a named `dataflow` region. An `auto` capture picks its own region from
    // a sample window and a single invocation of it is typically over before the
    // operator has looked at anything, so "re-arm until Stop" is the useful
    // reading of "watch this process". The default re-applies on each entry into
    // a mode until the operator moves the checkbox themselves (continuous_touched),
    // after which their choice stands for the session.
    bool continuous = false;
    bool continuous_touched = false;
    LiveMode continuous_defaulted_for = LiveMode::Log; // the mode last defaulted

    // The `auto` SAMPLE WINDOW in ms (39 T3): how long the out-of-band sampler
    // watches before ranking a region. 0 means "unset" — the host's own default
    // (AUTO_WINDOW_MS, 400) is used and no `ms` is sent. Defaults to 2000: a
    // 400 ms window frequently sees nothing at all in a target that is only
    // intermittently in the region, and an empty window costs a retry rather
    // than producing a capture. Surfaced as a capture-pane input for `auto` only
    // (dataflow/trace name their region and sample nothing); sent as `ms`.
    int window_ms = 2000;

    // Cross-pane requests from the Processes pane's row actions (double-click /
    // right-click): the door cannot reach ShellState, so it raises a flag the
    // docked shell consumes to reveal the Connect / Live-capture pane. No-ops in
    // the single-window shell, where all three are already in the Inspect tab.
    bool want_open_connect = false;
    bool want_open_capture = false;
    // Entering Capture mode raises this so the docked shell connects the serve
    // host from the saved Settings (asmspy path / ssh host) and lands the user on
    // the Processes pane already attached — no forced Connect-pane detour. Set by
    // shell_select_mode, consumed once in draw_shell; a failed connect falls back
    // to revealing Connect (so its host_error is visible).
    bool want_autoconnect = false;
    // Raised alongside want_autoconnect when Capture mode is entered: the docked
    // shell brings the Processes pane forward (SetNextWindowFocus) so it wins its
    // dock node's tab bar rather than opening behind a peer (Connect / Live
    // capture). Consumed once in draw_shell; no-op in the single-window shell.
    bool want_focus_processes = false;
    // docs/internal/gui/45 T4: the Launch mode's counterpart to
    // want_focus_processes — brings kPaneLaunch forward instead of
    // Processes, and (unlike Capture) with no want_autoconnect alongside it.
    // Set by shell_select_mode, consumed once in draw_shell.
    bool want_focus_launch = false;

    // What the client believes is live on this target, for the patch bay. The
    // serve loop refuses a second concurrent start too, but the budget is
    // decided HERE so the UI can render an occupied jack and offer a swap
    // rather than firing a command that comes back as an error.
    std::vector<LiveMode> active;
    // 39 T5: the count of terminal `session` events already reconciled against
    // `active`. A capture that ends on its OWN (one-shot `auto`, hit `max`, target
    // exited) increments LiveStatus::sessions_ended, but NOTHING cleared the
    // patch bay's belief — every `active` mutation is a user action — so an idle
    // pane held [Auto] forever and budget-refused every Start. Tracking the count
    // lets the pane free the ptrace jack the frame a session self-ends (keyed on
    // the terminal EVENT, not on state==Idle, which also holds in the gap between
    // a Start and its `started` reply). Reset on Disconnect.
    uint64_t seen_sessions_ended = 0;
    // 39 T5: a start the desktop issued (Start / Swap / Queue) whose `started`
    // reply has not landed yet — the jack is "held" from the desktop's intent
    // even though the host has not confirmed. It stops the self-end reconcile
    // above from freeing a session between its send_start and its `started`
    // (the swap's stop+start pair is the case that needs it). Cleared once the
    // host resolves the start (state leaves Idle).
    bool awaiting_started = false;
    LiveMode want = LiveMode::Log;
    // Set when a start was blocked: the swap the user may confirm. A swap
    // stops someone else's capture, so it is never silent.
    bool swap_pending = false;
    LiveMode swap_blocker = LiveMode::Log;
    std::string swap_reason;

    // The split "paused" state (23-graded-truth-layer.md T3, F23). `operator_paused`
    // is set by the Pause button and cleared by Resume — it is the OPERATOR pause,
    // rendered "PAUSED (you)" and distinct from the budget BLOCK. The Queue path:
    // `has_queued` stashes `queued_want` as a visible cancellable chip when the
    // jack is held; when budget_queue_ready(queued_want, active) turns true (the
    // blocker stopped) the start fires automatically — never an auto-swap, since
    // Queue only fires on a genuinely free jack.
    bool operator_paused = false;
    bool has_queued = false;
    LiveMode queued_want = LiveMode::Log;
    // The start params snapshotted the moment Queue was armed (inspect_arm_queue),
    // replayed verbatim when the jack frees. Snapshotting is what keeps a queued
    // capture faithful: the queue is locked to `queued_want`, so its region / tree
    // filter / --steps must be locked to what was configured THEN, not re-read from
    // a picker the user may have moved on to something else. Empty for a whole-
    // process mode, exactly as inspect_start_params returns.
    nlohmann::json queued_params;

    // The uniform busy signal for the unbounded live stream (23 T4): its elapsed
    // clock + Cancel. `started_at` is re-armed to 0 between sessions so each
    // capture times from its own start.
    LongOp stream_op;

    // The perturbation gate (18-breach-stops.md T5, F22): arming a single-step
    // (mode_uses_ptrace) mode dirties the traced page, perturbs timing, and on
    // arm64 can kill a target blocked in a syscall — so the first Start ARMS a
    // pre-commit confirm (like swap above and the syscall reveal-all) and only
    // the second fires. `perturb_reason` is the pure mode_perturb_warning()
    // sentence; `perturb_confirmed` is the one-shot the confirm sets so the
    // re-entered inspect_request_start passes the gate. `target_arch` keys the
    // arm64 clause + the greyed/annotated single-step rows (best-effort from the
    // host; "" = unknown, still annotated, never a hidden refusal).
    bool perturb_pending = false;
    bool perturb_confirmed = false;
    // docs/internal/gui/45 T3: which gate armed perturb_pending — an attach
    // (inspect_request_start) or a launch (inspect_request_launch) — so
    // inspect_confirm_perturb resumes through the SAME path rather than
    // guessing; false (the default) preserves inspect_request_start's
    // pre-45 behaviour exactly.
    bool perturb_via_launch = false;
    std::string perturb_reason;
    std::string target_arch;
    // The picker defaults to the least-perturbing substrate the host supports
    // (T5). `sample_available` is set by the shell from the capability probe
    // (AMD IBS); `want_defaulted` makes the choice a ONE-TIME default so it never
    // fights a user who then picks a heavier mode on purpose.
    bool sample_available = false;
    bool want_defaulted = false;

    // The live Observer deck (08-observer-views.md) over this session's
    // recording. It is the SAME deck the replay tabs draw, rebuilt as the
    // recording grows — which is what makes "every view renders identically
    // from a recording" a build-enforced fact rather than an aspiration.
    ObserverState observer;
    uint64_t observed_events = 0; // the deck was built at this event count
    size_t observed_recordings = 0;

    // The PT-replay slice (08-observer-views.md T8). Held across frames because
    // running it is an explicit action: replaying a path costs real work, and a
    // view that re-ran it every frame would be charging for a picture nobody
    // asked to refresh.
    PtSliceResult ptslice;
    bool ptslice_ran = false;

    // Save this session's capture to a .asmtrace file — the one thing the live
    // host does not do on its own, because the desktop keeps recordings in
    // memory rather than on disk. `save_path` is the target; `saved_path` (set
    // on a successful save) is what "Open in Loom" hands back to the shell.
    char save_path[1024] = "capture.asmtrace";
    std::string save_status; // last save result, shown verbatim
    bool saved_ok = false;   // a save succeeded -> offer "Open in Loom"
    std::string saved_path;  // the file the last successful save wrote
    bool saved_statistical = false; // the saved capture was non-exact (no Loom)
    // A cross-door request: a path the shell should open into the Workspace and
    // show in the Loom. draw_shell consumes and clears it — the door cannot
    // reach ShellState, and should not. Empty = nothing pending.
    std::string open_request;
};

// Rescan /proc into `s.rows` (also called once on first draw).
void inspect_scan(InspectState &s);
// Start the serve host if it is not up. Records the failure in `host_error`.
void inspect_connect(InspectState &s);
// Tear the serve host down and forget the session, returning to the Connect
// form. The typed asmspy path / ssh host and the /proc scan survive — only the
// beliefs that a live host made true are cleared. No-op if none is up.
void inspect_disconnect(InspectState &s);
// Ask to start `s.want` on `s.selected_pid`, honouring the budget. Returns
// false and arms `perturb_pending` when the mode single-steps (T5), then
// `swap_pending` when the jack is occupied.
bool inspect_request_start(InspectState &s);
// docs/internal/gui/45 T3: `launch`'s counterpart to inspect_request_start —
// same tree-filter/perturb/budget gates, but sends `launch` (argv built from
// launch_cmd/launch_args/launch_cwd) with no pid, arming launch_awaiting_pid
// instead. A busy jack refuses cleanly (no swap offer — see doors.h). False
// if s.launch_cmd is blank.
bool inspect_request_launch(InspectState &s);
// Confirm the armed perturbation: re-run the start with the consequence
// accepted (T5). The budget/swap gate still applies afterwards. Resumes
// through inspect_request_launch when perturb_via_launch armed it.
void inspect_confirm_perturb(InspectState &s);
// Confirm the armed swap: stop the holder, then start what was refused.
void inspect_confirm_swap(InspectState &s);

// Arm the Queue: stash the current want AND a SNAPSHOT of its start params, so the
// auto-fire (when the jack frees) replays exactly what was configured at queue
// time — not whatever the picker holds later. Pure over InspectState; test_shell
// drives it.
void inspect_arm_queue(InspectState &s);

// The serve `start` params for the current want: a scoped region (func name or
// base+len, parse_region_spec) for trace/dataflow, plus `steps:true` when the
// register ring is armed on a dataflow/auto capture; empty for the whole-process
// modes. Pure over InspectState; test_shell drives it.
nlohmann::json inspect_start_params(const InspectState &s);

// Apply the per-mode default for `continuous` (see InspectState::continuous):
// ON for `auto`, off for every other mode. Applied once per entry into a mode —
// draw_patch_bay calls it each frame, inspect_attach_full_detail calls it after
// pinning `auto` so the start it fires immediately carries the same value the
// checkbox will show — and never after the operator has moved the checkbox
// themselves (continuous_touched). Pure over InspectState; test_shell drives it.
void inspect_apply_continuous_default(InspectState &s);

// 39 T5: reconcile the patch bay's `active` belief against a session that ended
// on its OWN (one-shot `auto`, hit `max`, target exited) — freeing the ptrace
// jack no user action would, so an idle pane stops holding [Auto] and refusing
// every Start. Keyed on the terminal-event COUNT (LiveStatus::sessions_ended),
// guarded so an in-flight start (a Swap's stop+start pair) is not freed before
// its `started` lands. Pure over (InspectState, LiveStatus); draw_patch_bay
// calls it each frame and test_shell drives it directly.
void inspect_reconcile_self_end(InspectState &s, const LiveStatus &st);

// The "full detail" attach (a Processes-pane double-click, or the right-click
// "Attach & trace"): capture `pid` at the fullest detail an un-named target
// allows — `auto` picks the hottest region and data-flows it, with the register
// ring armed, so the Loom / Slice / 3D / Scrubber all light. With a host up this
// arms the capture (the perturb confirm still gates the first single-step); with
// no host it selects the target and asks the shell to reveal the Connect pane.
void inspect_attach_full_detail(InspectState &s, long pid);

// docs/internal/archive/gui/45-launch-and-window-target.md T3: the Launch pane's
// "Launch & trace" — mirrors inspect_attach_full_detail's connect-then-start
// shape, but there is no pid yet (send_launch instead of send_start), and
// launch_awaiting_pid is armed so inspect_reconcile_self_end adopts
// selected_pid once the `started` reply names one. `s.launch_cmd` must be
// non-blank; a blank command is the caller's job to prevent (the Launch
// pane greys the button on it) rather than this function's to refuse
// silently.
void inspect_launch_full_detail(InspectState &s);

// The Inspect / live-capture workflow, split into dockable panes (the
// docked shell Begins each in its own window; draw_inspect_door stacks them
// all for the windowed / render-only path):
//   Connect     — the serve-host connection (asmspy path pre-filled + ssh host).
//   Processes   — the searchable /proc target picker (pid / comm / attach / why).
//   Launch      — fork+exec a NEW process and trace it from birth (doc 45 T4),
//                 the second way to arrive at a target.
//   Live capture — the patch bay CONTROLS (mode + region + Start). The session
//                  log and save-to-.asmtrace are their OWN panes
//                  (draw_session_status / draw_save_pane), the PT-replay slice is
//                  its own PT-host-gated pane (draw_pt_slice_pane), and the live
//                  views render in the doc-25 live-tab mirror panes.
void draw_connect_pane(InspectState &s);
void draw_processes_pane(InspectState &s);
void draw_launch_pane(InspectState &s);
void draw_capture_pane(InspectState &s);
// Process details (gui-process-details): a probe-only pane over `asmspy
// --info`, driven purely by InspectState::selected_pid and gated on the
// selection alone (its context gate is NOT pctx_capture's host_started —
// see pctx_details in shell.cpp).
void draw_details_pane(InspectState &s);

// The Log pane's session half (the colored session/refusal/skip/torn/end-cause
// log that used to sit inside the capture pane), the Save pane (save the session's
// capture to a .asmtrace + Open in Loom), and the PT slice pane (the def-use slice
// with zero single-steps of the target — hardware-recorded path, emulator-replayed
// values). All three forward to the same bodies the windowed stack
// (draw_inspect_door) draws inline.
void draw_session_status(InspectState &s);
void draw_save_pane(InspectState &s);
void draw_pt_slice_pane(InspectState &s);
// Is there a capture to act on (a growing one, or a completed one this session)?
// The Save pane's context gate.
bool inspect_has_capture(const InspectState &s);
// Is this an Intel PT host — a host where a live PT capture can be started, so
// there is ever a hardware-recorded path for the PT slice to replay? The pure
// verdict is ptslice_gate(ptslice_facts()).can_capture (tested with synthetic
// facts in test_obs_ptslice); this reads the real host. Gates the PT slice pane's
// context so that tab appears ONLY on a PT host, and the windowed stack's inline
// PT slice with it.
bool inspect_pt_host_available();

// The single-window composition of the three panes (the windowed shell's Inspect
// tab and the render-only viewer). The docked shell draws the three separately.
void draw_inspect_door(InspectState &s);

} // namespace asmdesk
#endif // ASMDESK_UI_DOORS_H
