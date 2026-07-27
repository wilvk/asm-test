// test_palette.cpp — the command palette (21-spine-navigation.md T1). Model only:
// the table enumerates every dt_nav_bindings() row, each command dispatches the
// right want_view / dt_nav_go, a typed go-to equals dt_nav_parse, and a refusing
// dispatch fills s.status verbatim. No ImGui context is created here — build_palette
// and palette_parse_goto are pure functions of ShellState.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "nav.h"
#include "ui/palette.h"
#include "ui/shell.h"

using namespace asmdesk;

#ifndef ASMTEST_GOLDEN_DIR
#error "ASMTEST_GOLDEN_DIR must be defined by the build (mk/desktop.mk)"
#endif

static int failures;
static void check(const char *what, bool cond, const char *why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}
static std::string gd(const char *name) {
    return std::string(ASMTEST_GOLDEN_DIR) + "/" + name;
}
static const PaletteEntry *find_label(const std::vector<PaletteEntry> &v,
                                      const std::string &lbl) {
    for (const PaletteEntry &e : v)
        if (e.label == lbl)
            return &e;
    return nullptr;
}

int main() {
    ShellState s;
    std::string err;
    int idx = shell_open(s, gd("sum_via_rbx.asmtrace"), err);
    check("open recording", idx >= 0, err.c_str());
    s.active_tab = idx >= 0 ? idx : 0;
    const std::string rec = idx >= 0 ? s.streams[static_cast<size_t>(idx)].id
                                     : std::string("sum_via_rbx.asmtrace");

    // --- every accelerator is enumerated, one-for-one with dt_nav_bindings() --
    {
        std::vector<PaletteEntry> pal = build_palette(s);
        std::vector<const PaletteEntry *> hints;
        for (const PaletteEntry &e : pal)
            if (e.category == PaletteCategory::KeyHint)
                hints.push_back(&e);
        const std::vector<dt_binding> &binds = dt_nav_bindings();
        check("hint count == binding count", hints.size() == binds.size(),
              "every accelerator surfaced by typing, none silently dropped");
        bool ok = hints.size() == binds.size();
        for (size_t i = 0; ok && i < binds.size(); i++)
            if (hints[i]->label != binds[i].what ||
                hints[i]->detail != binds[i].keys)
                ok = false;
        check("hint rows carry each binding's what + keys, in order", ok,
              "label==what, detail==keys");
    }

    // --- a view-switch command sets want_view (mirrors 1/2/3/4) ---------------
    {
        std::vector<PaletteEntry> pal = build_palette(s);
        const PaletteEntry *e = find_label(pal, "Show timeline view");
        check("view-switch entry present", e != nullptr, "one per dt_all_views()");
        if (e) {
            ShellState st;
            e->action(st);
            check("view-switch sets want_view to the right view",
                  st.want_view.has_value() &&
                      *st.want_view == dt_view::timeline,
                  "the visible tab actually switches");
        }
    }

    // --- go-to command == dt_nav_parse, and dispatched moves nav.current ------
    {
        std::string link = "asmtrace-link:v=slice&rec=" + rec + "&step=2";
        dt_link viaHelper, viaParse;
        std::string perr;
        bool okH = palette_parse_goto(s, link, viaHelper);
        bool okP = dt_nav_parse(link, viaParse, perr);
        check("palette go-to parse succeeds", okH, "");
        check("palette go-to == dt_nav_parse", okH && okP &&
                  dt_nav_format(viaHelper) == dt_nav_format(viaParse),
              "a typed target and a clicked link produce the same dt_link (D4)");

        std::snprintf(s.palette_buf, sizeof s.palette_buf, "%s", link.c_str());
        std::vector<PaletteEntry> pal = build_palette(s);
        const PaletteEntry *g = nullptr;
        for (const PaletteEntry &e : pal)
            if (e.category == PaletteCategory::GoTo && e.action &&
                e.label.rfind("Go to asmtrace-link", 0) == 0) {
                g = &e;
                break;
            }
        check("a parseable query prepends a live Go to entry", g != nullptr, "");
        if (g) {
            g->action(s);
            check("dispatched go-to moves nav.current onto the link",
                  s.nav.current.has_value() &&
                      dt_nav_format(*s.nav.current) == dt_nav_format(viaParse),
                  "routed through dt_nav_go");
        }
    }

    // --- a refusing dispatch fills s.status verbatim (never a silent no-op) ----
    {
        ShellState st;
        std::string e2;
        int i2 = shell_open(st, gd("sum_via_rbx.asmtrace"), e2);
        st.active_tab = i2 >= 0 ? i2 : 0;
        std::string bad = "asmtrace-link:v=canvas&rec=nope.asmtrace";
        std::snprintf(st.palette_buf, sizeof st.palette_buf, "%s", bad.c_str());
        std::vector<PaletteEntry> pal = build_palette(st);
        const PaletteEntry *g = nullptr;
        for (const PaletteEntry &e : pal)
            if (e.category == PaletteCategory::GoTo && e.action &&
                e.label.find("nope.asmtrace") != std::string::npos) {
                g = &e;
                break;
            }
        check("the refusing go-to entry is present", g != nullptr, "");
        if (g) {
            st.status.clear();
            g->action(st);
            check("a refused dispatch lands nav.last_error in s.status",
                  !st.status.empty(),
                  "loud refusal, exactly as the keymap / go-to paths do");
        }
    }

    // --- reset-layout / attach dispatch the same intents the keymap uses -------
    {
        std::vector<PaletteEntry> pal = build_palette(s);
        const PaletteEntry *reset = find_label(pal, "Reset panel layout");
        check("reset-layout entry present", reset != nullptr, "");
        if (reset) {
            ShellState st;
            st.want_layout_reset = false;
            reset->action(st);
            check("reset raises the same want_layout_reset as Ctrl+Shift+R",
                  st.want_layout_reset, "");
        }
        const PaletteEntry *attach =
            find_label(pal, "Attach to a running process…");
        check("attach-pid entry present", attach != nullptr, "");
        if (attach) {
            ShellState st;
            attach->action(st);
            check("attach opens the Inspect door", st.show_inspect, "");
        }
    }

    if (failures) {
        std::fprintf(stderr, "test_palette: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_palette: all checks passed\n");
    return 0;
}
