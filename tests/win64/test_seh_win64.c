/* test_seh_win64.c — Phase 4 slice: in-process crash-to-failure under Wine.
 *
 * Verifies asmtest_win32_guard (src/platform_win32.c): a hardware fault in the
 * guarded function is caught and turned into a verdict instead of killing the
 * process, and the guard is re-usable afterwards. The Win32 analogue of the
 * runner's sigaction + siglongjmp crash-to-failure for `--no-fork`.
 */
#include <stdio.h>
#include <windows.h>

#include "platform_win32.h"

static void fn_ok(void *arg) { *(int *)arg = 42; }

static void fn_null(void *arg) {
    (void)arg;
    volatile int *p = NULL;
    *p = 1; /* null write -> access violation */
}

static void fn_again(void *arg) { *(int *)arg = 99; }

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

int main(void) {
    asmtest_win32_fault_t f;
    int x;

    x = 0;
    f.code = 0;
    int r = asmtest_win32_guard(fn_ok, &x, &f);
    CHECK(r == 0 && x == 42, "clean function: no fault, ran to completion");

    f.code = 0;
    f.address = NULL;
    r = asmtest_win32_guard(fn_null, NULL, &f);
    printf("     (caught: faulted=%d code=0x%lx addr=%p)\n", r, f.code,
           f.address);
    CHECK(r == 1 && f.code == (unsigned long)EXCEPTION_ACCESS_VIOLATION,
          "null write caught as ACCESS_VIOLATION (process survives)");

    /* The process is still alive and the guard re-arms for the next run. */
    x = 0;
    r = asmtest_win32_guard(fn_again, &x, &f);
    CHECK(r == 0 && x == 99,
          "guard is reusable after a caught fault");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
