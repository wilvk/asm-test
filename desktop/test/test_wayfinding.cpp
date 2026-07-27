// test_wayfinding.cpp — the persistent wayfinding chrome (21-spine-navigation.md
// T2, F9). Model only (never pixels): disambiguated_label is pure over paths;
// breadcrumb_model reflects nav.current after a real dt_nav_go jump; the empty
// state prompts rather than blanks.
#include <cstdio>
#include <string>

#include "doc/recording.h"
#include "doc/workspace.h"
#include "nav.h"
#include "ui/shell.h"
#include "ui/wayfinding.h"

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

int main() {
    // --- disambiguated_label: pure over the open set's paths ------------------
    {
        Workspace ws;
        Recording a;
        a.path = "/home/alice/add.asmtrace";
        ws.recordings.push_back(a);
        Recording b;
        b.path = "/home/bob/add.asmtrace";
        ws.recordings.push_back(b);
        Recording c;
        c.path = "/tmp/unique.asmtrace";
        ws.recordings.push_back(c);

        std::string l0 = disambiguated_label(ws, 0);
        std::string l1 = disambiguated_label(ws, 1);
        std::string l2 = disambiguated_label(ws, 2);
        check("two same-basename are distinguishable", l0 != l1,
              "the whole point of F9's disambiguation");
        check("label keeps the basename",
              l0.find("add.asmtrace") != std::string::npos, "");
        check("label adds the distinguishing parent segment",
              l0.find("alice") != std::string::npos &&
                  l1.find("bob") != std::string::npos,
              "shortest parent-dir segment");
        check("a unique basename stays plain (no suffix)",
              l2 == "unique.asmtrace",
              "stable label when it already differs");
    }

    // --- identical paths -> the short-hash fallback, still distinct -----------
    {
        Workspace ws;
        Recording a;
        a.path = "add.asmtrace"; // no parent dir at all
        ws.recordings.push_back(a);
        Recording b;
        b.path = "add.asmtrace";
        ws.recordings.push_back(b);
        check("identical paths still resolve to distinct labels",
              disambiguated_label(ws, 0) != disambiguated_label(ws, 1),
              "the hash fallback keeps them apart");
    }

    // --- empty nav.current -> a prompt, never a blank bar ---------------------
    {
        ShellState s;
        BreadcrumbModel m = breadcrumb_model(s);
        check("empty: has_position is false", !m.has_position, "nothing navigated");
        check("empty: prompts rather than blanks", !m.prompt.empty(),
              "the 'no position' nudge");
    }

    // --- after a real deep-link jump, the band reflects nav.current -----------
    {
        ShellState s;
        std::string err;
        int idx = shell_open(s, gd("sum_via_rbx.asmtrace"), err);
        check("open recording", idx >= 0, err.c_str());
        if (idx >= 0) {
            s.active_tab = idx;
            dt_link l;
            l.rec = s.streams[static_cast<size_t>(idx)].id;
            l.view = dt_view::timeline;
            l.step = 3;
            bool ok = dt_nav_go(s.nav, l);
            check("jump lands", ok, s.nav.last_error.c_str());
            BreadcrumbModel m = breadcrumb_model(s);
            check("has_position after jump", m.has_position, "current populated");
            check("recording segment reflects nav.current.rec",
                  m.recording.find("sum_via_rbx.asmtrace") != std::string::npos,
                  "");
            check("view segment == dt_view_name", m.view == "timeline", "");
            check("selection segment reflects the step",
                  m.selection.find("step 3") != std::string::npos, "");
        }
    }

    if (failures) {
        std::fprintf(stderr, "test_wayfinding: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_wayfinding: all checks passed\n");
    return 0;
}
