/* test_pool_win64.c — Phase 4 slice: the parallel -jN pool under Wine.
 *
 * Verifies asmtest_win32_run_pool (src/platform_win32.c): a batch of test bodies
 * run with a bounded number in flight, each isolated and deadline-bounded. The
 * Win32 analogue of the runner's forked `-jN` pool. The binary is both parent and
 * child (child action selected by argv). Runs as a real PE under Wine.
 */
#include <stdio.h>
#include <windows.h>

#include "child_actions.h"
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

int main(int argc, char **argv) {
    if (argc >= 2)
        return asmtest_win64_run_child(argv[1]); /* child mode */

    char self[MAX_PATH];
    GetModuleFileNameA(NULL, self, sizeof self);

    /* A mix of clean, crashing, and hanging tasks, run two at a time. */
    const char *acts[] = {"ok", "ok", "crash", "ok", "hang", "ok"};
    enum { N = 6 };
    char cmds[N][MAX_PATH + 32];
    const char *cmdptrs[N];
    for (int i = 0; i < N; i++) {
        snprintf(cmds[i], sizeof cmds[i], "\"%s\" %s", self, acts[i]);
        cmdptrs[i] = cmds[i];
    }

    asmtest_win32_run_t results[N];
    unsigned long codes[N];
    int rc = asmtest_win32_run_pool(cmdptrs, N, 2, 800, results, codes);

    for (int i = 0; i < N; i++)
        printf("     (task %d %-6s result=%d exit=0x%lx)\n", i, acts[i],
               (int)results[i], codes[i]);

    CHECK(rc == 0, "pool runs every task to completion at -j2");
    CHECK(results[0] == ASMTEST_W32_OK && codes[0] == 7 &&
              results[1] == ASMTEST_W32_OK && results[3] == ASMTEST_W32_OK &&
              results[5] == ASMTEST_W32_OK,
          "clean tasks report OK with the right exit code");
    CHECK(results[2] == ASMTEST_W32_CRASH,
          "crashing task is contained as CRASH (pool keeps going)");
    CHECK(results[4] == ASMTEST_W32_TIMEOUT,
          "hanging task is killed by its per-task timeout");

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails,
           fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
