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

    /* Divergent cases: unterminated '[', in-class escapes, trailing '\'. */
    check("test[", "test[");
    check("a[bc", "a[bc");
    check("[\\]]", "]");
    check("a\\", "a\\");
    check("a\\", "a");
    check("[]", "[]");
    check("[!ab", "[!ab");
    check("*[", "x[");
    check("[a\\-c]", "-");

    /* A few more shapes around the same edges, for margin. */
    check("[", "[");
    check("[a", "[a");
    check("[^ab", "[^ab");
    check("*\\", "anything\\");
    check("*\\", "anything");
    check("\\", "\\");
    check("\\", "");
    check("[a\\-c]", "a");
    check("[a\\-c]", "c");
    check("[a\\-c]", "b");

    printf("\n%s (%d/%d failures)\n", fails ? "FAILED" : "PASSED", fails,
           total);
    return fails ? 1 : 0;
}
