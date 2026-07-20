/* tests/glob_parity.c — differential parity test: asmtest_glob_match against
 * the host's real fnmatch(pattern, str, 0) (src/glob_match.c, code-review-
 * plausible-triage T2).
 *
 * The Win64 runner's --filter uses asmtest_glob_match (mingw has no
 * fnmatch); the POSIX runner uses fnmatch directly. This binary is the
 * actual oracle both are pinned to: for every {pattern, str} pair below,
 * asmtest_glob_match must agree with the CI image's glibc fnmatch bit for
 * bit (glob == (fnmatch == 0)). POSIX-only (fnmatch isn't available under
 * mingw) — `make check` never runs in the Wine lane, so this is fine there;
 * tests/win64/test_glob.c carries the same divergent cases with the
 * expectations hard-coded to what fnmatch says on this oracle.
 *
 * Well-defined patterns are compared live against the host fnmatch on every
 * POSIX host (glibc and BSD agree there). The undefined-behavior patterns,
 * where glibc and BSD/macOS fnmatch legitimately diverge, are checked via
 * check_div() against glob_match's glibc-pinned contract directly, and cross-
 * checked against the host fnmatch only when it IS glibc (see check_div).
 */
#include <fnmatch.h>
#include <stdio.h>

#include "glob_match.h"

static int fails = 0;
static int total = 0;

static void check(const char *pattern, const char *str) {
    int g = asmtest_glob_match(pattern, str) ? 1 : 0;
    int f = (fnmatch(pattern, str, 0) == 0) ? 1 : 0;
    total++;
    if (g == f) {
        printf("ok   - glob_match(\"%s\", \"%s\") == fnmatch (%d)\n", pattern,
               str, g);
    } else {
        printf("FAIL - glob_match(\"%s\", \"%s\") = %d, fnmatch = %d\n",
               pattern, str, g, f);
        fails++;
    }
}

/* Divergent / undefined-behavior patterns (unterminated '[', in-class escapes,
 * trailing '\'): POSIX leaves these implementation-defined, and glibc and BSD
 * (macOS) fnmatch make different choices. asmtest_glob_match is pinned to
 * glibc's choice — the CI oracle the Win64 --filter must reproduce — so here we
 * assert that glibc-pinned result directly (the contract, checkable on every
 * host, exactly as tests/win64/test_glob.c hard-codes it), and ONLY where the
 * host fnmatch IS that glibc oracle (__GLIBC__) do we also cross-check it live.
 * Comparing glob_match against a non-glibc host fnmatch on these inputs would
 * be measuring the wrong oracle. */
static void check_div(const char *pattern, const char *str, int expected) {
    int g = asmtest_glob_match(pattern, str) ? 1 : 0;
    total++;
    if (g != expected) {
        printf("FAIL - glob_match(\"%s\", \"%s\") = %d, expected %d (glibc)\n",
               pattern, str, g, expected);
        fails++;
        return;
    }
#if defined(__GLIBC__)
    int f = (fnmatch(pattern, str, 0) == 0) ? 1 : 0;
    if (f != expected) {
        printf("FAIL - host glibc fnmatch(\"%s\", \"%s\") = %d, expected %d\n",
               pattern, str, f, expected);
        fails++;
        return;
    }
    printf("ok   - glob_match(\"%s\", \"%s\") == glibc fnmatch (%d)\n", pattern,
           str, g);
#else
    printf("ok   - glob_match(\"%s\", \"%s\") = %d (glibc-pinned; host fnmatch "
           "is not that oracle here)\n",
           pattern, str, g);
#endif
}

int main(void) {
    /* Every case already in tests/win64/test_glob.c. */
    check("foo", "foo");
    check("foo", "bar");
    check("foo", "foo2");
    check("*", "anything");
    check("*", "");
    check("foo*", "foobar");
    check("*bar", "foobar");
    check("f*r", "foobar");
    check("a*b*c", "axxbyyc");
    check("a*b*c", "axxbyy");
    check("f?o", "foo");
    check("f?o", "fo");
    check("f?o", "fooo");
    check("[abc]", "b");
    check("[abc]", "d");
    check("[a-z]", "m");
    check("[a-z]", "M");
    check("[!a-z]", "M");
    check("[!a-z]", "m");
    check("\\*", "*");
    check("\\*", "x");
    check("\\?", "?");
    check("test_*", "test_arith");
    check("arith.*", "arith.add");
    check("*.overflow", "math.overflow");

    /* Divergent cases: unterminated '[', in-class escapes, trailing '\'.
     * The expected value is glob_match's glibc-pinned result (see check_div). */
    check_div("test[", "test[", 1);
    check_div("a[bc", "a[bc", 1);
    check_div("[\\]]", "]", 1);
    check_div("a\\", "a\\", 0);
    check_div("a\\", "a", 0);
    check_div("[]", "[]", 1);
    check_div("[!ab", "[!ab", 1);
    check_div("*[", "x[", 1);
    check_div("[a\\-c]", "-", 1);

    /* A few more shapes around the same edges, for margin. */
    check_div("[", "[", 1);
    check_div("[a", "[a", 1);
    check_div("[^ab", "[^ab", 1);
    check_div("*\\", "anything\\", 0);
    check_div("*\\", "anything", 0);
    check_div("\\", "\\", 0);
    check_div("\\", "", 0);
    check_div("[a\\-c]", "a", 1);
    check_div("[a\\-c]", "c", 1);
    check_div("[a\\-c]", "b", 0);

    printf("\n%s (%d/%d failures)\n", fails ? "FAILED" : "PASSED", fails,
           total);
    return fails ? 1 : 0;
}
