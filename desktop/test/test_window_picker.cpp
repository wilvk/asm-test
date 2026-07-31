// test_window_picker.cpp — the headless bar for docs/internal/gui/
// 45-launch-and-window-target.md T6: window_picker.cpp talks to a LIVE X11
// display, which this null harness deliberately does not have (no display,
// no GL, "any host with a C++17 compiler" — the same bar every desktop-test
// binary holds). The real correctness proof (a live/virtual display, a real
// second window, a real resolved pid) is T9's Xvfb-backed integration lane —
// this binary is NOT that test and does not pretend to be.
//
// What IS assertable with no display at all: window_picker_supported()
// terminates and returns a bool (never crashes probing for a display that
// is not there), and resolve_window_at_screen_point's honest-refusal
// invariant holds — !ok implies a non-empty why_not, never a silent
// "nothing happened". This also doubles as the "does this link into a
// binary with ZERO other desktop deps" proof mk/desktop.mk's D4 comment
// makes for window_picker.o generally: this test links NOTHING but it.
#include <cstdio>
#include <cstdlib>

#include "platform/window_picker.h"

using asmdesk::PickedWindow;
using asmdesk::resolve_window_at_screen_point;
using asmdesk::window_picker_global_pointer;
using asmdesk::window_picker_supported;

static int failures;

static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

int main() {
    // Whatever this host answers (a real X11 session, XWayland, or neither —
    // the sandbox this runs in is unknown to the test), the call must simply
    // terminate and return a plain bool.
    bool supported = window_picker_supported();
    check(
        "supported/terminates", supported == true || supported == false,
        "window_picker_supported() must return a plain bool, unconditionally");

    // A point far off any plausible screen (deliberately implausible: no
    // display has a window at (-999999,-999999)) still must not crash, and
    // when it refuses, it must SAY why — the honest-refusal invariant this
    // whole module exists to hold, checked here on WHICHEVER branch this
    // host's build/runtime actually takes (X11-supported-but-no-window,
    // vs. the compiled-out stub — see window_picker.cpp's ASMDESK_X11_PICKER
    // gate), rather than assuming one.
    PickedWindow r = resolve_window_at_screen_point(-999999, -999999);
    check("resolve/terminates", true,
          "resolve_window_at_screen_point must return");
    if (!r.ok) {
        check("resolve/refusal-explained", !r.why_not.empty(),
              "!ok must always carry a non-empty why_not — never a silent "
              "refusal");
        check("resolve/refusal-no-pid", r.pid == 0,
              "a refused pick must not carry a stray pid");
    } else {
        // Only reachable if this host's X11 session genuinely has a mapped
        // window covering that point (implausible, but not impossible on an
        // exotic multi-monitor span) — then ok must carry a real pid.
        check("resolve/ok-has-pid", r.pid > 0,
              "an ok pick must carry a positive pid");
    }

    // T7's global-pointer read (the crosshair drag needs it once the pointer
    // leaves the app's own window): must terminate and, on success, hand
    // back SOME coordinate pair — never crash for lack of a display.
    int gx = -1, gy = -1;
    bool have_ptr = window_picker_global_pointer(&gx, &gy);
    check("global-pointer/terminates", have_ptr == true || have_ptr == false,
          "window_picker_global_pointer() must return a plain bool");
    if (have_ptr)
        check("global-pointer/wrote-coords", gx != -1 && gy != -1,
              "a true return must have written *x/*y");

    if (failures) {
        std::fprintf(stderr, "%d window_picker check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_window_picker: all checks passed\n");
    return 0;
}
