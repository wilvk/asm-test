// test_diff_view.cpp — the two-recording summary panel (04-replay-views.md T7).
#include "nav.h"
#include "view_test.h"
#include "views/diff_view.h"

using namespace asmdesk;
using vt::load;

int main() {
    // --- the golden pair: every navigable row's link parses BACK ------------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/pair-b.asmtrace");
        dt_diff_view v = dt_diff_view_build(a, b);
        vt::check("the pair is not refused", v.refusal.empty(), v.refusal);
        vt::eq("A id", v.a_id, std::string("pair-a.asmtrace"));
        vt::eq("B id", v.b_id, std::string("pair-b.asmtrace"));

        bool saw_divergence = false, saw_statistical = false;
        size_t linked = 0;
        for (const dt_diff_row &r : v.rows) {
            if (r.kind == dt_diff_row_kind::divergence)
                saw_divergence = true;
            if (r.statistical)
                saw_statistical = true;
            if (r.link.empty())
                continue;
            linked++;
            // A link a view emits must be a link the router can consume. This
            // is the round trip that keeps the deep-link spine honest.
            dt_link parsed;
            std::string err;
            vt::check("row link parses: " + r.link,
                      dt_nav_parse(r.link, parsed, err), err);
            vt::eq("round-trip is byte-stable", dt_nav_format(parsed), r.link);
        }
        vt::check("some rows are navigable", linked > 0, "none were");
        vt::check("the divergence card is there", saw_divergence, "missing");
        vt::check("hot-edge rows are chipped statistical", saw_statistical,
                  "both fixtures carry a survey edge");

        // The divergence card must link to the right step.
        for (const dt_diff_row &r : v.rows) {
            if (r.kind != dt_diff_row_kind::divergence || r.link.empty())
                continue;
            dt_link parsed;
            std::string err;
            if (dt_nav_parse(r.link, parsed, err)) {
                vt::eq("the divergence link's step", parsed.step.value_or(999),
                       3u);
                vt::check("it opens the timeline",
                          parsed.view == dt_view::timeline, "wrong view");
                vt::eq("it carries both recordings", parsed.rec_b, v.b_id);
            }
        }
        vt::golden("diff_view-pair.txt", dt_diff_view_dump(v));
    }

    // --- a refused pair: the reason, and nothing that looks like a result ---
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/mixed-basis.asmtrace");
        dt_diff_view v = dt_diff_view_build(a, b);
        vt::check("an unplaceable B is refused", !v.refusal.empty(),
                  "it must not compare");
        vt::eq("a refusal renders exactly one row", v.rows.size(), size_t{1});
        vt::check("that row is not a result",
                  v.rows[0].kind == dt_diff_row_kind::note,
                  "a refusal is a note, never a coverage or heat row");
        vt::check("and carries no link", v.rows[0].link.empty(),
                  "there is nowhere to navigate to");
        vt::golden("diff_view-refused.txt", dt_diff_view_dump(v));
    }

    // --- a bounded pair never claims identity -------------------------------
    {
        Streams a = load("views/trunc-trace.asmtrace");
        Streams b = load("views/trunc-trace.asmtrace");
        b.id = "trunc-trace-b.asmtrace";
        dt_diff_view v = dt_diff_view_build(a, b);
        std::string dump = dt_diff_view_dump(v);
        // The affirmative identity verdict — the exact sentence the panel uses
        // when it HAS compared two complete recordings — must be absent. (The
        // bounded wording deliberately contains the word "identical" in the
        // clause disclaiming it, so match the claim, not the word.)
        vt::check("byte-identical truncated streams are not CLAIMED identical",
                  dump.find("the recorded instruction streams are identical") ==
                      std::string::npos,
                  "a bounded verdict must not claim identity:\n" + dump);
        vt::check("it says the window bounds the verdict instead",
                  dump.find("within the recorded window") != std::string::npos,
                  dump);
        vt::check("the panel says both sides are truncated",
                  dump.find("both recordings are truncated") !=
                      std::string::npos,
                  dump);
    }

    // --- the heat cap is announced when it bites ----------------------------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/pair-b.asmtrace");
        dt_diff_view v = dt_diff_view_build(a, b, /*top_heat=*/1);
        bool announced = false;
        for (const dt_diff_row &r : v.rows)
            if (r.text.find("largest of") != std::string::npos)
                announced = true;
        vt::check("a truncated row list says how much it dropped", announced,
                  "a silent top-N reads as 'that was everything'");
    }

    return vt::report("test_diff_view");
}
