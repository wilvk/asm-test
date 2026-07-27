// palette.h — the command palette over the deep-link spine (21-spine-navigation.md
// T1, F8). A pure, testable command table + its modal draw.
//
// Ctrl+Shift+P / Ctrl+P opens a fuzzy finder that makes the WHOLE spine reachable
// by typing. Every entry dispatches through `dt_nav_go` (or the exact
// `want_view`/`show_*` intent handle_keymap uses) — no view reaches around the
// router — and the accelerator hints are enumerated from `dt_nav_bindings()`, so
// the palette can never advertise a key the app does not honour and a future
// binding cannot silently drop from it. The command table (`build_palette`) is a
// pure function of ShellState with no ImGui context, so test_palette drives it
// headlessly; the draw half degrades to an unranked list under the null backend
// (ImSearch is app-only, exactly like the Learn-door catalog filter, doc 16 T2).
#ifndef ASMDESK_UI_PALETTE_H
#define ASMDESK_UI_PALETTE_H

#include <functional>
#include <string>
#include <vector>

#include "nav.h"

namespace asmdesk {

struct ShellState; // ui/shell.h; the actions mutate one by reference

// The command categories, so a test (and the draw) can tell an accelerator HINT
// row (non-dispatching) from a real command.
enum class PaletteCategory {
    GoTo,       // go to a step/offset/link — dispatches dt_nav_go
    View,       // switch the active recording's view — sets want_view
    Recent,     // select an open recording — sets want_open_tab
    Attach,     // open the Inspect capture door
    Walkthrough,// open a Learn-door card
    Layout,     // reset the panel layout
    Routine,    // jump to a recorded code offset (v1 has no symbol table)
    KeyHint,    // a discoverable accelerator, from dt_nav_bindings() — no action
};

// One command. `action` is null ONLY for a KeyHint row (it exists to make a key
// discoverable by typing; running it is a no-op). `detail` is the dim right-hand
// tag: a category word, or the accelerator keys for a hint / bound command.
struct PaletteEntry {
    std::string label;
    std::string detail;
    PaletteCategory category = PaletteCategory::View;
    std::function<void(ShellState &)> action;
};

// Build the command table for the current shell. Pure (no ImGui context): it
// enumerates dt_all_views(), the open workspace, the Learn cards, the recorded
// code offsets, and — always — every dt_nav_bindings() row as a hint. When
// `s.palette_buf` parses as a step/offset/link it also prepends a live "Go to …"
// command. Runs every frame the palette is open; cheap.
std::vector<PaletteEntry> build_palette(const ShellState &s);

// The go-to parse the palette shares with draw_goto_modal (D4): a bare number is
// a step on the active recording in the current view; anything else is a full
// asmtrace-link. False when it is neither (no active recording for a bare number,
// or an unparseable link).
bool palette_parse_goto(const ShellState &s, const std::string &text,
                        dt_link &out);

// Draw the modal (call once per frame, beside draw_goto_modal). Opens on
// `s.show_palette`; Enter runs the top match, Esc/close dismisses. Backend-free
// ImGui.
void draw_palette(ShellState &s);

} // namespace asmdesk
#endif // ASMDESK_UI_PALETTE_H
