// test_window_picker_xvfb.cpp — docs/internal/gui/45-launch-and-window-
// target.md T9: the REAL correctness proof for window_picker.cpp, against a
// LIVE (virtual) X11 display and a real second window — not a mock, and not
// a self-skip. This binary is meant to run under `xvfb-run` (mk/desktop.mk's
// desktop-test-xvfb target / Dockerfile.desktop's CMD), which is what gives
// it a real DISPLAY; run bare (no X server reachable) it FAILS rather than
// skipping, because the whole point of this lane (CLAUDE.md: "a missing
// dependency is not a blocker... Xvfb and a dummy X11 client are both
// installable software, not hardware/credentials") is that nothing here is
// allowed to self-skip for a reason that is just "nobody installed Xvfb".
//
// test_window_picker.cpp (the OTHER, null-backend binary) is the headless
// bar: it proves the API terminates and honors the honest-refusal invariant
// with no display at all, and deliberately does not attempt what only a
// live display can prove.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <signal.h>
#include <sys/wait.h>
#include <time.h> // nanosleep
#include <unistd.h>

#include "platform/window_picker.h"

using asmdesk::PickedWindow;
using asmdesk::resolve_window_at_screen_point;
using asmdesk::window_picker_supported;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

#ifndef ASMTEST_X11_DUMMY_WINDOW
#define ASMTEST_X11_DUMMY_WINDOW "build/x11_dummy_window"
#endif

int main() {
    // The whole lane's precondition: a REAL X11 display must be reachable —
    // asserted, not assumed, so a misconfigured runner (xvfb-run not
    // actually wrapping this binary) fails loudly instead of the rest of
    // the file silently proving nothing.
    check("xvfb/supported", window_picker_supported(),
          "window_picker_supported() must be true under xvfb-run — if this "
          "fails, the lane is not actually running under a live X11 "
          "display (check the xvfb-run wrapper, not this binary)");
    if (!window_picker_supported()) {
        std::fprintf(stderr,
                     "%d window_picker_xvfb check(s) failed (no display — "
                     "nothing else here is meaningful without one)\n",
                     failures);
        return 1;
    }

    // Spawn the dummy window as a genuinely SEPARATE process (not a thread
    // of this one) — the whole point is resolving a DIFFERENT process's
    // pid, which a same-process window could not prove.
    int outpipe[2];
    if (pipe(outpipe) != 0) {
        std::fprintf(stderr, "FAIL xvfb/pipe: %s\n", std::strerror(errno));
        return 1;
    }
    pid_t child = fork();
    if (child < 0) {
        std::fprintf(stderr, "FAIL xvfb/fork: %s\n", std::strerror(errno));
        return 1;
    }
    if (child == 0) {
        close(outpipe[0]);
        dup2(outpipe[1], 1); // the fixture's "READY ..." line
        close(outpipe[1]);
        execl(ASMTEST_X11_DUMMY_WINDOW, ASMTEST_X11_DUMMY_WINDOW,
              (char *)nullptr);
        _exit(127); // execl failed
    }
    close(outpipe[1]);

    // Read the "READY <pid> <x> <y> <w> <h>" line the fixture prints once
    // its window is actually mapped (MapNotify) — never race "created"
    // against "viewable".
    FILE *out = fdopen(outpipe[0], "r");
    long dummy_pid = 0;
    int dx = 0, dy = 0, dw = 0, dh = 0;
    bool got_ready = false;
    if (out) {
        char line[256];
        if (std::fgets(line, sizeof line, out) &&
            std::sscanf(line, "READY %ld %d %d %d %d", &dummy_pid, &dx, &dy,
                        &dw, &dh) == 5)
            got_ready = true;
    }
    check("xvfb/fixture-ready", got_ready,
          "the dummy window fixture did not print its READY line — "
          "did build/x11_dummy_window build? (make desktop-test-xvfb)");

    if (got_ready) {
        int cx = dx + dw / 2, cy = dy + dh / 2;

        // Point INSIDE the dummy window: must resolve to its real pid. A
        // short bounded retry (the map + this test's own fork/read race
        // against the X server's property propagation, not a hard timeout
        // the resolver itself has).
        PickedWindow hit;
        for (int i = 0; i < 50; i++) {
            hit = resolve_window_at_screen_point(cx, cy);
            if (hit.ok)
                break;
            struct timespec ts = {0, 20 * 1000 * 1000}; // 20ms
            nanosleep(&ts, nullptr);
        }
        check("xvfb/hit-ok", hit.ok,
              "no window resolved at the dummy window's own center point (" +
                  std::to_string(cx) + "," + std::to_string(cy) +
                  "): why_not=\"" + hit.why_not + "\"");
        check("xvfb/hit-pid", hit.pid == dummy_pid,
              "resolved pid " + std::to_string(hit.pid) +
                  " != the "
                  "fixture's real pid " +
                  std::to_string(dummy_pid));

        // Point at empty background — well outside the dummy window and (on
        // Xvfb's default 1024x768-or-larger virtual screen) outside
        // anything else that could plausibly be mapped there. Must refuse,
        // with a stated reason.
        PickedWindow miss = resolve_window_at_screen_point(900, 700);
        check("xvfb/miss-refuses", !miss.ok,
              "an empty point resolved to a window — geometry hit-testing "
              "is wrong, or the virtual screen is smaller than assumed");
        check("xvfb/miss-explained", !miss.ok && !miss.why_not.empty(),
              "a miss must carry a stated why_not, never a silent no-op");
        check("xvfb/miss-no-pid", miss.pid == 0,
              "a refused pick must not carry a stray pid");
    }

    kill(child, SIGTERM);
    int status = 0;
    waitpid(child, &status, 0);
    if (out)
        fclose(out);
    else
        close(outpipe[0]);

    if (failures) {
        std::fprintf(stderr, "%d window_picker_xvfb check(s) failed\n",
                     failures);
        return 1;
    }
    std::printf("test_window_picker_xvfb: all checks passed\n");
    return 0;
}
