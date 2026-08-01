// window_picker.h — "which PID owns the window at this screen point?"
// (docs/internal/archive/gui/45-launch-and-window-target.md, Part B, T6).
//
// A small, pure-ish platform module: genuinely new platform surface (this is
// why it lives in its own desktop/src/platform/, not ui/ or live/), X11-only,
// with an honest refusal — never a silent no-op — everywhere the platform
// cannot answer: no live X11 display (pure Wayland, no display at all,
// non-Linux), no window at the point, or a window with no readable
// _NET_WM_PID (a non-EWMH client).
//
// D4 (docs/internal/gui/README.md): this module links X11 only — no
// Unicorn/Keystone/Capstone, no ptrace — and builds into BOTH
// asmtest-desktop and the render-only asmtest-viewer identically, the same
// engine-free posture live/inspect.cpp's /proc reads already keep for
// process enumeration, extended one layer further into windowing.
//
// window_picker_supported() answers "is X11 actually reachable RIGHT NOW"
// (XOpenDisplay succeeds) — not "is $DISPLAY set", which can be stale under
// a pure-Wayland session with no X server at all (main.cpp:137-150's
// Wayland-degrade-honestly comment is the precedent this follows: a stated
// unavailable control, never one that looks live and silently does
// nothing).
#ifndef ASMDESK_PLATFORM_WINDOW_PICKER_H
#define ASMDESK_PLATFORM_WINDOW_PICKER_H

#include <string>

namespace asmdesk {

// True only when a live X11 display is reachable right now (including
// XWayland — it is still a real X server). False under pure Wayland, no
// display at all, macOS, or a build with no X11 dev headers (ASMTEST_HAVE_X11
// unset at compile time, mirroring ASMTEST_HAVE_CAPSTONE's pattern).
bool window_picker_supported();

// The result of resolving a screen point to a window's owning process.
struct PickedWindow {
    bool ok = false;
    long pid = 0;
    std::string title;
    // Populated whenever ok is false — a stated reason, never a bare
    // failure: not supported (no X11 display), no window at that point, or
    // the topmost window there carries no readable _NET_WM_PID (a real,
    // non-EWMH client, not mapped to a fabricated pid 0). A resolved pid
    // that is not on the SAME host the capture would target is a fact
    // about the CALL SITE (only meaningful for local capture), not this
    // resolver — T8 gates that where s.inspect.ssh_host is visible, per
    // docs/internal/gui/45's "Constraints & gates".
    std::string why_not;
};

// Resolve the topmost window at screen point (x, y) — X11 root-window
// (screen) coordinates, the same space glfwGetMonitorPos / a global pointer
// query use — to its owning PID. Walks the window stack top-to-bottom
// (_NET_CLIENT_LIST_STACKING when the window manager publishes one, else a
// plain XQueryTree walk from the root — which is what makes this resolvable
// under a bare Xvfb + a client with NO window manager running at all, T9's
// test lane), hit-tests each candidate's geometry (XGetWindowAttributes +
// XTranslateCoordinates to root-relative coordinates), and reads
// _NET_WM_PID off the first (topmost) match — walking to the nearest
// descendant that carries the property if the hit window itself does not
// (common: the point can land on a WM decoration/frame subwindow rather
// than the client window proper).
PickedWindow resolve_window_at_screen_point(int x, int y);

// The CURRENT global pointer position, in the same X11 root/screen
// coordinates resolve_window_at_screen_point takes — read directly from the
// X server via XQueryPointer, not through the app's own window. T7's drag
// affordance needs this specifically because GLFW's per-window
// glfwGetCursorPos is specified relative to that window, and its behavior
// once the pointer leaves the window's own bounds is not something to rely
// on sight-unseen (docs/internal/gui/45 T7's own citation) — reading the
// display server directly sidesteps the question. Returns false (leaving
// *x/*y untouched) when no X11 display is reachable, same honest-refusal
// posture as the rest of this module.
bool window_picker_global_pointer(int *x, int *y);

} // namespace asmdesk
#endif // ASMDESK_PLATFORM_WINDOW_PICKER_H
