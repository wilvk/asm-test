/*
 * debug.c — env-gated debug logging for the AMD/hwtrace trace tier (Phase 4). See
 * src/debug.h for the rationale and the async-signal-safety caveat. Compiled into both
 * the static test objects and libasmtest_hwtrace (wired in mk/native-trace.mk beside
 * msr_lbr.o / ibs_backend.o).
 */
#include "debug.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int asmtest_hwtrace_debug_enabled(void) {
    /* getenv-once: cached on first call. The benign first-call data race (two threads
     * both computing the same value) matches the codeimage.c convention. */
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("ASMTEST_HWTRACE_DEBUG") != NULL ||
                  getenv("ASMTEST_AMD_DEBUG") != NULL)
                     ? 1
                     : 0;
    return cached;
}

void asmtest_hwtrace_debugf(const char *fn, const char *fmt, ...) {
    if (!asmtest_hwtrace_debug_enabled())
        return;
    fprintf(stderr, "[asmtest hwtrace] %s: ", fn != NULL ? fn : "?");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
