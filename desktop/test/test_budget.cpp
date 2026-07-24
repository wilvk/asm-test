// test_budget.cpp — the D6 concurrency budget, every mode PAIR
// (07-serve-live-host.md T4).
//
// This is the doc's own "cheapest highest-value test", and the reason is worth
// stating: the budget is a claim about what the kernel will allow, but the
// thing that can actually be WRONG is the table — one mode mis-marked as free
// and the UI cheerfully fires a second attach that cannot succeed. The table is
// pure, so all 100 pairs are checked here on any host, and the live lanes are
// then only responsible for the wiring.
//
// It links budget.o and NOTHING else.
#include <cstdio>
#include <string>
#include <vector>

#include "live/budget.h"

using namespace asmdesk;

static int failures;

static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

int main(void) {
    const std::vector<LiveMode> &modes = all_modes();
    check("modes/count", modes.size() == 10,
          "the protocol defines 10 modes; got " + std::to_string(modes.size()));

    // The ONE free view, stated positively so a future mode marked free by
    // accident fails here rather than in production.
    int free_count = 0;
    for (LiveMode m : modes)
        if (!mode_uses_ptrace(m)) {
            free_count++;
            check("free/is-sample", mode_name(m) == std::string("sample"),
                  std::string("only `sample` reads out of band; `") +
                      mode_name(m) + "` must not be marked free");
        }
    check("free/count", free_count == 1,
          "exactly one mode is out of band; got " + std::to_string(free_count));

    // auto ENDS UP holding the jack even though it starts by sampling. A mode
    // is judged by the jack it holds, not by how it begins.
    LiveMode autom;
    check("auto/known", mode_from_name("auto", &autom), "auto should parse");
    check("auto/holds-jack", mode_uses_ptrace(autom),
          "auto captures with the data-flow engine, so it takes the jack");

    // Every mode names its structural reason — a refusal with no reason is a
    // dead end rather than a next step.
    for (LiveMode m : modes) {
        check("reason/nonempty", mode_jack_reason(m)[0] != '\0',
              std::string(mode_name(m)) + " has no jack reason");
        LiveMode rt;
        check("name/roundtrip", mode_from_name(mode_name(m), &rt) && rt == m,
              std::string("name round-trip failed for ") + mode_name(m));
    }
    check("name/unknown-refused", !mode_from_name("nonesuch", nullptr),
          "an unknown mode name must be refused, not guessed");

    // ---- every ordered pair ------------------------------------------------
    int pairs = 0, allowed = 0, refused = 0;
    for (LiveMode active : modes) {
        for (LiveMode want : modes) {
            pairs++;
            std::vector<LiveMode> live{active};
            BudgetDecision d = budget_can_start(want, live);
            // THE rule: refused exactly when both want and the active view
            // need the jack. Everything else is allowed.
            bool expect_refused =
                mode_uses_ptrace(want) && mode_uses_ptrace(active);
            check("pair/decision", d.allowed == !expect_refused,
                  std::string("start ") + mode_name(want) + " while " +
                      mode_name(active) + " runs: got " +
                      (d.allowed ? "allowed" : "refused") + ", want " +
                      (expect_refused ? "refused" : "allowed"));
            if (d.allowed) {
                allowed++;
                check("pair/allowed-quiet", d.reason.empty(),
                      "an allowed decision must carry no refusal prose");
                check("pair/allowed-nolabel", budget_blocked_label(d).empty(),
                      "an allowed decision must render no blocked label");
            } else {
                refused++;
                check("pair/blocker", d.blocker == active,
                      std::string("the refusal must name ") +
                          mode_name(active) + " as the blocker");
                // The refusal has to be actionable: it names the holder AND
                // the mode that was refused, so the UI can offer a swap.
                check("pair/names-both",
                      d.reason.find(mode_name(active)) != std::string::npos &&
                          d.reason.find(mode_name(want)) != std::string::npos,
                      "the refusal must name both the holder and the refused "
                      "mode");
                check("pair/label",
                      budget_blocked_label(d).find("holds the tracer") !=
                          std::string::npos,
                      "the blocked label must say the tracer is held");
            }
        }
    }
    check("pairs/exhaustive", pairs == 100,
          "expected 100 ordered pairs, walked " + std::to_string(pairs));
    // 9 ptrace modes x 9 = 81 refusals; the rest allowed. Asserting the split
    // stops a table that trivially allows (or refuses) everything from passing
    // the per-pair check by agreeing with a broken expectation.
    check("pairs/refused-count", refused == 81,
          "expected 81 refusals, got " + std::to_string(refused));
    check("pairs/allowed-count", allowed == 19,
          "expected 19 allowed, got " + std::to_string(allowed));

    // ---- the two facts the doc calls out by name ---------------------------
    // procs and watch ARE consumers. They are the two most easily forgotten:
    // procs looks like a listing, watch like a breakpoint.
    LiveMode procs, watch, sample, log;
    mode_from_name("procs", &procs);
    mode_from_name("watch", &watch);
    mode_from_name("sample", &sample);
    mode_from_name("log", &log);
    check("procs/consumer", mode_uses_ptrace(procs),
          "procs SEIZEs the descendant tree — it is a consumer");
    check("watch/consumer", mode_uses_ptrace(watch),
          "watch SEIZEs every thread to arm debug registers — it is a "
          "consumer");
    check("procs/scope-is-tree",
          std::string(mode_jack_reason(procs)).find("TREE") !=
              std::string::npos,
          "procs' reason must state the jack is per-TREE, not per-process");

    // A free view neither blocks nor is blocked, in both directions.
    check("sample/never-blocks", budget_can_start(log, {sample}).allowed,
          "a running sample must not block a ptrace view");
    check("sample/never-blocked", budget_can_start(sample, {log}).allowed,
          "a running ptrace view must not block sample");
    check("sample/pairs-with-itself",
          budget_can_start(sample, {sample}).allowed,
          "two out-of-band views can coexist");

    // Nothing active: everything starts.
    for (LiveMode m : modes)
        check("empty/allows", budget_can_start(m, {}).allowed,
              std::string("an idle target must allow ") + mode_name(m));

    // Multiple actives: the FIRST ptrace holder is named, and a free view in
    // the list does not mask it.
    check("multi/blocked", !budget_can_start(log, {sample, watch}).allowed,
          "a ptrace holder anywhere in the active set must block");
    check("multi/names-holder",
          budget_can_start(log, {sample, watch}).blocker == watch,
          "the named blocker must be the ptrace holder, not the free view");

    if (failures) {
        std::fprintf(stderr, "test_budget: %d FAILURE(S)\n", failures);
        return 1;
    }
    std::printf("test_budget: all checks passed (%d mode pairs)\n", pairs);
    return 0;
}
