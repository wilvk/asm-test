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
#include "live/ptslice.h"
#include "live/session.h"
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
    bool dirty = false;
    char save_path[1024] = "authored.asmtrace";
    std::string save_status;
    bool saved_ok = false;

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
// a hot function via `--auto` with honest evidence labels.
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

    std::vector<ProcRow> rows;
    bool scanned = false;
    long selected_pid = 0;

    // What the client believes is live on this target, for the patch bay. The
    // serve loop refuses a second concurrent start too, but the budget is
    // decided HERE so the UI can render an occupied jack and offer a swap
    // rather than firing a command that comes back as an error.
    std::vector<LiveMode> active;
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
// Confirm the armed perturbation: re-run the start with the consequence
// accepted (T5). The budget/swap gate still applies afterwards.
void inspect_confirm_perturb(InspectState &s);
// Confirm the armed swap: stop the holder, then start what was refused.
void inspect_confirm_swap(InspectState &s);

void draw_inspect_door(InspectState &s);

} // namespace asmdesk
#endif // ASMDESK_UI_DOORS_H
