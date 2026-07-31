// window_picker.cpp — the X11 implementation (docs/internal/gui/
// 45-launch-and-window-target.md T6), and the honest stub every other
// platform (or a build with no X11 dev headers) degrades to.
//
// ASMTEST_HAVE_X11 is set by mk/desktop.mk exactly the way
// ASMTEST_HAVE_CAPSTONE already is (Makefile:883-885) — a pkg-config probe,
// present when libx11-dev is installed, absent otherwise. Both branches of
// this file compile into asmtest-desktop AND asmtest-viewer identically
// (D4) — there is no ptrace and no engine linkage on either path.
#include "platform/window_picker.h"

#include <cstring>
#include <vector>

// __APPLE__ is excluded even if X11 dev headers happen to be findable there
// (a XQuartz install) — T6's own "Done when" states this stays X11-only on
// Linux; a macOS window-picker would need CGWindowListCopyWindowInfo and is
// explicitly out of scope (Non-goals), and would be moot anyway since the
// ptrace engines this app attaches through are Linux-only.
#if defined(ASMTEST_HAVE_X11) && !defined(__APPLE__)
#define ASMDESK_X11_PICKER 1
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h> // XTextProperty / XGetWMName — the best-effort window title
#endif

namespace asmdesk {

#ifdef ASMDESK_X11_PICKER

namespace {

// RAII-lite: the display connection lives for exactly one call — this is not
// a hot path (a drag-release, T7), so there is no benefit to holding one
// open, and closing it each time means a display that comes and goes
// (Xephyr/Xvfb starting up mid-session) is reflected honestly on the very
// next call rather than cached stale.
struct Display1 {
    Display *dpy;
    Display1() : dpy(XOpenDisplay(nullptr)) {}
    ~Display1() {
        if (dpy)
            XCloseDisplay(dpy);
    }
    explicit operator bool() const { return dpy != nullptr; }
};

// Read a window-list property (WINDOW or CARDINAL-as-window array, format
// 32) into `out`. Returns false if the atom does not exist or the property
// is absent/wrong-format — both routine (no window manager, or an atom this
// WM does not publish), not an error to log.
bool read_window_list(Display *dpy, Window w, Atom prop,
                      std::vector<Window> &out) {
    out.clear();
    if (prop == None)
        return false;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = nullptr;
    int rc = XGetWindowProperty(dpy, w, prop, 0, 65536, False, AnyPropertyType,
                                &actual_type, &actual_format, &nitems,
                                &bytes_after, &data);
    if (rc != Success || !data)
        return false;
    bool ok = (actual_format == 32) &&
              (actual_type == XA_WINDOW || actual_type == XA_CARDINAL);
    if (ok) {
        const unsigned long *ids =
            reinterpret_cast<const unsigned long *>(data);
        out.reserve(nitems);
        for (unsigned long i = 0; i < nitems; i++)
            out.push_back(static_cast<Window>(ids[i]));
    }
    XFree(data);
    return ok;
}

// _NET_WM_PID (CARDINAL, format 32) directly on `w`, or -1 if absent/unreadable.
long read_net_wm_pid(Display *dpy, Atom net_wm_pid, Window w) {
    if (net_wm_pid == None)
        return -1;
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = nullptr;
    int rc = XGetWindowProperty(dpy, w, net_wm_pid, 0, 1, False, XA_CARDINAL,
                                &actual_type, &actual_format, &nitems,
                                &bytes_after, &data);
    long pid = -1;
    if (rc == Success && data && actual_type == XA_CARDINAL &&
        actual_format == 32 && nitems >= 1)
        pid = static_cast<long>(*reinterpret_cast<unsigned long *>(data));
    if (data)
        XFree(data);
    return pid;
}

// The WM_NAME / _NET_WM_NAME (best-effort, informational only — never gates
// a resolution). Falls back to "" rather than guessing.
std::string read_title(Display *dpy, Window w) {
    XTextProperty tp;
    if (XGetWMName(dpy, w, &tp) && tp.value) {
        std::string s(reinterpret_cast<char *>(tp.value));
        XFree(tp.value);
        return s;
    }
    return std::string();
}

// _NET_WM_PID is common on the client window itself, but the point can also
// land on a WM decoration/frame subwindow that carries no such property (the
// doc's own caveat) — so walk the subtree looking for the first descendant
// that has one. Bounded depth (DEPTH_MAX) so a pathological window tree
// cannot spin this forever; any real client's PID is at most a couple of
// reparenting levels down.
long find_pid_in_subtree(Display *dpy, Atom net_wm_pid, Window w, int depth) {
    long pid = read_net_wm_pid(dpy, net_wm_pid, w);
    if (pid >= 0)
        return pid;
    if (depth <= 0)
        return -1;
    Window root, parent, *children = nullptr;
    unsigned int nchildren = 0;
    if (!XQueryTree(dpy, w, &root, &parent, &children, &nchildren))
        return -1;
    long found = -1;
    for (unsigned int i = 0; i < nchildren && found < 0; i++)
        found = find_pid_in_subtree(dpy, net_wm_pid, children[i], depth - 1);
    if (children)
        XFree(children);
    return found;
}

// Does `w`'s on-screen rectangle contain (x, y) (X11 root/screen
// coordinates)? Unmapped windows are never hit — nothing is drawn there for
// the operator to have pointed at.
bool window_contains_point(Display *dpy, Window root, Window w, int x, int y) {
    XWindowAttributes attrs;
    if (!XGetWindowAttributes(dpy, w, &attrs))
        return false;
    if (attrs.map_state != IsViewable)
        return false;
    int screen_x = 0, screen_y = 0;
    Window child;
    if (!XTranslateCoordinates(dpy, w, root, 0, 0, &screen_x, &screen_y,
                               &child))
        return false;
    return x >= screen_x && x < screen_x + attrs.width && y >= screen_y &&
           y < screen_y + attrs.height;
}

} // namespace

bool window_picker_supported() {
    Display1 d;
    return static_cast<bool>(d);
}

PickedWindow resolve_window_at_screen_point(int x, int y) {
    PickedWindow r;
    Display1 d;
    if (!d) {
        r.why_not = "no X11 display reachable — window-pick needs a live X11 "
                    "session (including XWayland); pure Wayland or a "
                    "headless host has none";
        return r;
    }
    Window root = DefaultRootWindow(d.dpy);
    Atom net_client_list_stacking =
        XInternAtom(d.dpy, "_NET_CLIENT_LIST_STACKING", True);
    Atom net_wm_pid = XInternAtom(d.dpy, "_NET_WM_PID", True);

    std::vector<Window> candidates;
    bool have_ewmh =
        read_window_list(d.dpy, root, net_client_list_stacking, candidates);
    if (!have_ewmh) {
        // No window manager publishing EWMH (T9's bare Xvfb lane — no WM at
        // all) — fall back to the raw window tree. XQueryTree's children come
        // back bottom-to-top, same order EWMH promises, so the walk below
        // (reversed, topmost first) is identical either way.
        Window rroot, rparent, *children = nullptr;
        unsigned int nchildren = 0;
        if (XQueryTree(d.dpy, root, &rroot, &rparent, &children, &nchildren)) {
            candidates.assign(children, children + nchildren);
            if (children)
                XFree(children);
        }
    }

    // Topmost first: both sources are bottom-to-top, so walk in reverse.
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
        if (!window_contains_point(d.dpy, root, *it, x, y))
            continue;
        long pid = find_pid_in_subtree(d.dpy, net_wm_pid, *it, /*depth=*/4);
        if (pid < 0) {
            r.why_not = "the window at that point has no readable "
                        "_NET_WM_PID — it is not an EWMH-aware client, so "
                        "its owning process cannot be named from here";
            return r;
        }
        r.ok = true;
        r.pid = pid;
        r.title = read_title(d.dpy, *it);
        return r;
    }
    r.why_not = "no window at that point";
    return r;
}

bool window_picker_global_pointer(int *x, int *y) {
    Display1 d;
    if (!d)
        return false;
    Window root = DefaultRootWindow(d.dpy);
    Window root_ret, child_ret;
    int root_x, root_y, win_x, win_y;
    unsigned int mask;
    if (!XQueryPointer(d.dpy, root, &root_ret, &child_ret, &root_x, &root_y,
                       &win_x, &win_y, &mask))
        return false; // the pointer is on a different SCREEN (multi-screen X)
    *x = root_x;
    *y = root_y;
    return true;
}

#else // !ASMDESK_X11_PICKER

// The honest stub: non-Linux, or no X11 dev headers at build time (this
// host's mk/desktop.mk probe found no libx11-dev, mirroring
// ASMTEST_HAVE_CAPSTONE's pattern exactly) — never silently "supported" and
// never a link failure.
bool window_picker_supported() { return false; }

bool window_picker_global_pointer(int *, int *) { return false; }

PickedWindow resolve_window_at_screen_point(int, int) {
    PickedWindow r;
    r.why_not = "built without X11 support (no libx11-dev at build time, or "
                "a non-Linux host)";
    return r;
}

#endif // ASMDESK_X11_PICKER

} // namespace asmdesk
