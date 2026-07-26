// shell.h — the visible skeleton (03-desktop-shell.md T6): a home screen with
// the three doors, a recording-open dialog, and a tab strip over the Workspace.
// draw_shell is backend-free (pure ImGui immediate calls over ShellState), so
// the null backend drives it headlessly in tests. Real views land in docs 04-09.
#ifndef ASMDESK_UI_SHELL_H
#define ASMDESK_UI_SHELL_H

#include <optional>
#include <string>
#include <vector>

#include "analysis/stepindex.h"
#include "doc/streams.h"
#include "doc/workspace.h"
#include "loom/loom_draw.h"
#include "nav.h"
#include "ui/doors.h"
#include "views/completeness.h"
#include "views/observer_draw.h"
#include "walkthrough.h"

namespace asmdesk {

struct ShellState {
    Workspace ws;
    int active_tab = -1;        // -1 = home (the three doors)
    bool open_dialog = false;   // the recording-open dialog is showing
    char open_path[1024] = {0}; // its InputText buffer
    std::string open_error;     // last open failure, rendered verbatim
    // Author/Inspect open an empty placeholder tab in the full app (behaviour
    // lands in docs 06/08); disabled with a reason in the render-only viewer.
    std::vector<std::string> door_tabs;

    // --- the replay views (04-replay-views.md) ---------------------------
    // Decoded once per open recording, parallel to ws.recordings: the builders
    // are pure functions of these, so nothing below re-parses JSON per frame.
    std::vector<Streams> streams;
    // Plan D3: every view takes one OR two recordings. `b_index` is the
    // attached B side (the `d` binding), -1 for none.
    int b_index = -1;
    dt_view view = dt_view::canvas;
    std::optional<uint32_t> selected_step;
    std::optional<uint64_t> selected_off;
    // The lit cones, when a slice is active; cleared by `c`.
    bool cone_active = false;
    dt_nav_table nav;
    bool show_help = false;
    bool show_learn = false;
    bool show_author = false;
    bool show_inspect = false;
    std::string status; // the status bar: nav refusals land here verbatim
    CompletenessState completeness;
    // The Loom's per-tab state (05-loom-day-one.md). Woven once per recording,
    // not per frame.
    LoomState loom;
    // The Learn door's card list + player (06-doors-and-learning.md T4).
    LearnState learn;
    // The capability panel (06-doors-and-learning.md T6).
    CapState caps;
    // The Author door (06-doors-and-learning.md T5); full build only.
    AuthorState author;
    // The Inspect door (07-serve-live-host.md T4/T5). In BOTH binaries: it
    // links no engine and captures through the `asmspy --serve` subprocess.
    InspectState inspect;
    // The live Observer views (08-observer-views.md), one deck per open
    // recording — parallel to ws.recordings, exactly like `streams`. A live
    // session's recording and a replayed file feed the SAME deck, which is the
    // property the whole doc is built on.
    std::vector<ObserverState> observers;
    std::string repo_root = ".";

    // The register time-travel scrubber (09-teaching-producers.md T3), surfaced
    // as a per-recording tab. `stepidx` is the O(1) regstate seek index, built
    // once at open and parallel to ws.recordings exactly like `streams` /
    // `observers`; a recording with no ring yields an absent index and the tab
    // shows the producer placard. `scrubber_playhead` is that tab's cursor, one
    // per recording so switching tabs keeps each recording's place.
    std::vector<StepIndex> stepidx;
    std::vector<uint64_t> scrubber_playhead;

    // The ABI x-ray (09-teaching-producers.md T4), surfaced as a tab that locks
    // the active recording (the SysV leg) against the attached B (the Win64 leg,
    // the `d` binding) — the same A/B mechanism the Diff tab uses. The rail
    // MUTATES `abixray_walk` (stop navigation), so it must persist across frames;
    // `abixray_key` is the pair it was built for (`A_id\x1fB_id`), rebuilt only
    // when the pair changes. `abixray_playhead` is the single locked playhead.
    std::string abixray_key;
    wt_model abixray_walk;
    uint64_t abixray_playhead = 0;

    // A pending cross-door jump: a capture the Inspect door just saved and asked
    // to open in the Loom (07-serve-live-host.md). `want_open_tab` is the
    // recording index whose outer tab to select; `want_loom` forces its Loom
    // inner tab. Both are set when draw_shell consumes InspectState::open_request
    // and cleared the same frame after the tab strip applies them.
    int want_open_tab = -1;
    bool want_loom = false;
};

// Open a recording AND decode its streams, keeping ShellState::streams parallel
// to Workspace::recordings. Returns the new index, or -1 with `err` set.
int shell_open(ShellState &s, const std::string &path, std::string &err);
void shell_close(ShellState &s, size_t idx);

// Register every view with the router and point it at the open set. Idempotent:
// safe to call again after the workspace changes.
void shell_wire_nav(ShellState &s);

// The A / B streams for the active tab; B is null when nothing is attached.
const Streams *shell_a(const ShellState &s);
const Streams *shell_b(const ShellState &s);

// Draw one frame of the shell. Backend-free: only ImGui immediate-mode calls, so
// a null ImGui context (no GLFW/GL) drives it in tests.
void draw_shell(ShellState &s);

// The truncation/drops/torn banner for a recording — PURE: nullptr when the
// recording is clean, else a human-readable line (e.g.
// "TRUNCATED recording — buffers filled"). The returned pointer is valid until
// the next call on the same thread. This is D7 as behaviour, asserted by tests.
const char *shell_banner(const Recording &r);

} // namespace asmdesk
#endif // ASMDESK_UI_SHELL_H
