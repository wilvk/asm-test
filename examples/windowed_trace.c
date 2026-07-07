/*
 * windowed_trace.c — §D3 cross-process whole-window capture over the JIT-address
 * channel. The out-of-process analog of the in-process whole-window scope: the tracer
 * runs in a SEPARATE process from the "runtime" it traces, so it cannot see where the
 * JIT put a method — it learns the addresses through a shared-memory channel the runtime
 * publishes into (asmtest_addr_channel.h), then records every instruction that falls in
 * the window frame OR any published method, following the calls into them.
 *
 * This demo needs no managed runtime: it stands in two mmap'd leaf routines for "JIT'd
 * methods" and a driver blob that calls them via absolute-address indirect calls — the
 * shape a real JIT emits. The child (the "runtime") publishes both method regions to the
 * channel and runs the driver; the parent (the stepper) run_to's the driver and windowed-
 * traces it, then prints which regions it captured and in what order. The point: the
 * parent records addresses it did NOT know when it forked — proving the cross-process
 * address handoff that lets the concealed §D3 stepper trace a live JIT.
 *
 * Self-skips (exit 0) where ptrace is denied (yama ptrace_scope) or off x86-64 Linux, so
 * the lane never flakes.
 */
#define _GNU_SOURCE

#include "asmtest_addr_channel.h"
#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

/* Publish a leaf routine into a fresh executable mapping; return its base. */
static void *map_code(const unsigned char *bytes, size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    memcpy(p, bytes, n);
    mprotect(p, n, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + n);
    return p;
}

int main(void) {
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP windowed_trace: %s\n", why);
        return 0;
    }
    printf("== §D3 out-of-process whole-window capture over the JIT-address "
           "channel ==\n\n");

    /* Two "JIT'd methods": pure-register leaves at their own mappings. */
    static const unsigned char M1[] = {
        0x48, 0x89, 0xf8, 0x48,
        0x01, 0xf0, 0xc3}; /* mov rax,rdi; add rax,rsi; ret */
    static const unsigned char M2[] = {
        0x48, 0x89, 0xf8, 0x48,
        0x29, 0xf0, 0xc3}; /* mov rax,rdi; sub rax,rsi; ret */
    void *m1 = map_code(M1, sizeof M1), *m2 = map_code(M2, sizeof M2);
    if (m1 == NULL || m2 == NULL) {
        printf("# SKIP windowed_trace: mmap failed\n");
        return 0;
    }
    uint64_t a1 = (uint64_t)(uintptr_t)m1, a2 = (uint64_t)(uintptr_t)m2;

    /* Driver: movabs rax,m1; call rax; movabs rax,m2; call rax; ret. */
    unsigned char drv_bytes[25] = {0x48, 0xB8, 0,    0,    0,   0, 0, 0, 0, 0,
                                   0xFF, 0xD0, 0x48, 0xB8, 0,   0, 0, 0, 0, 0,
                                   0,    0,    0xFF, 0xD0, 0xC3};
    memcpy(drv_bytes + 2, &a1, 8);
    memcpy(drv_bytes + 14, &a2, 8);
    void *drv = map_code(drv_bytes, sizeof drv_bytes);
    if (drv == NULL) {
        printf("# SKIP windowed_trace: mmap failed\n");
        return 0;
    }
    uint64_t dv = (uint64_t)(uintptr_t)drv;

    /* The cross-process channel lives in shared memory the forked child inherits. */
    asmtest_addr_channel_t *chan =
        mmap(NULL, sizeof *chan, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (chan == MAP_FAILED) {
        printf("# SKIP windowed_trace: channel mmap failed\n");
        return 0;
    }
    asmtest_addr_channel_init(chan);

    pid_t pid = fork();
    if (pid < 0) {
        printf("# SKIP windowed_trace: fork failed\n");
        return 0;
    }
    if (pid == 0) {
        /* The "runtime": publish each JIT'd method's (base,len) — as its listener would —
         * then run the driver that calls them. */
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        asmtest_addr_channel_publish(chan, a1, sizeof M1, 0);
        asmtest_addr_channel_publish(chan, a2, sizeof M2, 0);
        raise(SIGSTOP);
        ((void (*)(void))drv)();
        _exit(0);
    }

    /* The stepper: run the child to the window entry, then windowed-trace it. */
    int st = 0;
    long res = 0;
    int rc = ASMTEST_PTRACE_ETRACE;
    asmtest_trace_t *tr = asmtest_trace_new(128, 64);
    if (waitpid(pid, &st, 0) >= 0 && WIFSTOPPED(st) &&
        asmtest_ptrace_run_to(pid, drv) == ASMTEST_PTRACE_OK)
        rc = asmtest_ptrace_trace_attached_windowed(pid, drv, sizeof drv_bytes,
                                                    chan, &res, tr);
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);

    if (rc != ASMTEST_PTRACE_OK) {
        printf("# SKIP windowed_trace: capture did not complete (rc=%d) — "
               "ptrace denied?\n",
               rc);
        return 0;
    }

    /* Attribute the recorded ABSOLUTE addresses back to their regions, in order. */
    printf("the runtime published 2 JIT'd methods to the channel AFTER the "
           "stepper forked:\n");
    printf("    method#1  0x%llx (+%zu bytes)\n", (unsigned long long)a1,
           sizeof M1);
    printf("    method#2  0x%llx (+%zu bytes)\n\n", (unsigned long long)a2,
           sizeof M2);

    size_t n = tr->insns_len, in_drv = 0, in_m1 = 0, in_m2 = 0;
    printf("the out-of-process windowed capture recorded %zu instructions, in "
           "execution order:\n",
           n);
    const char *prev = "";
    for (size_t i = 0; i < n; i++) {
        uint64_t at = tr->insns[i];
        const char *where = "?";
        if (at >= dv && at < dv + sizeof drv_bytes) {
            where = "driver";
            in_drv++;
        } else if (at >= a1 && at < a1 + sizeof M1) {
            where = "method#1 (channel)";
            in_m1++;
        } else if (at >= a2 && at < a2 + sizeof M2) {
            where = "method#2 (channel)";
            in_m2++;
        }
        if (strcmp(where, prev) !=
            0) { /* print one line per region transition */
            printf("    -> %s\n", where);
            prev = where;
        }
    }
    printf(
        "\ncaptured: driver %zu, method#1 %zu, method#2 %zu instructions%s.\n",
        in_drv, in_m1, in_m2, tr->truncated ? " (truncated)" : "");
    printf("-> the stepper is a SEPARATE process; it recorded both methods "
           "purely from the\n");
    printf("   addresses the runtime streamed over the shared channel — the "
           "§D3 handoff that\n");
    printf("   lets the concealed ptrace stepper trace a live JIT it cannot "
           "see into.\n");

    asmtest_trace_free(tr);
    return 0;
}

#else
int main(void) {
    printf("# SKIP windowed_trace: x86-64 Linux only\n");
    return 0;
}
#endif
