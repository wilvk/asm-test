/* glob_match.h — a small portable filename-glob matcher (an fnmatch subset).
 *
 * The POSIX runner uses fnmatch() for --filter; MinGW (the native Win64 tier)
 * has no fnmatch, so the Win32 runner uses this instead. Platform-neutral, so it
 * also builds and is tested on the host. Supports `*`, `?`, `[...]` classes (with
 * `a-z` ranges and a leading `!`/`^` negation), and `\` escaping — matching
 * fnmatch with flags 0 for the patterns test-id filtering uses. Parity with
 * fnmatch(flags=0) (proven differentially against the host's real fnmatch by
 * tests/glob_parity.c) extends to its three easy-to-miss edge rules: an
 * unterminated `[` class is a literal `[`, `\` escapes a class member (or
 * range endpoint) to a plain literal instead of a range operator, and a
 * pattern ending in an unescaped `\` never matches anything.
 */
#ifndef ASMTEST_GLOB_MATCH_H
#define ASMTEST_GLOB_MATCH_H

/* Returns 1 if `str` matches the glob `pattern`, else 0. */
int asmtest_glob_match(const char *pattern, const char *str);

#endif /* ASMTEST_GLOB_MATCH_H */
