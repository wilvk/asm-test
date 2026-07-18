/* glob_match.c — portable filename-glob matcher (fnmatch subset). See the header.
 *
 * Iterative `*` backtracking (linear-ish, no recursion): on a mismatch we fall
 * back to just-after the most recent `*` and advance the consumed text by one.
 */
#include "glob_match.h"

#include <stddef.h> /* NULL */

/* `p` points just past a class's opening '['. Skip an optional negation
 * (`!`/`^`) and an optional leading literal ']', then scan for the closing
 * ']', honoring `\x` escape pairs so an escaped ']' doesn't end the class
 * early. Returns a pointer to the closing ']', or NULL if the class runs off
 * the end of the string unterminated — fnmatch(flags=0) then treats the
 * opening '[' as an ordinary literal character instead of a class. */
static const char *class_end(const char *p) {
    if (*p == '!' || *p == '^')
        p++;
    if (*p == ']')
        p++;
    while (*p != '\0' && *p != ']') {
        if (*p == '\\' && p[1] != '\0')
            p += 2;
        else
            p++;
    }
    return (*p == ']') ? p : NULL;
}

/* Match one `[...]` class against `c`. On entry *pp points just past '['; on
 * return *pp points just past the closing ']'. Caller must already know the
 * class is terminated (class_end), so this never runs off the string. `\x`
 * escapes a member (or a range endpoint) to a plain literal, exactly as
 * fnmatch honors escaping inside a bracket expression without FNM_NOESCAPE. */
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
        unsigned char lo;
        int lo_len;
        if (*p == '\\' && p[1] != '\0') {
            lo = (unsigned char)p[1];
            lo_len = 2;
        } else {
            lo = (unsigned char)p[0];
            lo_len = 1;
        }
        if (p[lo_len] == '-' && p[lo_len + 1] != '\0' && p[lo_len + 1] != ']') {
            const char *hp = p + lo_len + 1;
            unsigned char hi;
            int hi_len;
            if (*hp == '\\' && hp[1] != '\0') {
                hi = (unsigned char)hp[1];
                hi_len = 2;
            } else {
                hi = (unsigned char)hp[0];
                hi_len = 1;
            }
            if ((unsigned char)c >= lo && (unsigned char)c <= hi)
                matched = 1;
            p = hp + hi_len;
        } else {
            if ((unsigned char)c == lo)
                matched = 1;
            p += lo_len;
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
        } else if (*pattern == '[' && class_end(pattern + 1) != NULL) {
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
        } else if (*pattern == '\\') {
            /* POSIX: a pattern ending in an unescaped backslash shall not
             * match, regardless of what precedes it or what's left of str —
             * there is no character left in the pattern for it to escape. */
            if (pattern[1] == '\0') {
                return 0;
            } else if (pattern[1] == *str) {
                pattern += 2;
                str++;
            } else if (star != 0) {
                pattern = star;
                str = ++ss;
            } else {
                return 0;
            }
        } else if (*pattern == *str) {
            /* Also covers an unterminated '[' (class_end returned NULL
             * above): fnmatch treats it as an ordinary literal character. */
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
    /* A trailing unescaped backslash (pattern == "\\" here) fails this
     * check too, since it is never '\0' — no separate mirror needed. */
    return *pattern == '\0';
}
