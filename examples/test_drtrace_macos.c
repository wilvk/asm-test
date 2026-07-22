/*
 * test_drtrace_macos.c — the macOS M0 go/no-go harness for the DynamoRIO
 * native-trace tier (macos-dynamorio-port.md T5, runtime built by
 * scripts/build-dynamorio-macos.sh from the pinned source fork).
 *
 * Unlike test_drtrace.c this deliberately does NOT use the generated-bytes
 * path (asmtest_exec_alloc W^X memory): the M0 question is whether a
 * NORMALLY-COMPILED function in the Mach-O __TEXT segment can be traced end
 * to end — attach (dr_app_setup/start), marker resolution on a Mach-O main
 * executable (dr_get_proc_address), coverage through the app-owned trace,
 * and a clean detach (dr_app_stop_and_cleanup). Marker resolution failing
 * is SILENT by design (begin/end become no-ops, nothing crashes), so the
 * asmtest_trace_covered(tr, 0) checks are the detector.
 *
 * Standalone, single-process, run directly (NOT through the forking runner),
 * exactly like test_drtrace.c. Client path from argv[1] or $ASMTEST_DRCLIENT.
 */
#include "asmtest_drtrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (cond) {                                                            \
            printf("ok %d - %s\n", checks, msg);                               \
        } else {                                                               \
            printf("not ok %d - %s\n", checks, msg);                           \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* Compiled straight into __TEXT — no exec_alloc, no W^X, no entitlement.
 * noinline + a volatile sink keep it a real, addressable function. */
__attribute__((noinline)) static long add2(long a, long b) { return a + b; }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF,
            0); /* unbuffered: progress survives a hard kill */
    if (!asmtest_dr_available()) {
        char why[256];
        asmtest_dr_skip_reason(why, sizeof why);
        printf("# SKIP drtrace-macos: %s\n", why);
        printf("1..0 # skipped\n");
        return 0;
    }
    const char *client = (argc > 1) ? argv[1] : getenv("ASMTEST_DRCLIENT");

    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    opts.mode = ASMTEST_DRTRACE_BLOCKS;

    int rc = asmtest_dr_init(&opts);
    if (rc != ASMTEST_DR_OK) {
        printf("# SKIP drtrace-macos: dr_init failed (%d) — is the client path "
               "set?\n",
               rc);
        printf("1..0 # skipped\n");
        return 0;
    }
    CHECK(asmtest_dr_start() == ASMTEST_DR_OK,
          "dr_start takes over in-process (dr_app_setup + dr_app_start)");
    CHECK(asmtest_dr_under_dynamorio() == 1,
          "process reports running under DynamoRIO");

    /* Compiled-function mode: register the __TEXT range of add2 itself. */
    asmtest_trace_t *tr = asmtest_trace_new(64, 64);
    CHECK(tr != NULL, "trace allocated");
    CHECK(asmtest_dr_register_region("add2", (void *)add2, 64, tr) ==
              ASMTEST_DR_OK,
          "register_region records the compiled function's range");

    asmtest_trace_begin("add2");
    volatile long r = add2(20, 22);
    asmtest_trace_end("add2");
    CHECK(r == 42, "traced compiled call returns the right value (20+22)");
    CHECK(asmtest_trace_covered(tr, 0),
          "block offset 0 covered (markers resolved on a Mach-O executable)");

    /* Re-run: coverage accumulates across begin/end pairs. */
    asmtest_trace_begin("add2");
    volatile long r2 = add2(1, 2);
    asmtest_trace_end("add2");
    CHECK(r2 == 3, "second traced call computes correctly (1+2)");
    CHECK(asmtest_trace_covered(tr, 0), "coverage persists across runs");
    CHECK(asmtest_dr_marker_error() == 0, "all begin/end markers balanced");

    /* Symbol mode: the same dr_get_proc_address path, no manual markers. */
    asmtest_trace_t *str = asmtest_trace_new(0, 64);
    CHECK(asmtest_dr_register_symbol("asmtest_symbol_demo", 256, str) ==
              ASMTEST_DR_OK,
          "register_symbol resolves an exported Mach-O function");
    volatile long sr = asmtest_symbol_demo(3, 4); /* no begin/end */
    CHECK(sr == 10, "symbol-mode function computes correctly (3*2+4)");
    CHECK(asmtest_trace_covered(str, 0),
          "symbol mode records coverage with no manual region calls");
    asmtest_dr_unregister_region("asmtest_symbol_demo");
    asmtest_trace_free(str);

    asmtest_dr_unregister_region("add2");
    asmtest_trace_free(tr);
    asmtest_dr_shutdown();
    CHECK(asmtest_dr_under_dynamorio() == 0,
          "dr_app_stop_and_cleanup returns the process to native");

    printf("1..%d\n", checks);
    printf("# %d passed, %d failed\n", checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
