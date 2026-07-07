/* platform_win32.h — internal Win32 primitives for the native Win64 runner port
 * (see docs/internal/archive/plans/win64-native-tier-plan.md, Phase 4). Not part of the public
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
    unsigned long code; /* the EXCEPTION_* / NTSTATUS code (e.g. 0xC0000005) */
    void *address; /* the faulting data address (AV) or instruction       */
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
 * watchdog that recovers a *user-mode* hang (e.g. a spin loop). NOTE: the
 * watchdog redirects via SetThreadContext, which only takes effect on return to
 * user mode, so a test blocked in a kernel wait (WaitForSingleObject on a
 * never-signaled handle, Sleep(INFINITE), a deadlocked lock) cannot be recovered
 * in-process; the watchdog then fails hard with ASMTEST_WIN32_HANG_EXIT rather
 * than wedging (finding #36). The default forked mode contains any hang.
 * A crash, a timeout, or an assertion failure
 * (asmtest_fail -> __builtin_longjmp on `asmtest_win32_test_recover`) all unwind
 * to the runner's `__builtin_setjmp` recovery, leaving `asmtest_win32_test_reason`
 * set. `disarm` is called by the assertion path before it longjmps; `end` tears
 * the arming down after the test. */
enum {
    ASMTEST_WIN32_REASON_CRASH = 1000001,
    ASMTEST_WIN32_REASON_TIMEOUT = 1000002
};

/* Process exit code used by the `--no-fork` watchdog when a test is wedged in a
 * kernel wait that SetThreadContext cannot divert (finding #36): the runner fails
 * hard with this distinctive code instead of hanging forever. The default forked
 * mode contains such hangs by terminating the child, so this is a --no-fork-only
 * last resort. */
enum { ASMTEST_WIN32_HANG_EXIT = 124 };

extern void
    *asmtest_win32_test_recover[5]; /* __builtin_setjmp/longjmp buffer */
extern volatile int asmtest_win32_test_reason;
extern asmtest_win32_fault_t asmtest_win32_test_fault;

void asmtest_win32_test_begin(unsigned timeout_ms);
void asmtest_win32_test_disarm(void);
/* Re-arm the crash/timeout facility for a subsequent run_one phase (teardown)
 * after an assertion disarmed it; see the definition (finding #33). */
void asmtest_win32_test_rearm(void);
void asmtest_win32_test_end(void);

/* --- VEH single-step front-end (single-step plan Phase 5; src/ss_win64.c) ---
 * Trace ONE call of `code` — a leaf-oriented Win64-ABI routine occupying
 * [code, code+len) — with up to 4 integer args (RCX/RDX/R8/R9). EFLAGS.TF is
 * armed around a library-owned call; each EXCEPTION_SINGLE_STEP whose RIP falls
 * inside the region appends its offset to offs[0..cap) in execution order
 * (out-of-region steps around the call are filtered, not recorded). *result
 * (if non-NULL) gets RAX at return; *n_out the recorded count; *truncated (if
 * non-NULL) 1 when the stream overflowed cap (the call still completes).
 * Returns 0 on success, -1 on bad args, -2 when a stepper is already active
 * (single-active contract, same as the Linux tier's MVP), -3 when the VEH
 * cannot be installed. Arm and call happen on the calling thread; x86-64 only.
 */
int asmtest_win64_ss_trace_call(const void *code, size_t len,
                                const long long *args, int nargs,
                                long long *result, unsigned long long *offs,
                                unsigned cap, unsigned *n_out, int *truncated);

#endif /* _WIN32 */

#endif /* ASMTEST_PLATFORM_WIN32_H */
