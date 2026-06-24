/* test_isolate_win64.c — Phase 4 slice: isolated execution under Wine.
 *
 * Verifies asmtest_win32_run (src/platform_win32.c): a test body run in a child
 * process is contained — a clean child is OK, a crashing child is reported as
 * CRASH (not propagated to the parent), and a hanging child is killed by the
 * timeout. The Win32 analogue of the POSIX runner's fork + waitpid + alarm.
 *
 * The binary is both parent and child: with an argument it performs that child
 * action; with none it is the parent that spawns itself for each case. Runs as a
 * real PE under Wine.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "platform_win32.h"

/* --- child actions ------------------------------------------------------- */
static int run_child(const char *what) {
    /* Don't let an unhandled fault pop a Windows Error Reporting dialog; we want
     * the child to just die with the exception code so the parent can read it. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    if (strcmp(what, "ok") == 0)
        return 7; /* a distinctive clean exit code */
    if (strcmp(what, "crash") == 0) {
        volatile int *p = NULL;
        *p = 1; /* null write -> access violation */
        return 0;
    }
    if (strcmp(what, "hang") == 0) {
        for (;;)
            Sleep(1000);
    }
    return 0;
}

/* --- parent harness ------------------------------------------------------ */
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

int main(int argc, char **argv) {
    if (argc >= 2)
        return run_child(argv[1]); /* child mode */

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);

    char cmd[MAX_PATH + 32];
    unsigned long code = 0;
    asmtest_win32_run_t r;

    snprintf(cmd, sizeof cmd, "\"%s\" ok", self);
    code = 0;
    r = asmtest_win32_run(cmd, 5000, &code);
    printf("     (ok:    result=%d exit=0x%lx)\n", (int)r, code);
    CHECK(r == ASMTEST_W32_OK && code == 7,
          "isolated clean child: OK, exit code captured");

    snprintf(cmd, sizeof cmd, "\"%s\" crash", self);
    code = 0;
    r = asmtest_win32_run(cmd, 5000, &code);
    printf("     (crash: result=%d exit=0x%lx)\n", (int)r, code);
    CHECK(r == ASMTEST_W32_CRASH,
          "isolated crashing child: contained as CRASH, parent survives");

    snprintf(cmd, sizeof cmd, "\"%s\" hang", self);
    code = 0;
    r = asmtest_win32_run(cmd, 500, &code);
    printf("     (hang:  result=%d exit=0x%lx)\n", (int)r, code);
    CHECK(r == ASMTEST_W32_TIMEOUT,
          "isolated hanging child: killed by the timeout");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
