/*
 * mach_backend.c — macOS out-of-process single-step native-trace backend (W2, Darwin).
 * See asmtest_mach.h and docs/internal/implementations/macos-oop-mach-stepper.md.
 *
 * XNU cripples BSD ptrace for PC/flag edits (PT_STEP/PT_CONTINUE return ENOTSUP off the
 * `addr == (caddr_t)1` sentinel, by kernel-comment design, to force use of Mach SPIs) —
 * so unlike ptrace_backend.c's Linux tracer, this backend arms EFLAGS.TF through
 * `task_for_pid` + `thread_set_state` and receives each `#DB` as an `EXC_BREAKPOINT`
 * Mach exception message on a dedicated exception port (T2), single-steps through it
 * (T3), and layers trace_call / run_to (T4/T5) on that engine. This translation unit is
 * the T1 skeleton: the platform gate, the five-symbol shape, and one real function
 * (available/skip_reason). The Mach logic itself lands in T2-T5.
 */
#include "asmtest_mach.h"
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__) && defined(__APPLE__)

/* asmtest_disas_available() gates block normalization (Capstone length-decoder), the
 * same dependency ss_backend.c's in-process stepper uses — see asmtest_mach.h. */
int asmtest_mach_available(void) { return asmtest_disas_available() ? 1 : 0; }

void asmtest_mach_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = asmtest_mach_available()
                          ? "available"
                          : "built without Capstone (mach block normalization)";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* TODO(T4): fork-and-trace a self-contained blob through the T3 engine. */
int asmtest_mach_trace_call(const void *code, size_t len, const long *args,
                            int nargs, long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

/* TODO(T3): task_for_pid + thread_get_state/thread_set_state single-step engine. */
int asmtest_mach_trace_attached(pid_t pid, const void *base, size_t len,
                                long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

/* TODO(T5): plant a breakpoint and run the target to it. */
int asmtest_mach_run_to(pid_t pid, const void *addr) {
    (void)pid;
    (void)addr;
    return ASMTEST_MACH_ENOSYS;
}

#else /* not x86-64 Darwin: harmless no-op everywhere else (Linux, Apple Silicon). */

int asmtest_mach_available(void) { return 0; }

void asmtest_mach_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "mach stepper is x86-64 macOS only";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

int asmtest_mach_trace_call(const void *code, size_t len, const long *args,
                            int nargs, long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

int asmtest_mach_trace_attached(pid_t pid, const void *base, size_t len,
                                long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
    (void)result;
    (void)trace;
    return ASMTEST_MACH_ENOSYS;
}

int asmtest_mach_run_to(pid_t pid, const void *addr) {
    (void)pid;
    (void)addr;
    return ASMTEST_MACH_ENOSYS;
}

#endif
