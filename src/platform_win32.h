/* platform_win32.h — internal Win32 primitives for the native Win64 runner port
 * (see docs/plans/win64-native-tier-plan.md, Phase 4). Not part of the public
 * binding ABI; shared between src/platform_win32.c and its tests.
 */
#ifndef ASMTEST_PLATFORM_WIN32_H
#define ASMTEST_PLATFORM_WIN32_H

#if defined(_WIN32)

/* Outcome of running one test body in an isolated child process. A child that
 * dies from an unhandled hardware exception is reported as CRASH (its exit code
 * is the NTSTATUS exception code); one that overruns its deadline is TIMEOUT. */
typedef enum {
    ASMTEST_W32_OK = 0,
    ASMTEST_W32_CRASH = 1,
    ASMTEST_W32_TIMEOUT = 2,
    ASMTEST_W32_SPAWN_FAIL = 3
} asmtest_win32_run_t;

/* Spawn `cmdline` as a child, wait up to `timeout_ms` (0 = wait forever), and
 * classify the result. On a clean exit, *exit_code (if non-NULL) gets the child's
 * exit code. The Win32 analogue of the runner's fork + waitpid + alarm: process
 * isolation gives crash containment, the bounded wait gives the per-test timeout.
 */
asmtest_win32_run_t asmtest_win32_run(const char *cmdline, unsigned timeout_ms,
                                      unsigned long *exit_code);

/* Run `n` test bodies with at most `jobs` in flight at once, each isolated and
 * deadline-bounded as in asmtest_win32_run. results[i]/exit_codes[i] receive the
 * outcome of cmdlines[i] (exit_codes may be NULL). The Win32 analogue of the
 * runner's forked `-jN` pool (poll over children -> WaitForMultipleObjects).
 * Returns 0 on success, -1 if the wait machinery failed. */
int asmtest_win32_run_pool(const char *const *cmdlines, int n, int jobs,
                           unsigned timeout_ms, asmtest_win32_run_t *results,
                           unsigned long *exit_codes);

#endif /* _WIN32 */

#endif /* ASMTEST_PLATFORM_WIN32_H */
