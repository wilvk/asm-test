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
};

// Rescan /proc into `s.rows` (also called once on first draw).
void inspect_scan(InspectState &s);
// Start the serve host if it is not up. Records the failure in `host_error`.
void inspect_connect(InspectState &s);
// Ask to start `s.want` on `s.selected_pid`, honouring the budget. Returns
// false and arms `swap_pending` when the jack is occupied.
bool inspect_request_start(InspectState &s);
// Confirm the armed swap: stop the holder, then start what was refused.
void inspect_confirm_swap(InspectState &s);

void draw_inspect_door(InspectState &s);

} // namespace asmdesk
#endif // ASMDESK_UI_DOORS_H
