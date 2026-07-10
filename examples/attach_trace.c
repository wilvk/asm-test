/* attach_trace.c — attach to a process asm-test did NOT start, resolve a region
 * inside it, and trace ONE invocation of that region out of process.
 *
 * The library never attaches for you (which PID and when are your calls), so the
 * foreign-attach flow is:
 *     resolve (base,len)  ->  PTRACE_ATTACH + wait  ->  run_to(base)
 *                          ->  trace_attached        ->  PTRACE_DETACH
 *
 * Usage (two ways to say WHAT to trace):
 *     attach_trace <pid> <hex-addr> <len>          # by absolute address + length
 *     attach_trace <pid> --symbol <perf-map-name>  # by JIT method name in /tmp/perf-<pid>.map
 *
 * `make hwtrace-attach-demo` drives it end to end against examples/attach_victim.c.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

/* Copy the region bytes out of the target for a readable disassembly. The tracer
 * does NOT need this to trace (the library reads bytes itself via
 * process_vm_readv) — it is only for the listing at the end. */
static int read_target_bytes(pid_t pid, const void *base, size_t len,
                             uint8_t *out) {
    struct iovec local = {out, len};
    struct iovec remote = {(void *)base, len};
    return process_vm_readv(pid, &local, 1, &remote, 1, 0) == (ssize_t)len ? 0
                                                                           : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <pid> <hex-addr> <len>\n"
                "       %s <pid> --symbol <perf-map-name>\n",
                argv[0], argv[0]);
        return 2;
    }

    /* 0) Can the out-of-process single-step tracer run on this host at all? */
    if (!asmtest_ptrace_available()) {
        char why[256];
        asmtest_ptrace_skip_reason(why, sizeof why);
        fprintf(stderr, "SKIP: ptrace tracer unavailable: %s\n", why);
        return 0; /* self-skip, never a hard failure */
    }

    pid_t pid = (pid_t)strtol(argv[1], NULL, 0);

    /* 1) RESOLVE the (base,len) to trace. Pure /proc reads, no ptrace, so it is
     *    safe before attaching. */
    void *base = NULL;
    size_t len = 0;
    if (strcmp(argv[2], "--symbol") == 0) {
        if (argc < 4) {
            fprintf(stderr, "need a symbol name after --symbol\n");
            return 2;
        }
        int rc = asmtest_proc_perfmap_symbol(pid, argv[3], &base, &len);
        if (rc != ASMTEST_PTRACE_OK) {
            fprintf(stderr, "symbol '%s' not in /tmp/perf-%d.map (rc=%d)\n",
                    argv[3], (int)pid, rc);
            return 1;
        }
    } else {
        base = (void *)(uintptr_t)strtoull(argv[2], NULL, 0);
        len = (size_t)strtoull(argv[3], NULL, 0);
    }
    fprintf(stderr, "tracing [%p, +%zu) in pid %d\n", base, len, (int)pid);

    /* 2) ATTACH. This — not any library call — is what makes it a process you did
     *    not start: become its tracer, then wait for the stop. */
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        perror("PTRACE_ATTACH (needs same-uid + ptrace_scope=0, CAP_SYS_PTRACE, "
               "or the target's PR_SET_PTRACER opt-in)");
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    /* 3) RUN TO the region entry: plant a breakpoint at `base` and let the target
     *    run until IT calls in. Returns with PC exactly at `base`. */
    int rc = asmtest_ptrace_run_to(pid, base);
    if (rc != ASMTEST_PTRACE_OK) {
        fprintf(stderr, "run_to failed (rc=%d): did the target call into it?\n",
                rc);
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return 1;
    }

    /* 4) TRACE one invocation: single-step from the entry until the region
     *    returns; call-outs to helpers are stepped over at native speed. */
    asmtest_trace_t *tr = asmtest_trace_new(4096, 256);
    long result = 0;
    rc = asmtest_ptrace_trace_attached(pid, base, len, &result, tr);

    /* Grab the code bytes for a listing while still stopped, then let go. */
    uint8_t *code = (uint8_t *)malloc(len ? len : 1);
    int have_bytes = code && read_target_bytes(pid, base, len, code) == 0;

    /* 5) DETACH — the target resumes its loop, untouched. */
    ptrace(PTRACE_DETACH, pid, NULL, NULL);

    if (rc != ASMTEST_PTRACE_OK) {
        fprintf(stderr, "trace_attached failed (rc=%d)\n", rc);
        free(code);
        asmtest_trace_free(tr);
        return 1;
    }

    printf("\nreturn value : %ld\n", result);
    printf("instructions : %llu executed, %llu recorded%s\n",
           asmtest_emu_trace_insns_total(tr), asmtest_emu_trace_insns_len(tr),
           asmtest_emu_trace_truncated(tr) ? "  (TRUNCATED)" : "");
    printf("basic blocks : %llu distinct\n\n", asmtest_emu_trace_blocks_len(tr));

    if (have_bytes && asmtest_disas_available()) {
        printf("ordered instruction trace:\n");
        asmtest_trace_disasm(tr, ASMTEST_ARCH_X86_64, code, len,
                             (uint64_t)(uintptr_t)base, stdout);
    } else {
        asmtest_trace_report(tr, stdout); /* bare offsets without Capstone */
    }

    free(code);
    asmtest_trace_free(tr);
    return 0;
}
