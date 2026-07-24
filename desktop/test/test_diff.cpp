// test_diff.cpp — the alignment seam (04-replay-views.md T6).
//
// The rules under test are the ones that stop a diff from lying: a refused pair
// yields a REASON and no numbers, and a bounded verdict never says "identical".
#include "analysis/diff.h"
#include "view_test.h"

using namespace asmdesk;
using vt::load;

namespace {

// A minimal comparable recording, built by hand so the divergence cases are
// exactly what they say they are.
Streams mk(const std::vector<uint64_t> &insns,
           const std::vector<uint64_t> &blocks, bool truncated = false,
           const char *basis = "rel", const char *arch = "x86_64") {
    Streams s;
    s.trace.insns = insns;
    s.trace.blocks = blocks;
    s.trace.basis = basis;
    s.truncated = truncated;
    s.arch = arch;
    s.id = "hand";
    return s;
}

} // namespace

int main() {
    dt_diff d;
    std::string err;

    // --- two 5-step streams that part company at step 3 ---------------------
    {
        Streams a = mk({0, 2, 4, 6, 9}, {0, 6});
        Streams b = mk({0, 2, 4, 20, 22}, {0, 20});
        vt::check("a comparable pair builds", dt_diff_build(a, b, d, err), err);
        vt::check("it diverged", d.div.diverged,
                  "the streams differ at step 3");
        vt::eq("divergence step", d.div.step, 3u);
        vt::eq("A's offset there", d.div.off_a, uint64_t{6});
        vt::eq("B's offset there", d.div.off_b, uint64_t{20});
        vt::check("the verdict is not bounded", !d.div.bounded,
                  "neither side is truncated, so the whole run was compared");
        vt::eq("A-only blocks", d.only_a.size(), size_t{1});
        vt::eq("B-only blocks", d.only_b.size(), size_t{1});
        vt::eq("shared blocks", d.both.size(), size_t{1});
    }

    // --- identical streams --------------------------------------------------
    {
        Streams a = mk({0, 2, 4}, {0});
        Streams b = mk({0, 2, 4}, {0});
        vt::check("identical streams compare", dt_diff_build(a, b, d, err),
                  err);
        vt::check("no divergence", !d.div.diverged, "they are the same");
        vt::check("not bounded", !d.div.bounded, "neither is truncated");
        vt::check("no heat deltas", d.heat.empty(),
                  "identical streams have identical heat");
        vt::check("the dump says identical",
                  dt_diff_dump(d).find("identical") != std::string::npos,
                  dt_diff_dump(d));
    }

    // --- A truncated at step 2, agreeing so far: BOUNDED, not identical -----
    {
        Streams a = mk({0, 2}, {0}, /*truncated=*/true);
        Streams b = mk({0, 2, 4, 6}, {0, 6});
        vt::check("a truncated pair still compares",
                  dt_diff_build(a, b, d, err), err);
        vt::check("no divergence is CLAIMED", !d.div.diverged,
                  "A simply stopped; that is not evidence the runs differ");
        vt::check(
            "the verdict is bounded", d.div.bounded,
            "A is truncated, so agreement past step 2 was never observed");
        std::string dump = dt_diff_dump(d);
        vt::check("the dump never says identical",
                  dump.find("identical") == std::string::npos,
                  "a bounded verdict must not claim identity:\n" + dump);
        vt::check("the dump says the window bounds it",
                  dump.find("within the recorded window") != std::string::npos,
                  dump);
    }

    // --- B shorter and NOT truncated: that IS a divergence -------------------
    {
        Streams a = mk({0, 2, 4, 6}, {0});
        Streams b = mk({0, 2}, {0});
        vt::check("compares", dt_diff_build(a, b, d, err), err);
        vt::check("a clean short stream is a real divergence", d.div.diverged,
                  "B ran to completion in two instructions; A did not");
        vt::eq("it diverges where B ends", d.div.step, 2u);
    }

    // --- refusals: a reason, and NO numbers ---------------------------------
    {
        Streams a = mk({0, 2}, {0}, false, "rel");
        Streams b = mk({0, 2}, {0}, false, "abs");
        vt::check("a mixed-basis pair is REFUSED", !dt_diff_build(a, b, d, err),
                  "it must not compare");
        vt::check("the reason names both bases",
                  err.find("rel") != std::string::npos &&
                      err.find("abs") != std::string::npos,
                  err);
        vt::check("a refused diff carries no coverage numbers",
                  d.only_a.empty() && d.only_b.empty() && d.both.empty(),
                  "a refusal must be empty, not plausible");
        vt::check("and no divergence claim", !d.div.diverged, "still claimed");
        vt::check("the dump leads with the refusal",
                  dt_diff_dump(d).rfind("refused:", 0) == 0, dt_diff_dump(d));
    }
    {
        Streams a = mk({0, 2}, {0}, false, "rel", "x86_64");
        Streams b = mk({0, 2}, {0}, false, "rel", "aarch64");
        vt::check("a cross-arch pair is REFUSED", !dt_diff_build(a, b, d, err),
                  "the same offset is a different instruction");
        vt::check("the reason names the architectures",
                  err.find("x86_64") != std::string::npos &&
                      err.find("aarch64") != std::string::npos,
                  err);
    }
    {
        Streams a = mk({0, 2}, {0});
        Streams b;
        b.arch = "x86_64";
        b.id = "empty";
        vt::check("a pair with no comparable stream is REFUSED",
                  !dt_diff_build(a, b, d, err), "nothing to align");
        vt::check("the reason says which side", !err.empty(), "empty reason");
    }

    // --- the identity gap is stated, never assumed away ---------------------
    {
        Streams a = mk({0, 2}, {0});
        Streams b = mk({0, 2}, {0});
        vt::check("compares", dt_diff_build(a, b, d, err), err);
        vt::check("the identity note is always present",
                  !d.identity_note.empty(),
                  "every diff must say what it did and did not verify");
        vt::check("it says routine identity was NOT checked",
                  d.identity_note.find("NOT checked") != std::string::npos,
                  d.identity_note);
    }

    // --- the golden pair, from committed fixtures ---------------------------
    {
        Streams a = load("views/pair-a.asmtrace");
        Streams b = load("views/pair-b.asmtrace");
        vt::check("the golden pair compares", dt_diff_build(a, b, d, err), err);
        vt::eq("golden divergence step", d.div.step, 3u);
        vt::check("the statistical hot-edge delta is kept apart",
                  !d.edges.empty(),
                  "both fixtures carry a survey edge; the deltas belong here");
        vt::check("the dump labels the edge rows statistical",
                  dt_diff_dump(d).find("[statistical]") != std::string::npos,
                  dt_diff_dump(d));
        vt::golden("diff-pair.txt", dt_diff_dump(d));
    }

    // --- two DIFFERENT routines still compare, with the gap stated ----------
    // add_signed and sum3 are different routines in the same basis and arch.
    // v1 cannot tell, and this is exactly what the identity note is for.
    {
        Streams a = load("add_signed.asmtrace");
        Streams b = load("sum3.asmtrace");
        vt::check("v1 cannot refuse a wrong-routine pair",
                  dt_diff_build(a, b, d, err), err);
        vt::check("so it says so, loudly",
                  d.identity_note.find("NOT checked") != std::string::npos,
                  d.identity_note);
        vt::golden("diff-different-routines.txt", dt_diff_dump(d));
    }

    return vt::report("test_diff");
}
