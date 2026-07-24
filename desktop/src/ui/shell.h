// shell.h — the visible skeleton (03-desktop-shell.md T6): a home screen with
// the three doors, a recording-open dialog, and a tab strip over the Workspace.
// draw_shell is backend-free (pure ImGui immediate calls over ShellState), so
// the null backend drives it headlessly in tests. Real views land in docs 04-09.
#ifndef ASMDESK_UI_SHELL_H
#define ASMDESK_UI_SHELL_H

#include <string>
#include <vector>

#include "doc/workspace.h"

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
};

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
