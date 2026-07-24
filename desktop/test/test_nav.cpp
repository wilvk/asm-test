// test_nav.cpp — the deep-link spine (04-replay-views.md T2).
//
// A link is something people paste into a bug report, so parse and format have
// to be total and byte-stable, and a link that cannot be honoured has to say
// why. All three are asserted here rather than assumed.
#include <cstdio>
#include <string>
#include <vector>

#include "nav.h"

using namespace asmdesk;

static int failures;

static void fail(const std::string &what, const std::string &why) {
    std::fprintf(stderr, "FAIL %s: %s\n", what.c_str(), why.c_str());
    failures++;
}
static void check(const std::string &what, bool cond, const std::string &why) {
    if (!cond)
        fail(what, why);
}
static void eq(const std::string &what, const std::string &got,
               const std::string &want) {
    if (got != want)
        fail(what, "got \"" + got + "\", want \"" + want + "\"");
}

int main() {
    // --- round trip, over every optional-field combination ------------------
    {
        std::vector<dt_link> table;
        dt_link l;
        l.view = dt_view::canvas;
        l.rec = "add_signed.asmtrace";
        table.push_back(l);
        l.view = dt_view::timeline;
        l.step = 4;
        table.push_back(l);
        l.view = dt_view::slice;
        l.off = 0x12;
        table.push_back(l);
        l.view = dt_view::diff;
        l.rec_b = "sum3.asmtrace";
        table.push_back(l);
        // A recording id with characters that are structural in a link: if the
        // encoder did not escape them, this would parse back as a DIFFERENT
        // link, which is the one failure a round-trip contract cannot survive.
        dt_link odd;
        odd.view = dt_view::canvas;
        odd.rec = "a&b=c d.asmtrace";
        table.push_back(odd);

        for (const dt_link &want : table) {
            std::string text = dt_nav_format(want);
            dt_link got;
            std::string err;
            if (!dt_nav_parse(text, got, err)) {
                fail("round trip of " + text, err);
                continue;
            }
            eq("rec of " + text, got.rec, want.rec);
            eq("rec_b of " + text, got.rec_b, want.rec_b);
            check("view of " + text, got.view == want.view, "view differs");
            check("step of " + text, got.step == want.step, "step differs");
            check("off of " + text, got.off == want.off, "off differs");
            eq("format is byte-stable for " + text, dt_nav_format(got), text);
        }
    }

    // --- an unknown key is IGNORED (forward compat), not an error -----------
    {
        dt_link l;
        std::string err;
        check("an unknown key does not reject the link",
              dt_nav_parse("asmtrace-link:v=canvas&rec=x.asmtrace&future=42&"
                           "step=7",
                           l, err),
              err);
        check("the known fields still arrive", l.step.value_or(0) == 7,
              "step was lost");
        eq("re-formatting drops what this build cannot represent",
           dt_nav_format(l), "asmtrace-link:v=canvas&rec=x.asmtrace&step=7");
    }

    // --- rejections, each with a reason a user can act on -------------------
    {
        struct Case {
            const char *text;
            const char *needle;
        };
        const Case cases[] = {
            {"not-a-link", "asmtrace-link:"},
            {"asmtrace-link:rec=x.asmtrace", "no view"},
            {"asmtrace-link:v=canvas", "no recording"},
            {"asmtrace-link:v=canvas&rec=", "no recording"},
            {"asmtrace-link:v=nope&rec=x", "unknown view"},
            {"asmtrace-link:v=canvas&rec=x&step=abc", "not a 32-bit step"},
            {"asmtrace-link:v=canvas&rec=x&step=4294967296",
             "not a 32-bit step"},
            {"asmtrace-link:v=canvas&rec=x&off=zz", "not an offset"},
            {"asmtrace-link:v=canvas&rec=x%zz", "malformed % escape"},
            {"asmtrace-link:v=canvas&rec", "has no value"},
        };
        for (const Case &c : cases) {
            dt_link l;
            std::string err;
            if (dt_nav_parse(c.text, l, err)) {
                fail(std::string("reject ") + c.text, "it was accepted");
                continue;
            }
            check(std::string("reason for ") + c.text,
                  err.find(c.needle) != std::string::npos,
                  "reason must name \"" + std::string(c.needle) +
                      "\", got: " + err);
        }
    }

    // --- hex and decimal both parse; format is canonical --------------------
    {
        dt_link a, b;
        std::string err;
        check("decimal off parses",
              dt_nav_parse("asmtrace-link:v=canvas&rec=x&off=18", a, err), err);
        check("hex off parses",
              dt_nav_parse("asmtrace-link:v=canvas&rec=x&off=0x12", b, err),
              err);
        check("they are the same offset", a.off == b.off,
              "18 == 0x12; the two spellings must agree");
        eq("format is canonical (hex)", dt_nav_format(a),
           "asmtrace-link:v=canvas&rec=x&off=0x12");
    }

    // --- the router: dispatch, and refuse LOUDLY when it cannot ------------
    {
        dt_nav_table t;
        t.have_recording = [](const std::string &id) {
            return id == "a.asmtrace" || id == "b.asmtrace";
        };
        int canvas_hits = 0;
        uint32_t last_step = 0;
        dt_nav_register(t, dt_view::canvas, [&](const dt_link &l) {
            canvas_hits++;
            last_step = l.step.value_or(0);
        });

        dt_link l;
        l.view = dt_view::canvas;
        l.rec = "a.asmtrace";
        l.step = 9;
        check("a valid link navigates", dt_nav_go(t, l), t.last_error);
        check("the handler ran", canvas_hits == 1, "it did not");
        check("with the link's step", last_step == 9, "step was not passed");
        check("the current position is remembered for the copy action",
              t.current.has_value(), "not recorded");

        l.rec = "missing.asmtrace";
        check("a link to an unopened recording is refused", !dt_nav_go(t, l),
              "it must not silently no-op");
        check("and says which recording",
              t.last_error.find("missing.asmtrace") != std::string::npos,
              t.last_error);
        check("the handler did NOT run again", canvas_hits == 1,
              "a refused navigation must not move the view");

        l.rec = "a.asmtrace";
        l.rec_b = "gone.asmtrace";
        check("a diff link with a missing B side is refused", !dt_nav_go(t, l),
              "B must exist too");
        check("and says so",
              t.last_error.find("gone.asmtrace") != std::string::npos,
              t.last_error);

        l.rec_b.clear();
        l.view = dt_view::slice;
        check("a view with no handler is refused", !dt_nav_go(t, l),
              "an unregistered view cannot be navigated to");
        check("and names the view",
              t.last_error.find("slice") != std::string::npos, t.last_error);

        // Re-registering replaces rather than shadowing, so a rebuilt view does
        // not leave a stale handler behind holding a dangling reference.
        int second = 0;
        dt_nav_register(t, dt_view::canvas, [&](const dt_link &) { second++; });
        l.view = dt_view::canvas;
        check("re-registered handler navigates", dt_nav_go(t, l), t.last_error);
        check("the NEW handler ran", second == 1, "the old one is still bound");
        check("the old handler did not", canvas_hits == 1, "both ran");
    }

    // --- the bindings table is data, so help and behaviour cannot drift -----
    {
        check("there are keyboard bindings", !dt_nav_bindings().empty(),
              "the help overlay would be empty");
        for (const dt_binding &b : dt_nav_bindings()) {
            check("every binding names its keys", b.keys && *b.keys, "empty");
            check("every binding says what it does", b.what && *b.what,
                  "empty");
        }
    }

    if (failures) {
        std::fprintf(stderr, "%d nav check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_nav: all checks passed\n");
    return 0;
}
