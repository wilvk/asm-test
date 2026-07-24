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

} // namespace asmdesk
#endif // ASMDESK_UI_DOORS_H
