// wayfinding.h — the persistent global-context chrome (21-spine-navigation.md
// T2, F9), built ON doc-18's minimal breadcrumb rather than duplicating it.
//
// Two pure pieces the tests read as model, plus the draw half:
//   - disambiguated_label: two same-basename `.asmtrace` are indistinguishable
//     today (shell.cpp's tab titles); this returns the basename plus the SHORTEST
//     distinguishing parent-dir segment across the open set, or a short path hash
//     when even that collides. Used in the breadcrumb AND the recording tab
//     titles (so both agree) AND the palette's open-recent labels (T1).
//   - breadcrumb_model: the "where am I" band, sourced entirely from
//     `nav.current` (recording -> session -> view + step/selection + filter +
//     process scope). Empty position yields a PROMPT, never a blank bar.
// The draw half (draw_wayfinding_bar) lives in the OUTER shell, outside every
// per-tab body, so it is visible from any pane.
#ifndef ASMDESK_UI_WAYFINDING_H
#define ASMDESK_UI_WAYFINDING_H

#include <cstddef>
#include <string>

#include "doc/workspace.h"

namespace asmdesk {

struct ShellState; // defined in ui/shell.h; the bar/model only take a reference

// The display label for open recording `i`: its basename, disambiguated against
// every OTHER open recording that shares the basename. Pure function of the
// recordings' paths — no file I/O — so a test drives it on a hand-built set.
std::string disambiguated_label(const Workspace &ws, size_t i);

// The persistent-context model, read verbatim by test_wayfinding (never pixels).
// `has_position` is false before anything is navigated; then `prompt` carries the
// "open a recording or press Ctrl+Shift+P" nudge and every other field is empty.
struct BreadcrumbModel {
    bool has_position = false;
    std::string prompt;    // shown only when !has_position
    std::string recording; // disambiguated label of nav.current.rec
    std::string diff_b;    // "vs <B>" when a diff B is attached, else empty
    std::string session;   // "pid N" for a live coordinate, else empty (a file)
    std::string view;      // dt_view_name(nav.current.view)
    std::string selection; // "step N" / "off 0x..", else empty
    std::string filter;    // the active client-side filter (scope, not absence)
    std::string thread;    // the process/thread scope, else empty
};

// Derive the band from `s.nav.current` (+ the active recording's live filter).
BreadcrumbModel breadcrumb_model(const ShellState &s);

// Draw the band inside the CURRENT window (the outer menu bar when docked, the
// top strip of the windowed shell otherwise). Read-only: it surfaces scope and
// provenance, it does not navigate — the back/forward affordance (doc 18) stays
// its own control. Backend-free ImGui, so the null test backend drives it.
void draw_wayfinding_bar(ShellState &s);

} // namespace asmdesk
#endif // ASMDESK_UI_WAYFINDING_H
