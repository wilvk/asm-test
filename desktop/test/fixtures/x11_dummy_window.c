/* x11_dummy_window.c — the second real window docs/internal/gui/
 * 45-launch-and-window-target.md T9's Xvfb integration lane hit-tests
 * against. A bare XCreateSimpleWindow + XMapWindow client, no toolkit, no
 * window manager required (the resolver's XQueryTree fallback,
 * window_picker.cpp, is exactly what makes that unnecessary) — cheaper than
 * an xterm and needs no extra apt package (CLAUDE.md: Xvfb + a dummy X11
 * client are both installable software, not a hardware/credential gate, so
 * this must not be a reason to self-skip).
 *
 * Prints ONE line to stdout once mapped: "READY <pid> <x> <y> <w> <h>" (the
 * fixed, known geometry a test can hit-test against without querying it
 * back over X itself), then blocks handling X events until killed (SIGTERM
 * or the connection drops) — nothing else needs to happen for a window to
 * stay resolvable.
 *
 * Sets _NET_WM_PID itself (CARDINAL, format 32) — a bare Xlib client gets no
 * help from a toolkit or a window manager to publish that property, and
 * without it the resolver's own honest-refusal path (T6: "no readable
 * _NET_WM_PID") is all there would ever be to test, not a positive
 * resolution.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#define DUMMY_X 50
#define DUMMY_Y 50
#define DUMMY_W 200
#define DUMMY_H 200

int main(void) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "x11_dummy_window: XOpenDisplay failed (no DISPLAY? "
                        "run under xvfb-run)\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win =
        XCreateSimpleWindow(dpy, root, DUMMY_X, DUMMY_Y, DUMMY_W, DUMMY_H, 1,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));

    /* _NET_WM_PID: CARDINAL, format 32, one value — our own pid. */
    Atom net_wm_pid = XInternAtom(dpy, "_NET_WM_PID", False);
    unsigned long pid = (unsigned long)getpid();
    XChangeProperty(dpy, win, net_wm_pid, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&pid, 1);

    /* A window name too — best-effort, mirrors what a real client would set
     * (window_picker.cpp's read_title reads WM_NAME/XGetWMName). */
    XStoreName(dpy, win, "asm-test window-pick fixture");

    XSelectInput(dpy, win, StructureNotifyMask);
    XMapWindow(dpy, win);

    /* Block until the map actually lands (a MapNotify), so the test never
     * races "window created" against "window viewable" — window_contains_
     * point (window_picker.cpp) skips anything not IsViewable. */
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == MapNotify)
            break;
    }
    XFlush(dpy);

    printf("READY %lu %d %d %d %d\n", pid, DUMMY_X, DUMMY_Y, DUMMY_W, DUMMY_H);
    fflush(stdout);

    /* Now just hold the window open — handling events keeps the X
     * connection alive and responsive to a server-side close if the test
     * (or Xvfb) goes away first. Returns (and exits) on any connection
     * error, which SIGTERM from the test harness — or Xvfb shutting down —
     * both produce via a broken socket. */
    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        (void)ev;
    }
}
