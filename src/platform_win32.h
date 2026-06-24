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

/* A hardware fault caught in-process by asmtest_win32_guard. */
typedef struct {
    unsigned long code;  /* the EXCEPTION_* / NTSTATUS code (e.g. 0xC0000005) */
    void *address;       /* the faulting data address (AV) or instruction       */
} asmtest_win32_fault_t;

/* Run fn(arg) with a vectored exception handler installed, recovering from a
 * hardware fault instead of letting it kill the process — the Win32 analogue of
 * the runner's sigaction + siglongjmp crash-to-failure for `--no-fork`. Returns
 * 0 if fn returned normally, or 1 if it faulted (filling *fault if non-NULL).
 *
 * Recovery uses __builtin_setjmp/longjmp (a minimal sp/fp/pc restore, no SEH
 * unwinding) reached via a context-redirect, so it is robust for ordinary
 * compiled frames; recovering through frames with no unwind data is best-effort
 * (the forked path — asmtest_win32_run[_pool] — is the unconditional containment).
 */
int asmtest_win32_guard(void (*fn)(void *), void *arg,
                        asmtest_win32_fault_t *fault);

/* Runner per-test in-process facility (the `--no-fork` execution path). Tests
 * run one at a time, so a single global recovery point suffices. `begin` arms a
 * vectored exception handler (a hardware fault) and, when timeout_ms > 0, a
 * watchdog (a hung test); a crash, a timeout, or an assertion failure
 * (asmtest_fail -> __builtin_longjmp on `asmtest_win32_test_recover`) all unwind
 * to the runner's `__builtin_setjmp` recovery, leaving `asmtest_win32_test_reason`
 * set. `disarm` is called by the assertion path before it longjmps; `end` tears
 * the arming down after the test. */
enum {
    ASMTEST_WIN32_REASON_CRASH = 1000001,
    ASMTEST_WIN32_REASON_TIMEOUT = 1000002
};

extern void *asmtest_win32_test_recover[5]; /* __builtin_setjmp/longjmp buffer */
extern volatile int asmtest_win32_test_reason;
extern asmtest_win32_fault_t asmtest_win32_test_fault;

void asmtest_win32_test_begin(unsigned timeout_ms);
void asmtest_win32_test_disarm(void);
void asmtest_win32_test_end(void);

#endif /* _WIN32 */

#endif /* ASMTEST_PLATFORM_WIN32_H */
