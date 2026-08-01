// mode.h — the workspace's task-language modes (20-workspace-and-settings.md T2,
// F13). A trivial shared header so both the entry rail (ui/shell.cpp) and the
// data-driven view-presence predicate (ui/view_presence.*) can name a Mode
// without a circular include. The four *task* modes supersede the old "choose a
// door" metaphor: they are named as tasks, not nouns, and each drives a dock
// perspective (T2 step 5) so the label and the layout can never disagree.
#ifndef ASMDESK_UI_MODE_H
#define ASMDESK_UI_MODE_H

#include "ui/layout.h" // LayoutPreset (mode -> perspective)

namespace asmdesk {

// The task the analyst is doing right now. `Learn` is the dependency-free
// landing (no engine, no root), so an empty workspace auto-lands there (T2 step
// 4). `Open` replays a file they have; `Capture` attaches to a live process;
// `Author` writes a routine. `Inspect` is the live-capture posture `Capture`
// lands in once a session is up — kept distinct so a view can scope itself to
// "a live session is running" vs. "the user is about to start one". `Launch`
// (docs/internal/archive/gui/45-launch-and-window-target.md T4) is a second way INTO
// that same live-capture posture — start a new process and trace it from its
// first instruction, rather than pick one already running.
enum class Mode { Learn, Open, Capture, Author, Inspect, Launch };

// The dock perspective a mode leads with (T2 step 5). Learn/Open read a
// recording (ReplayInspect), Capture/Inspect/Launch watch a live process
// (LiveObserver — Launch is the SAME live workflow, not a new perspective),
// Author writes one (Author). Pure — the seam the rail sets and the frame
// applies, and the field test_shell asserts rather than the DockBuilder tree
// (test_layout already covers that).
inline LayoutPreset mode_preset(Mode m) {
    switch (m) {
    case Mode::Learn:
    case Mode::Open:
        return LayoutPreset::ReplayInspect;
    case Mode::Capture:
    case Mode::Inspect:
    case Mode::Launch:
        return LayoutPreset::LiveObserver;
    case Mode::Author:
        return LayoutPreset::Author;
    }
    return LayoutPreset::ReplayInspect;
}

// The task-sentence CTA for a mode (the rail's primary label). A sentence, not
// a noun — "Learn how assembly runs", never "Learn".
inline const char *mode_cta(Mode m) {
    switch (m) {
    case Mode::Learn:
        return "Learn how assembly runs";
    case Mode::Open:
        return "Open a trace I have";
    case Mode::Capture:
        return "Capture a live process";
    case Mode::Author:
        return "Author a routine";
    case Mode::Inspect:
        return "Inspect a live session";
    case Mode::Launch:
        return "Launch a new process";
    }
    return "?";
}

} // namespace asmdesk
#endif // ASMDESK_UI_MODE_H
