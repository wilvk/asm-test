/* test_veh_scope_win64.c — Phase 4 slice (code-review-plausible-triage T6):
 * the Win32 --no-fork VEH crash handler is scoped to the armed test thread.
 *
 * Verifies rt_veh_cb (src/platform_win32.c): a fault on any thread OTHER
 * than the thread that called asmtest_win32_test_begin must not be
 * redirected through the runner's global recovery buffer -- that buffer's
 * jmp_buf frame lives on the TEST thread's stack, so redirecting a foreign
 * thread there would hijack it onto someone else's stack (two threads
 * sharing one stack -- undefined behavior). Two argv-selected scenarios,
 * because "foreign" is only safely observable from a fresh process: pre-fix
 * it corrupts process state in ways a single binary can't recover from to
 * report a next check.
 *
 *   main     -- a same-thread fault is still caught and reported CRASH
 *               (regression guard: the tid gate must not break same-thread
 *               catches). Exit code gates pass/fail.
 *   foreign  -- a fault on a CreateThread worker must NOT be redirected onto
 *               the main (test) thread. Post-fix, the process dies inside
 *               the Sleep() below via Windows' normal unhandled-exception
 *               path; neither marker below is printed, and the exit code is
 *               nonzero. Pre-fix this was undefined behavior (two threads on
 *               one stack): observed as the MAIN-HIJACKED marker, a hang, or
 *               a corrupt crash after the marker -- mk/win64.mk's recipe
 *               rejects all of those.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "platform_win32.h"

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

static int scenario_main(void) {
    asmtest_win32_test_begin(5000);
    if (__builtin_setjmp(asmtest_win32_test_recover) == 0) {
        volatile int *p = NULL;
        *p = 1; /* null write -> access violation, on THIS (the test) thread */
        printf("FAIL - fault did not occur\n");
        fails++;
    } else {
        CHECK(asmtest_win32_test_reason == ASMTEST_WIN32_REASON_CRASH,
              "same-thread fault still recovered with CRASH reason (tid "
              "gate doesn't break same-thread catches)");
    }
    asmtest_win32_test_end();
    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}

static DWORD WINAPI foreign_fault_thread(LPVOID arg) {
    (void)arg;
    volatile int *p = NULL;
    *p = 1;   /* unguarded null write, on a thread OTHER than the test thread */
    return 0; /* unreachable */
}

static int scenario_foreign(void) {
    /* No timeout: only the VEH fault path is under test here. */
    asmtest_win32_test_begin(0);
    if (__builtin_setjmp(asmtest_win32_test_recover) == 0) {
        HANDLE h = CreateThread(NULL, 0, foreign_fault_thread, NULL, 0, NULL);
        if (h != NULL)
            CloseHandle(h);
        /* Give the worker thread time to fault. Pre-fix: rt_veh_cb redirects
         * the FOREIGN thread onto this thread's recovery buffer, so this
         * Sleep either never returns (the real, wedged main thread) or the
         * process state is already corrupted; post-fix: the worker's fault
         * takes the OS's normal unhandled-exception path and the whole
         * process dies here. */
        Sleep(10000);
        printf("SURVIVED-FOREIGN-FAULT\n");
    } else {
        printf("MAIN-HIJACKED\n");
    }
    asmtest_win32_test_end();
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s {main|foreign}\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "main") == 0)
        return scenario_main();
    if (strcmp(argv[1], "foreign") == 0)
        return scenario_foreign();
    fprintf(stderr, "usage: %s {main|foreign}\n", argv[0]);
    return 2;
}
