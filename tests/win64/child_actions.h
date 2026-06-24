/* child_actions.h — shared child-process bodies for the Win64 runner-port tests
 * (isolation + pool). The test binaries are both parent and child; in child mode
 * they perform one of these actions, selected by argv.
 */
#ifndef ASMTEST_WIN64_CHILD_ACTIONS_H
#define ASMTEST_WIN64_CHILD_ACTIONS_H

#include <string.h>
#include <windows.h>

/* On an unhandled exception, exit immediately with the exception code rather
 * than letting Wine start its (slow) crash debugger — so the parent reads a
 * prompt NTSTATUS exit code (e.g. 0xC0000005) instead of timing the child out. */
static LONG WINAPI asmtest_win64_fast_die(EXCEPTION_POINTERS *ep) {
    ExitProcess(ep->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER; /* unreached */
}

static int asmtest_win64_run_child(const char *what) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter(asmtest_win64_fast_die);

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

#endif /* ASMTEST_WIN64_CHILD_ACTIONS_H */
