/* test_glob.c — the portable glob matcher (src/glob_match.c) used for the Win64
 * runner's --filter. Platform-neutral, so it runs natively (and under Wine).
 */
#include <stdio.h>

#include "glob_match.h"

static int fails = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (cond) {                                                            \
            printf("ok   - %s\n", (msg));                                      \
        } else {                                                               \
            printf("FAIL - %s\n", (msg));                                      \
            fails++;                                                           \
        }                                                                      \
    } while (0)

#define M(p, s) asmtest_glob_match((p), (s))

int main(void) {
    CHECK(M("foo", "foo") && !M("foo", "bar") && !M("foo", "foo2"),
          "literals match exactly");
    CHECK(M("*", "anything") && M("*", ""), "* matches everything incl. empty");
    CHECK(M("foo*", "foobar") && M("*bar", "foobar") && M("f*r", "foobar"),
          "* matches a run anywhere");
    CHECK(M("a*b*c", "axxbyyc") && !M("a*b*c", "axxbyy"),
          "multiple * backtrack correctly");
    CHECK(M("f?o", "foo") && !M("f?o", "fo") && !M("f?o", "fooo"),
          "? matches exactly one character");
    CHECK(M("[abc]", "b") && !M("[abc]", "d"), "[...] character class");
    CHECK(M("[a-z]", "m") && !M("[a-z]", "M"), "[a-z] range");
    CHECK(M("[!a-z]", "M") && !M("[!a-z]", "m"), "[!...] negated class");
    CHECK(M("\\*", "*") && !M("\\*", "x") && M("\\?", "?"),
          "\\ escapes a metacharacter");
    /* The shapes the runner's --filter actually uses (id / suite / name). */
    CHECK(M("test_*", "test_arith") && M("arith.*", "arith.add") &&
              M("*.overflow", "math.overflow"),
          "test-id filter patterns");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
