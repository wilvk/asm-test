/* glob_match.c — portable filename-glob matcher (fnmatch subset). See the header.
 *
 * Iterative `*` backtracking (linear-ish, no recursion): on a mismatch we fall
 * back to just-after the most recent `*` and advance the consumed text by one.
 */
#include "glob_match.h"

/* Match one `[...]` class against `c`. On entry *pp points just past '['; on
 * return *pp points just past the closing ']'. */
static int match_class(const char **pp, char c) {
    const char *p = *pp;
    int negate = 0;
    int matched = 0;

    if (*p == '!' || *p == '^') {
        negate = 1;
        p++;
    }
    /* A ']' immediately after '[' (or '[!') is a literal member. */
    if (*p == ']') {
        if (c == ']')
            matched = 1;
        p++;
    }
    while (*p != '\0' && *p != ']') {
        if (p[1] == '-' && p[2] != '\0' && p[2] != ']') {
            unsigned char lo = (unsigned char)p[0];
            unsigned char hi = (unsigned char)p[2];
            if ((unsigned char)c >= lo && (unsigned char)c <= hi)
                matched = 1;
            p += 3;
        } else {
            if (*p == c)
                matched = 1;
            p++;
        }
    }
    if (*p == ']')
        p++;
    *pp = p;
    return negate ? !matched : matched;
}

int asmtest_glob_match(const char *pattern, const char *str) {
    const char *star = 0; /* pattern position just after the last '*' */
    const char *ss = 0;   /* str position when that '*' was seen */

    while (*str != '\0') {
        if (*pattern == '?') {
            pattern++;
            str++;
        } else if (*pattern == '*') {
            star = ++pattern;
            ss = str;
        } else if (*pattern == '[') {
            const char *q = pattern + 1;
            if (match_class(&q, *str)) {
                pattern = q;
                str++;
            } else if (star != 0) {
                pattern = star;
                str = ++ss;
            } else {
                return 0;
            }
        } else if (*pattern == '\\' && pattern[1] != '\0') {
            if (pattern[1] == *str) {
                pattern += 2;
                str++;
            } else if (star != 0) {
                pattern = star;
                str = ++ss;
            } else {
                return 0;
            }
        } else if (*pattern == *str) {
            pattern++;
            str++;
        } else if (star != 0) {
            pattern = star;
            str = ++ss;
        } else {
            return 0;
        }
    }
    while (*pattern == '*')
        pattern++;
    return *pattern == '\0';
}
