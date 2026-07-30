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
           const char *basis = "rel", const char *arch = "x86_64",
           const char *code_sha = "") {
    Streams s;
    s.trace.insns = insns;
    s.trace.blocks = blocks;
    s.trace.basis = basis;
    s.truncated = truncated;
    s.arch = arch;
    s.id = "hand";
    if (code_sha && code_sha[0]) {
        s.code_present = true;
        s.code_sha = code_sha;
        s.code_name = "hand";
        s.code_len = 64;
    }
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

    // --- routine identity from a `code` header (28 R1 T1) --------------------
    {
        std::string sha(64, 'a');
        // Matching sha256: identity is now a FINDING, not the reader's caveat.
        Streams a = mk({0, 2, 4}, {0}, false, "rel", "x86_64", sha.c_str());
        Streams b = mk({0, 2, 4}, {0}, false, "rel", "x86_64", sha.c_str());
        vt::check("a matching-code pair compares", dt_diff_build(a, b, d, err),
                  err);
        vt::check("routine identity is now a finding",
                  d.identity_note.find("routine identity") !=
                          std::string::npos &&
                      d.identity_note.find("matches") != std::string::npos,
                  d.identity_note);
        vt::check("it no longer states the caveat",
                  d.identity_note.find("NOT checked") == std::string::npos,
                  d.identity_note);
    }
    {
        std::string sa(64, 'a'), sb(64, 'b');
        // Differing sha256: a wrong-routine pair is REFUSED, with a reason and
        // no numbers — the same discipline as a basis or arch mismatch.
        Streams a = mk({0, 2, 4}, {0}, false, "rel", "x86_64", sa.c_str());
        Streams b = mk({0, 2, 4}, {0}, false, "rel", "x86_64", sb.c_str());
        vt::check("a differing-code pair is REFUSED",
                  !dt_diff_build(a, b, d, err), "different routines refuse");
        vt::check("the reason names different routines",
                  err.find("different routines") != std::string::npos, err);
        vt::check("a refused diff carries no coverage numbers",
                  d.only_a.empty() && d.only_b.empty() && d.both.empty(),
                  "a refusal must be empty, not plausible");
    }
    {
        std::string sa(64, 'c');
        // One side carries `code`, the other does not: the hashes cannot be
        // compared, so the faithful caveat STANDS — a code-less v1 recording is
        // still real, it just cannot prove sameness.
        Streams a = mk({0, 2, 4}, {0}, false, "rel", "x86_64", sa.c_str());
        Streams b = mk({0, 2, 4}, {0});
        vt::check("a one-sided-code pair still compares",
                  dt_diff_build(a, b, d, err), err);
        vt::check("the caveat stands",
                  d.identity_note.find("NOT checked") != std::string::npos,
                  d.identity_note);
        vt::check("it names the one-sided gap",
                  d.identity_note.find("only the first") != std::string::npos,
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

    // --- two DIFFERENT routines are now REFUSED by their code identity ------
    // add_signed and sum3 are different routines; the regenerated corpus carries
    // a `code` header (28 R1 T1), so their byte hashes differ and the diff
    // REFUSES the pair rather than stating an unverifiable caveat. This flip is
    // the whole point of T1 — the gap that test used to pin is now closed.
    {
        Streams a = load("add_signed.asmtrace");
        Streams b = load("sum3.asmtrace");
        vt::check("both goldens carry a code identity",
                  a.code_present && b.code_present,
                  "the regenerated corpus must carry the code header");
        vt::check("a wrong-routine pair is now REFUSED",
                  !dt_diff_build(a, b, d, err),
                  "different sha256 must refuse the diff");
        vt::check("the reason names different routines",
                  err.find("different routines") != std::string::npos, err);
        vt::golden("diff-different-routines.txt", dt_diff_dump(d));
    }

    // --- the SAME routine, twice: identity is a finding ---------------------
    {
        Streams a = load("add_signed.asmtrace");
        Streams b = load("add_signed.asmtrace");
        vt::check("same-routine goldens compare", dt_diff_build(a, b, d, err),
                  err);
        vt::check("routine identity is a finding, not a caveat",
                  d.identity_note.find("routine identity") !=
                          std::string::npos &&
                      d.identity_note.find("NOT checked") == std::string::npos,
                  d.identity_note);
    }

    // --- the two-recording state-diff (33 R6 T2) ----------------------------
    // A helper that arms a statediff stream: each entry is {step, computed,
    // {reg: new value}}. The first held step is the baseline (computed false).
    auto with_sd = [](Streams s,
                      std::vector<StateDelta> sd) -> Streams {
        s.statediff = std::move(sd);
        return s;
    };

    {
        // Two matched-identity recordings whose state evolves IDENTICALLY: the
        // merge runs (identity is a finding) and reports no divergence.
        Streams a = with_sd(mk({0, 2, 4}, {0}, false, "rel", "x86_64", "abc"),
                            {{0, {}, false}, {1, {{"rax", 7}}, true}});
        Streams b = with_sd(mk({0, 2, 4}, {0}, false, "rel", "x86_64", "abc"),
                            {{0, {}, false}, {1, {{"rax", 7}}, true}});
        dt_statediff m = dt_statediff_build(a, b);
        vt::check("statediff merge runs on a matched pair", m.merged, m.err);
        vt::check("identical state evolution => no register divergence",
                  [&] {
                      for (const auto &ss : m.steps)
                          if (!ss.regs.empty())
                              return false;
                      return true;
                  }(),
                  dt_statediff_dump(m));
        // step 0 is bounded on both sides (baseline, computed false).
        vt::check("the baseline step is bounded, not a fabricated delta",
                  !m.steps.empty() && m.steps.front().step == 0 &&
                      m.steps.front().bounded,
                  dt_statediff_dump(m));
    }
    {
        // Same routine, DIFFERENT state at step 1 (rax 7 vs 9): the merge names
        // rax as the diverging register at that step.
        Streams a = with_sd(mk({0, 2, 4}, {0}, false, "rel", "x86_64", "abc"),
                            {{0, {}, false}, {1, {{"rax", 7}}, true}});
        Streams b = with_sd(mk({0, 2, 4}, {0}, false, "rel", "x86_64", "abc"),
                            {{0, {}, false}, {1, {{"rax", 9}}, true}});
        dt_statediff m = dt_statediff_build(a, b);
        vt::check("a state divergence is found", m.merged && !m.steps.empty(),
                  dt_statediff_dump(m));
        bool named = false;
        for (const auto &ss : m.steps)
            if (ss.step == 1 && ss.regs.size() == 1 && ss.regs[0] == "rax")
                named = true;
        vt::check("the diverging register is named at its step", named,
                  dt_statediff_dump(m));
    }
    {
        // A wrong-routine pair is REFUSED by identity before any state merges —
        // the R1 T1 gate, reused.
        Streams a = with_sd(mk({0, 2}, {0}, false, "rel", "x86_64", "aaa"),
                            {{0, {}, false}});
        Streams b = with_sd(mk({0, 2}, {0}, false, "rel", "x86_64", "bbb"),
                            {{0, {}, false}});
        dt_statediff m = dt_statediff_build(a, b);
        vt::check("a code-mismatched pair is refused before merging",
                  !m.err.empty() && !m.merged, "must refuse by identity");
        vt::check("the refusal carries no fabricated steps", m.steps.empty(),
                  "a refused merge produces nothing");
    }
    {
        // A code-LESS pair keeps the faithful caveat (identity not checked) but
        // still merges what it has.
        Streams a = with_sd(mk({0, 2}, {0}), {{0, {}, false}});
        Streams b = with_sd(mk({0, 2}, {0}), {{0, {}, false}});
        dt_statediff m = dt_statediff_build(a, b);
        vt::check("a code-less pair still merges", m.merged, m.err);
        vt::check("but keeps the identity caveat",
                  m.identity_note.find("NOT checked") != std::string::npos,
                  m.identity_note);
    }
    {
        // No statediff on one side: the merge says so rather than presenting an
        // empty comparison as agreement.
        Streams a = with_sd(mk({0, 2}, {0}, false, "rel", "x86_64", "abc"),
                            {{0, {}, false}});
        Streams b = mk({0, 2}, {0}, false, "rel", "x86_64", "abc");
        dt_statediff m = dt_statediff_build(a, b);
        vt::check("a missing statediff stream is not silently agreed",
                  !m.merged && !m.note.empty(), dt_statediff_dump(m));
    }

    return vt::report("test_diff");
}
