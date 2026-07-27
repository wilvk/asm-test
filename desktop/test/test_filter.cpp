// test_filter.cpp — the one filter affordance + column sort + the discrete-time
// variant (24-one-visual-language.md T4 / F16). Model state, not pixels (D4):
//  (a) the unified filter reports the correct "N of M" for a known query, and an
//      empty query shows N == M;
//  (b) the sortable table reorders VIEW indices under a given sort spec while the
//      underlying model order is unchanged;
//  (c) the discrete time variant carries a non-empty intentional-honesty reason
//      for each discrete surface (Invocations, Disassembly logical time), so the
//      discreteness is marked as deliberate, not a missing scrubber.
#include <cstdio>
#include <string>
#include <vector>

#include "ui/filter.h"
#include "ui/timepos.h"

using namespace asmdesk;

static int failures;
static void check(const char *what, bool cond, const std::string &why) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s: %s\n", what, why.c_str());
        failures++;
    }
}

int main() {
    // --- (a) N of M -----------------------------------------------------------
    std::vector<std::string> items = {"read",  "write", "openat",
                                      "close", "mmap",  "readv"};
    int total = -1;
    int n = dt_filter_count("read", items, &total);
    check("M", total == 6, "M should be the full count");
    check("N", n == 2, "'read' should match 'read' and 'readv' (got " +
                           std::to_string(n) + ")");
    int n2 = dt_filter_count("", items, &total);
    check("empty-query", n2 == total && total == 6,
          "an empty query must show N == M");
    check("case-insensitive", dt_filter_count("READ", items, nullptr) == 2,
          "matching must be case-insensitive");
    check("no-match", dt_filter_count("zzz", items, nullptr) == 0,
          "an unmatched query shows 0");
    check("match-blank-space", dt_filter_match("   ", "anything"),
          "a whitespace-only query is the neutral show-all state");

    // --- (b) sort reorders the VIEW, not the model ----------------------------
    std::vector<double> keys = {5.0, 1.0, 3.0, 9.0}; // e.g. sample counts
    std::vector<int> asc = dt_sorted_order(keys, true);
    check("sort-asc", asc.size() == 4 && asc[0] == 1 && asc[1] == 2 &&
                          asc[2] == 0 && asc[3] == 3,
          "ascending order should be indices [1,2,0,3]");
    std::vector<int> desc = dt_sorted_order(keys, false);
    check("sort-desc", desc[0] == 3 && desc[3] == 1,
          "descending should put the largest key first");
    // The model container is untouched: dt_sorted_order returns indices only.
    check("model-untouched",
          keys[0] == 5.0 && keys[1] == 1.0 && keys[2] == 3.0 && keys[3] == 9.0,
          "sorting must reorder view indices, never the model");
    // Stable on ties.
    std::vector<double> ties = {2.0, 2.0, 1.0};
    std::vector<int> to = dt_sorted_order(ties, true);
    check("sort-stable", to[0] == 2 && to[1] == 0 && to[2] == 1,
          "equal keys keep their original relative order (stable)");

    // --- (c) the discrete-time variant is MARKED intentional ------------------
    check("invocations-reason",
          std::string(dt_timepos_discrete_reason("invocations")).find(
              "unobserved") != std::string::npos,
          "the Invocations discrete reason must state why it is not a scrub");
    check("disasm-reason",
          dt_timepos_discrete_reason("disasm-logical-time")[0] != '\0',
          "the disassembly logical-time step must carry a reason");
    check("unknown-surface",
          dt_timepos_discrete_reason("nope")[0] == '\0',
          "an unregistered surface has no discrete reason");

    if (failures) {
        std::fprintf(stderr, "%d test_filter check(s) failed\n", failures);
        return 1;
    }
    std::printf("test_filter: all checks passed\n");
    return 0;
}
