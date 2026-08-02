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

    // --- 54 T6: dt_link.invocation, appended LAST (after pid) ---------------
    {
        // No invocation: byte-identical to a pre-T6 link (nav.h's own bar --
        // "every link written before this change stays byte-identical").
        dt_link l;
        l.view = dt_view::canvas;
        l.rec = "x.asmtrace";
        eq("no invocation: format unchanged", dt_nav_format(l),
           "asmtrace-link:v=canvas&rec=x.asmtrace");

        // With invocation: round-trips.
        dt_link li;
        li.view = dt_view::canvas;
        li.rec = "x.asmtrace";
        li.invocation = 7;
        eq("invocation formats after rec", dt_nav_format(li),
           "asmtrace-link:v=canvas&rec=x.asmtrace&invocation=7");
        dt_link rt;
        std::string err;
        check("invocation round-trips",
              dt_nav_parse(dt_nav_format(li), rt, err), err);
        check("invocation value survives", rt.invocation.value_or(999) == 7,
              "invocation was lost");
        eq("re-format is byte-stable", dt_nav_format(rt), dt_nav_format(li));

        // Both step and invocation set: two different axes, neither one lost
        // ("the two may both be set", nav.h).
        dt_link both;
        both.view = dt_view::slice;
        both.rec = "x.asmtrace";
        both.step = 42;
        both.invocation = 3;
        dt_link rt2;
        check("step+invocation link parses",
              dt_nav_parse(dt_nav_format(both), rt2, err), err);
        check("step survived alongside invocation",
              rt2.step.value_or(0) == 42, "step was lost");
        check("invocation survived alongside step",
              rt2.invocation.value_or(0) == 3, "invocation was lost");
        eq("step+invocation format is canonical", dt_nav_format(both),
           "asmtrace-link:v=slice&rec=x.asmtrace&step=42&invocation=3");

        // Appended LAST means after pid too.
        dt_link withpid;
        withpid.view = dt_view::topo;
        withpid.rec = "x.asmtrace";
        withpid.pid = 4242;
        withpid.invocation = 1;
        eq("invocation lands after pid", dt_nav_format(withpid),
           "asmtrace-link:v=topo&rec=x.asmtrace&pid=4242&invocation=1");

        // Rejected the way pid's unparseable value is: named, not silent.
        dt_link bad;
        check("an out-of-range invocation is rejected",
              !dt_nav_parse(
                  "asmtrace-link:v=canvas&rec=x&invocation=4294967296", bad,
                  err),
              "it was accepted");
        check("...with a reason", err.find("invocation") != std::string::npos,
              err);
        check("a non-numeric invocation is rejected",
              !dt_nav_parse("asmtrace-link:v=canvas&rec=x&invocation=abc",
                            bad, err),
              "it was accepted");
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

    // --- 09-T5: the blame intake target ------------------------------------
    // Wave-2's backward-slice blame producer does not exist yet, so this pins
    // the ROUTER half of the socket: a blame link is the failure-attribution
    // entry point, v=blame&rec=<fail>&step=<failing step>. RE-POINT this at a
    // produced recording when the producer lands. (The shell resolves the target
    // onto the slice explorer with the backward cone lit — asserted end to end
    // over the hand-authored fixture in test_shell.)
    {
        dt_link l;
        std::string err;
        check("a blame link parses",
              dt_nav_parse("asmtrace-link:v=blame&rec=fail.asmtrace&step=3", l,
                           err),
              err);
        check("it names the blame target", l.view == dt_view::blame,
              "a blame link must resolve to the blame target");
        check("it carries the failure step", l.step.value_or(0) == 3,
              "the step is the slice origin the producer names");
        eq("rec of the blame link", l.rec, "fail.asmtrace");

        // Byte-stable round-trip, exactly like every other target — a blame link
        // is something a failing test pastes into its output.
        eq("blame link round-trips byte-stable", dt_nav_format(l),
           "asmtrace-link:v=blame&rec=fail.asmtrace&step=3");
        dt_link l2;
        check("re-parse of the formatted blame link",
              dt_nav_parse(dt_nav_format(l), l2, err), err);
        eq("and re-formats identically", dt_nav_format(l2), dt_nav_format(l));

        // Distinct from a plain slice link: same coordinate, different v= token,
        // so a blame deep link is greppable as one rather than an anonymous
        // slice link.
        dt_link slice_l = l;
        slice_l.view = dt_view::slice;
        check("blame is not just a slice link",
              dt_nav_format(slice_l) != dt_nav_format(l),
              "the v= token must distinguish the blame entry point");

        // Forward-compat holds on the blame target too: an unknown key from a
        // newer producer is ignored and the known fields still arrive.
        dt_link l3;
        check("an unknown key on a blame link is ignored",
              dt_nav_parse(
                  "asmtrace-link:v=blame&rec=fail.asmtrace&step=3&def=eax", l3,
                  err),
              err);
        check("the step survived the unknown key", l3.step.value_or(0) == 3,
              "step was lost");

        // It ROUTES: a registered blame handler receives the link with its rec
        // and step, refusing loudly (like every target) when the recording is
        // not open.
        dt_nav_table t;
        t.have_recording = [](const std::string &id) {
            return id == "fail.asmtrace";
        };
        uint32_t got_step = 0;
        int hits = 0;
        dt_nav_register(t, dt_view::blame, [&](const dt_link &link) {
            hits++;
            got_step = link.step.value_or(0);
        });
        check("a blame link navigates", dt_nav_go(t, l), t.last_error);
        check("the blame handler ran", hits == 1, "it did not");
        check("with the failure step", got_step == 3, "step was not passed");
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

    // --- 18-T1: the faithful keymap overlay's `wired` flag --------------------
    // The overlay is generated ENTIRELY from this table, greying every !wired
    // row "planned" — so the advertised==wired invariant is a property of the
    // data. Today every advertised binding is wired (F1/F18's fix: no dead key
    // is advertised as live), and the convention keys the review named are
    // present. A future advertised-but-unmapped gesture must land as wired:false.
    {
        bool saw_fit = false, saw_sibling = false, saw_wasd = false,
             saw_step_aliases = false, saw_history = false, saw_reset = false;
        for (const dt_binding &b : dt_nav_bindings()) {
            check("every advertised binding is wired (no dead key)", b.wired,
                  std::string("row \"") + b.keys + "\" is advertised but not "
                  "wired — grey it 'planned' or wire it");
            std::string keys = b.keys;
            if (keys.find("F") != std::string::npos && keys.find("fit") ==
                std::string::npos && std::string(b.what).find("fit") !=
                std::string::npos)
                saw_fit = true;
            if (keys == ", / .")
                saw_sibling = true;
            if (keys.find("W/S") != std::string::npos)
                saw_wasd = true;
            if (keys.find("F10") != std::string::npos)
                saw_step_aliases = true;
            if (keys.find("Alt+Left") != std::string::npos)
                saw_history = true;
            if (keys.find("Ctrl+Shift+R") != std::string::npos)
                saw_reset = true;
        }
        check("the F fit-selection binding is advertised", saw_fit, "missing");
        check("the ,/. sibling-step binding is advertised", saw_sibling,
              "missing");
        check("the WASD camera binding is advertised", saw_wasd, "missing");
        check("the F10/F11 step aliases are advertised", saw_step_aliases,
              "missing");
        check("the Alt+Left/Right history binding is advertised", saw_history,
              "missing");
        check("the Ctrl+Shift+R reset binding is advertised", saw_reset,
              "missing");
    }

    // --- 18-T6: bounded back/forward history over the router ----------------
    {
        dt_nav_table t;
        t.have_recording = [](const std::string &) { return true; };
        for (dt_view v : dt_all_views())
            dt_nav_register(t, v, [](const dt_link &) {});
        auto mk = [](const char *rec, dt_view v, uint32_t step) {
            dt_link l;
            l.rec = rec;
            l.view = v;
            l.step = step;
            return l;
        };
        dt_link a = mk("a.asmtrace", dt_view::canvas, 1);
        dt_link b = mk("a.asmtrace", dt_view::timeline, 2);
        dt_link c = mk("a.asmtrace", dt_view::slice, 3);

        check("first go lands", dt_nav_go(t, a), t.last_error);
        check("first go pushes no back (no prior position)", t.back.empty(),
              "the first navigation has nothing to go back to");
        dt_nav_go(t, b);
        dt_nav_go(t, c);
        check("back stack grew with each fresh navigation", t.back.size() == 2,
              "expected [a, b]");
        check("forward is empty after fresh navigations", t.forward.empty(),
              "a fresh navigation must not leave a forward branch");

        // A no-op re-navigation to the current position is not a history step.
        size_t before = t.back.size();
        dt_nav_go(t, c);
        check("re-navigating to the current link pushes no history",
              t.back.size() == before, "a no-op move must not grow the stack");

        check("back walks to the previous position", dt_nav_back(t),
              t.last_error);
        check("...and lands there", dt_nav_format(*t.current) ==
              dt_nav_format(b), "back should land on b");
        check("back pushed the position we left onto forward",
              t.forward.size() == 1, "forward should carry c");
        dt_nav_back(t);
        check("back again lands on the earliest", dt_nav_format(*t.current) ==
              dt_nav_format(a), "back should land on a");
        check("at the earliest, back refuses loudly", !dt_nav_back(t),
              "an empty back stack must return false");
        check("...with a reason", !t.last_error.empty(), "no reason given");

        check("forward retraces", dt_nav_forward(t), t.last_error);
        check("...to b", dt_nav_format(*t.current) == dt_nav_format(b),
              "forward should land on b");

        // A fresh navigation after a back CLEARS the forward branch.
        dt_link d = mk("a.asmtrace", dt_view::diff, 4);
        dt_nav_go(t, d);
        check("a new navigation clears forward", t.forward.empty(),
              "the forward branch must not survive a divergent navigation");
        check("...so forward now refuses", !dt_nav_forward(t),
              "an empty forward stack must return false");

        // The whole stack is just serialisable links: it round-trips.
        for (const dt_link &l : t.back) {
            dt_link rt;
            std::string err;
            check("a history link round-trips through format/parse",
                  dt_nav_parse(dt_nav_format(l), rt, err) &&
                      dt_nav_format(rt) == dt_nav_format(l),
                  err);
        }

        // The bound: a long session drops the oldest rather than growing forever.
        dt_nav_table t2;
        t2.have_recording = [](const std::string &) { return true; };
        for (dt_view v : dt_all_views())
            dt_nav_register(t2, v, [](const dt_link &) {});
        t2.history_cap = 3;
        for (uint32_t i = 0; i < 20; i++)
            dt_nav_go(t2, mk("a.asmtrace", dt_view::canvas, i));
        check("the back stack respects its bound", t2.back.size() <= 3,
              "cap=3 must drop the oldest past the bound; got " +
                  std::to_string(t2.back.size()));

        // A back-jump to a recording that CLOSED refuses loudly and keeps the
        // stacks intact (the same discipline dt_nav_go has).
        dt_nav_table t3;
        bool open = true;
        t3.have_recording = [&open](const std::string &) { return open; };
        dt_nav_register(t3, dt_view::canvas, [](const dt_link &) {});
        dt_nav_register(t3, dt_view::timeline, [](const dt_link &) {});
        dt_nav_go(t3, mk("a.asmtrace", dt_view::canvas, 1));
        dt_nav_go(t3, mk("a.asmtrace", dt_view::timeline, 2));
        open = false; // the recording is gone
        size_t back_before = t3.back.size();
        check("back to a closed recording refuses", !dt_nav_back(t3),
              "a target that is no longer open must refuse");
        check("...and keeps the stack intact", t3.back.size() == back_before,
              "a refused back-jump must not pop the stack");
    }

    if (failures) {
        std::fprintf(stderr, "%d nav check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_nav: all checks passed\n");
    return 0;
}
