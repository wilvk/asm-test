/* glob_match.h — a small portable filename-glob matcher (an fnmatch subset).
 *
 * The POSIX runner uses fnmatch() for --filter; MinGW (the native Win64 tier)
 * has no fnmatch, so the Win32 runner uses this instead. Platform-neutral, so it
 * also builds and is tested on the host. Supports `*`, `?`, `[...]` classes (with
 * `a-z` ranges and a leading `!`/`^` negation), and `\` escaping — matching
 * fnmatch with flags 0 for the patterns test-id filtering uses.
 */
#ifndef ASMTEST_GLOB_MATCH_H
#define ASMTEST_GLOB_MATCH_H

/* Returns 1 if `str` matches the glob `pattern`, else 0. */
int asmtest_glob_match(const char *pattern, const char *str);

#endif /* ASMTEST_GLOB_MATCH_H */
