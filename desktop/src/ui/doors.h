// doors.h — the Learn door's ImGui half (06-doors-and-learning.md T4).
//
// Same split as views_draw.h and loom_draw.h: everything decidable lives in the
// pure walkthrough.h model, which test_walkthrough drives headlessly. This
// header is the drawing and the file discovery, nothing else.
#ifndef ASMDESK_UI_DOORS_H
#define ASMDESK_UI_DOORS_H

#include <functional>
#include <string>
#include <unordered_set> // InspectState::proc_open — the process tree's open nodes
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
// render-only viewer, which never runs the engine capability SWEEP that
// builds them (perf_ok/perf_reason below are the one exception: a syscall,
// not a sweep, so they run everywhere).
struct CapState {
    std::vector<cap_row> rows;
    // Set once cap_probe has run; cleared by nothing except a fresh process --
    // an explicit Refresh click re-runs cap_probe but does not reset this
    // first. That means a disconnect and a later reconnect to a DIFFERENT
    // host does NOT by itself re-measure `rows` or perf_ok/perf_reason; only
    // Refresh does. That staleness was cosmetic while this only fed a panel a
    // human reads; now that perf_ok/perf_reason feed Task 4's mode-blocking
    // decision, it is worth stating plainly rather than leaving it implicit
    // in the panel's own "probed once at open" caption. (perf_probed below is
    // the field that actually protects the blocking decision from this: it is
    // re-derived every frame from the LIVE session, never latched like this.)
    bool probed = false;
    bool native_only = false;

    // Whether perf_event_open ACTUALLY opens on this host, measured once at connect
    // with the cheapest possible event. `perf_reason` is the errno's own text when
    // it does not. This is NOT asmtest_ibs_available(), which is a CPUID/sysfs
    // SUBSTRATE probe that reports available on a host where every perf_event_open
    // returns EACCES -- the exact defect this field exists to fix.
    bool perf_ok = false;
    std::string perf_reason;

    // Whether perf_ok/perf_reason describe the machine that will ACTUALLY run
    // asmspy for the CURRENT session -- distinct from `probed` above, which only
    // means "cap_probe has run at least once" (for the cascade `rows`, a fact
    // about this desktop's own hardware that never changes across reconnects).
    // cap_probe's syscall always measures the LOCAL machine (the one running
    // this desktop process); for a remote session (InspectState::ssh_host set,
    // inspect_connect: `spec.ssh_host = s.ssh_host`) that is the WRONG machine
    // -- asmspy runs elsewhere, and a local perf_event_open says nothing about
    // it, the exact hazard this file's own render-only-viewer comment warns
    // against for the engine sweep. shell.cpp therefore recomputes this every
    // frame from the live connection's locality instead of latching it like
    // `probed`: false must mean "block nothing, say nothing" -- Task 4's
    // mode_host_blocked only forms a verdict about a host this actually
    // measured.
    bool perf_probed = false;
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
    // Optional: record the WHOLE session — every capture, one file. Off by
    // default (a capture session should not write to disk unasked). This is
    // the only way to get `call` + `trace`/`coverage` + wide `df_step.ops` into
    // one Recording, because each capture is its own in-memory Recording and
    // Save serialises exactly one. Local only; see LiveSession::Spec.
    char record_path[512] = {0};
    bool record_session = false;
    // Two-step arm for "clear previous captures" (2026-08-10 simplified-LOD
    // spec): the first click only arms; any frame that does not confirm
    // disarms. Session-control UI state, never persisted.
    bool clear_prev_armed = false;

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

    // The session-level perf-capability probe (2026-08-06 final review,
    // post-review finding: the focus/selection dependency). `details` above
    // is gated on TWO things that need not ever become true: the Process
    // details pane's own window focus (details_pane.cpp sets `visible` from
    // ImGui::IsWindowFocused), and a real `selected_pid` (procinfo_tick
    // early-returns for `selected_pid <= 0`) — so an operator who never
    // focuses that exact pane, or the Launch pane's "needs no pid at all"
    // flow, never produces the `asmspy --info --json` verdict that feeds
    // mode_host_blocked (finding 8's fix). Both fail PERMISSIVE
    // (perf_probed stays false, which still means "block nothing"), so this
    // does not re-open #2/#8 — but Task 4's gate could never ARM in either
    // reachable flow.
    //
    // This runner exists solely to drive that one probe past both hazards:
    // `visible` is left at its struct default of `true` (shell.cpp is the
    // only place that ticks it, and nothing there ever sets it from ImGui
    // focus), and shell.cpp targets it at THIS PROCESS's own pid (getpid(),
    // always > 0) rather than InspectState::selected_pid, so it fires
    // whether or not anything has ever been selected. It is still
    // `asmspy --info --json` against the SAME asmspy binary/inode `details`
    // would use — the pid argument only selects which target's snapshot is
    // gathered and thrown away; the `host` object it carries alongside is a
    // fact about the binary, not about that target.
    ProcInfoRunner host_probe;

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

    // Nest the table by lineage (ProcRow::ppid) instead of listing pids flat,
    // so a browser and its content children read as one process rather than
    // nine unrelated ones. ON by default: a tree of roots IS a flat list, so
    // the setting costs a snapshot with no lineage nothing.
    //
    // A tree has exactly ONE order — parent, then children — so it can only
    // hold under the pid sort; clicking "activity" or "attach" asks for an
    // order the nesting cannot honour. Rather than silently ignore either the
    // sort or the checkbox, the checkbox greys with that reason and the table
    // goes flat. proc_sort_is_pid carries the sort column across frames the
    // same way want_cpu_sort above does, and for the same reason: the sort
    // specs only exist INSIDE BeginTable, and the checkbox is drawn above it.
    bool proc_tree = true;
    bool proc_sort_is_pid = true;

    // Which tree nodes are open. Starts EMPTY — everything collapsed — which
    // is a deliberate cost: measured on a 604-process desktop, a fully
    // collapsed tree shows 2 rows (3 with the attachable gate on), because
    // almost every process descends from pid 1. "Expand all" is therefore not
    // a convenience but the control that makes the collapsed default usable,
    // and it sits next to the tree checkbox rather than in a menu.
    //
    // Node ids are ProcTreeRow::node_id — a pid for a process, MINUS the pid
    // for its threads group, so one set holds both without collision.
    // Survives a Rescan on purpose: re-reading /proc must not re-close a tree
    // the operator just opened.
    std::unordered_set<long> proc_open;

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

    // 2026-08-06 plan, Task 8 (M12): two more flags the serve host has always
    // honoured (cli/asmspy.c's `mem`/`statediff` in serve_parse_start) that the
    // GUI never sent — measured: grep -c '"insns"|"mem"|"statediff"|"fpregs"'
    // over this door returned 0 (that grep also names `fpregs`, the XMM/MXCSR
    // deck opt-in, 31 R4 — it is OUT OF SCOPE for this task and still unsent;
    // do not read its presence in the measurement as coverage here). Both
    // `mem`/`statediff` are OPT-IN, unlike `steps` above, because both have a
    // real cost: `mem` adds one `mem` event per memory access over the whole
    // capture window, and `statediff` forces the register ring on (server-side
    // `p->steps = 1`) and adds one statediff + one regstate per step — against
    // a sweep leg hard-capped at only 400 (sweep_max below), that is up to 800
    // extra events for one leg. The capture-pane checkboxes state both costs;
    // see inspect_start_params for where they reach the wire.
    bool want_mem = false;
    bool want_statediff = false;
    // The BLAME cone (`--blame`): a single backward-attribution cone over the
    // def-use graph, seeded at the penultimate step of ONE invocation
    // (cli/asmspy.c:2512-2519's dataflow_emit_blame: gated `nsteps >= 1`, one
    // `rec_emit` per call — NOT one event per step; an earlier version of this
    // comment claimed "per step" and a live check proved it wrong: a 200-max
    // capture produced exactly 1 blame event, not ~80). DEFAULT ON, unlike
    // mem/statediff: the 3D pane's blame causal layer defaults ON
    // (scene3d/scene.h) and build_blame_forest runs every frame, so leaving
    // this off feeds that layer nothing — the exact defect this task closes —
    // and at a flat +1-per-invocation cost (cheaper than `mem`, no pricier
    // than `insns`, already unconditional above) there is no reason to make
    // the operator ask for it. The one real cost is `continuous` mode's pass
    // count (asmspy_engine.c:3753-3802's re-arm loop: `stop_loop =
    // !continuous`, unbounded until Stop, one blame event per pass) — NOT a
    // sweep leg, which forces `continuous = false` (inspect_sweep_poll) and so
    // pays a flat +1 regardless of sweep_max. The checkbox stays so a long
    // continuous `auto` capture can still turn it off.
    bool want_blame = true;

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
    // --- substrate sweep (59 T1) --------------------------------------------
    // One capture per 3D substrate, back to back, into ONE recording. The live
    // tab shows exactly one Recording (shell_sync_live_tab keeps a single slot
    // pointing at growing()-else-back()), so a sweep is only worth running with
    // `record_session` on: without it each leg lands in its own Recording and
    // the pane can still show only one substrate at a time.
    //
    // Each leg is BOUNDED by sweep_max. A hand-driven capture runs until the
    // operator stops it, which is right for a person and fatal for a sequence —
    // an unbounded first leg would mean the sweep never reaches its second.
    size_t sweep_at = 0;
    bool sweep_running = false;
    std::string sweep_note;
    long sweep_max = 400;
    // Which SHAPE the running sweep is: latched at inspect_sweep_start from
    // whether a region was named. It cannot be re-derived per frame, because an
    // auto-led sweep WRITES the region its first leg picked — and the leg list
    // would then flip from `auto -> tree -> trace` to `tree -> trace ->
    // dataflow` in the middle of the sequence.
    bool sweep_have_region = false;

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

    // 2026-08-06 plan, Task 4: the same measured perf verdict CapState::perf_ok/
    // perf_reason/perf_probed carry, copied here every frame (shell.cpp, beside
    // where it recomputes CapState::perf_probed) so mode_host_blocked can be
    // driven from InspectState alone — the door's pure functions (inspect_sweep_
    // blocked) take no CapState and must not gain one just to read three fields.
    // perf_probed=false is the safe default: an unprobed host blocks nothing.
    bool perf_ok = false;
    std::string perf_reason;
    bool perf_probed = false;

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

// Per-frame: fire the queued want the MOMENT the jack is free and the host
// still permits it (budget_queue_ready AND mode_host_blocked, re-checked at
// FIRE time rather than trusted from the arm-time snapshot — the jack can
// free itself long after Queue was armed). Returns true if it fired.
// Extracted out of draw_patch_bay (2026-08-06 plan, Task 4 fix, coordinator
// review Finding 3a) so this — like inspect_sweep_poll below — is a named,
// headlessly-testable function instead of inline ImGui-adjacent logic;
// test_shell drives it directly.
bool inspect_queue_poll(InspectState &s);

// The serve `start` params for the current want: a scoped region (func name or
// base+len, parse_region_spec) for trace/dataflow, plus `steps:true` when the
// register ring is armed on a dataflow/auto capture; empty for the whole-process
// modes. Pure over InspectState; test_shell drives it.
nlohmann::json inspect_start_params(const InspectState &s);

// --- substrate sweep (59 T1) ------------------------------------------------
//
// The legs a sweep runs, in order — one per 3D substrate a LIVE capture can
// fill, because each comes from a different engine and the serve host runs one
// engine at a time:
//
//   tree      -> the module excursion ribbon (`call` events)
//   trace     -> the invocation stack (`trace` + a `coverage` block set)
//   dataflow  -> the SIMD lane prism (wide `df_step.ops` register writes)
//
// The address plane rides along: all three arm a `codeimage`.
//
// With NO region named the shape is `auto -> tree -> trace` instead: the auto
// leg samples for its own region (and is itself the dataflow engine), and the
// trace leg behind it single-steps whatever it picked. That is what makes the
// other substrates reachable from the `auto` front door — see sweep_legs in
// live/budget.h for the whole rule.
//
// The DIVERGENCE worldline is deliberately absent and cannot be added here. It
// compares two RECORDINGS, so no sequence of captures inside one session can
// produce it — run a sweep twice and attach the second file as B (`d`).
//
// The shape is LATCHED at inspect_sweep_start: the auto leg writes the picked
// region into `region`, and re-deriving the list from it mid-sweep would swap
// the leg list out from under the running sequence.
const std::vector<LiveMode> &inspect_sweep_legs(const InspectState &s);

// Why a sweep cannot start right now; "" when it can. Each reason names the
// thing to fix, never a bare "unavailable".
std::string inspect_sweep_blocked(const InspectState &s);

// The region an auto-led sweep's SCOPED leg inherits, read off the session's
// `pick` events; "" when no pick carried one (the sampler was refused, or every
// window was idle). The LAST real pick wins: the serve host walks past a ranked
// candidate that never re-enters and re-emits on each, so the last one names the
// region it actually captured. Pure over the notes so test_shell can drive it
// with no live host — this is the hand-off the whole auto-led shape rests on.
std::string inspect_sweep_pick_region(const std::vector<LiveNote> &notes);

// Begin a sweep (no-op when inspect_sweep_blocked is non-empty), and abandon
// one. Cancelling leaves any running leg alone: stopping a capture is the Stop
// button's job, and silently killing one from here would lose its events.
void inspect_sweep_start(InspectState &s);
void inspect_sweep_cancel(InspectState &s);

// Per-frame: arm the next leg once the jack is free. Returns true if it started
// one. Rides the SAME one-ptrace-jack rule the Queue does (budget_queue_ready),
// so a sweep can never bypass the budget.
bool inspect_sweep_poll(InspectState &s);

// The completion note's decode half (2026-08-06 plan, Task 12; M13). The pure
// scorer (live/budget.h's sweep_result_note) takes counts and prose only —
// this is where the counts come from, walking EVERY completed leg in
// s.session.recordings() rather than trusting the leg count. Exposed (not
// static) so test_shell can drive it against a session built purely from
// feed_line, with no ImGui frame and no live host.
std::string inspect_sweep_result_note(const InspectState &s);

// The routine name a missing prism is blamed on (M13: the routine, not the
// capture) — the last real `auto` pick's `func`, or the operator's own typed
// symbol for a scoped sweep that never sampled. "" when neither is known (a
// hand-typed base+len names no routine). Exposed for the same reason as
// inspect_sweep_pick_region: test_shell drives it directly off notes/region.
std::string inspect_sweep_picked_name(const InspectState &s);

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
