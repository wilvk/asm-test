/*
 * ptrace_backend.c — out-of-process single-step native-trace backend (W2).
 * See asmtest_ptrace.h and docs/plans/zen2-singlestep-trace-plan.md (Phase 5, W2).
 *
 * A tracer PARENT PTRACE_SINGLESTEPs a forked tracee that calls the registered code,
 * reading RIP from the child's register file at each stop. It produces the same
 * exact/complete asmtest_trace_t offsets as the in-process EFLAGS.TF stepper
 * (src/ss_backend.c) — and reuses that backend's single-entry/ends-at-branch block
 * normalization — but collects them entirely out of band, so the tracee's signal
 * disposition and code cache are never touched (the property a JIT/GC managed
 * runtime needs, and the only single-step form available on AArch64).
 *
 * The parent observes every step through ptrace, so no shared memory is needed: it
 * fills the caller-owned trace directly from the register reads.
 */
#define _GNU_SOURCE

#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)

#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/* Ordered in-region RIP-offset capture buffer; overflow is flagged truncated, never
 * emitted as complete. Sized for the small-routine envelope, like ss_backend. */
#ifndef PTRACE_STREAM_CAP
#define PTRACE_STREAM_CAP (1u << 16) /* 65536 offsets */
#endif

int asmtest_ptrace_available(void) { return 1; }

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "available";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* Replay the captured ordered offsets into the trace, deriving blocks from
 * fall-through discontinuities — byte-identical to ss_backend.c's ss_normalize. */
static void normalize(asmtest_trace_t *t, const uint8_t *base, uint64_t base_ip,
                      size_t len, const uint64_t *stream, uint32_t n,
                      int overflow) {
    if (t == NULL)
        return;
    int have_prev = 0;
    uint64_t expected_next = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = stream[i];
        if (!have_prev || off != expected_next)
            trace_append_block(t, off);
        trace_append_insn(t, off);
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, base, len, base_ip, off, NULL,
                                 0);
        if (l == 0) {
            t->truncated = true;
            return;
        }
        expected_next = off + l;
        have_prev = 1;
    }
    if (overflow)
        t->truncated = true;
}

typedef long (*fn6_t)(long, long, long, long, long, long);

int asmtest_ptrace_trace_call(const void *code, size_t len, const long *args,
                              int nargs, long *result, asmtest_trace_t *trace) {
    if (code == NULL || len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return ASMTEST_PTRACE_EINVAL;

    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL)
        return ASMTEST_PTRACE_ETRACE;

    pid_t pid = fork();
    if (pid < 0) {
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }

    if (pid == 0) {
        /* Tracee: request tracing, stop so the parent can attach the stepper, then
         * call the registered code (inherited at the same address via fork) with up
         * to six integer args. Extra register args are ignored by the callee per the
         * SysV ABI. _exit avoids running atexit/stdio in the stepped child. */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)code)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }

    /* Tracer parent. */
    const uint64_t base_ip = (uint64_t)(uintptr_t)code;
    uint32_t n = 0;
    int overflow = 0, entered = 0, returned = 0, rc = ASMTEST_PTRACE_OK;
    int status = 0;

    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        /* Could not reach the initial SIGSTOP. */
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (waitpid(pid, &status, 0) < 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break; /* tracee finished */
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            /* The tracee took a real signal (e.g. a faulting routine). Record what we
             * have as truncated and let it die. */
            if (entered)
                overflow = 1; /* incomplete in-region capture */
            break;
        }

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        uint64_t rip = (uint64_t)regs.rip;

        if (rip >= base_ip && rip < base_ip + len) {
            entered = 1;
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = rip - base_ip;
            else
                overflow = 1;
        } else if (entered && !returned) {
            /* First step out of the region after entering = the routine returned
             * (supported target is a pure-compute routine that does not call out, so
             * this transition is the ret). RAX holds the return value. */
            if (result != NULL)
                *result = (long)regs.rax;
            returned = 1;
            ptrace(PTRACE_CONT, pid, NULL, NULL);
            waitpid(pid, &status, 0);
            break;
        }
    }

    if (rc == ASMTEST_PTRACE_OK)
        normalize(trace, (const uint8_t *)code, base_ip, len, stream, n, overflow);
    else {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    free(stream);
    return rc;
}

#else /* not Linux x86-64 — link-compatible stubs */

int asmtest_ptrace_available(void) { return 0; }

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "out-of-process ptrace stepper is Linux x86-64 only";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

int asmtest_ptrace_trace_call(const void *code, size_t len, const long *args,
                              int nargs, long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

#endif
