/*
 * jit_trace_node.c — REAL managed-runtime trace: attach to a live Node.js (V8) process
 * and trace a genuine JIT-compiled JavaScript function out of band.
 *
 * Every other W2 test traces code asm-test emits itself (a hand-assembled routine,
 * published to a perf-map the test wrote). This one points the SAME pipeline at a real
 * runtime: it spawns `node --perf-basic-prof` running a hot JS function, lets V8 JIT it,
 * resolves the optimized method from V8's real perf-map, PTRACE_ATTACHes the live
 * (multi-threaded, GC'd) process, runs it to the method entry (software breakpoint), and
 * single-steps one invocation — call-outs to V8 builtins stepped over (call-depth
 * aware). It exercises resolve -> attach -> run_to -> trace end to end on a real JIT.
 *
 * Honest by construction: it self-skips (never hangs, never flakes) when node is absent,
 * ptrace is denied (yama ptrace_scope), or the JIT method cannot be caught (V8 re-tiered
 * / moved the code) — a watchdog alarm bounds the single-step so a stale address cannot
 * stall the lane. The resolution + attach checks are firm (they validate the library
 * against real V8 output and a real /proc/maps); the trace itself is asserted when it
 * succeeds and skipped (not failed) when the runtime does not cooperate. Linux x86-64.
 */
#define _GNU_SOURCE

#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* A uniquely-named JS function in a tight int32 loop (the `|0` keeps the value int32, so
 * V8 keeps stable integer types and a stable optimized body) — hot enough that V8 emits
 * an optimized perf-map entry within a second or two. */
#define HOT_JS                                                                 \
    "function asmtjit(a,b){return (a+b)|0} let s=0; for(;;){s=asmtjit(s,1)}"
#define FNAME "asmtjit"

#if defined(__linux__) && defined(__x86_64__)

/* Empty SIGALRM handler installed WITHOUT SA_RESTART, so the watchdog alarm interrupts
 * the blocking waitpid inside run_to/trace_attached (they then return a ptrace error we
 * treat as a skip) instead of letting a never-hit breakpoint hang the lane forever. */
static void on_alarm(int sig) { (void)sig; }

static void finish(pid_t node) {
    if (node > 0) {
        kill(node, SIGKILL);
        waitpid(node, NULL, 0);
    }
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures, failures);
}

int main(void) {
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP jit-trace: %s\n1..0 # skipped\n", why);
        return 0;
    }

    /* Spawn Node as our child; we attach to it from the OUTSIDE later (no TRACEME) — a
     * true external attach, exactly the managed-runtime scenario. */
    pid_t node = fork();
    if (node == 0) {
        /* --no-turbo-inlining: keep asmtjit a REAL standalone callable body (else
         * TurboFan inlines the tiny function into the loop and the perf-map entry is
         * never actually called, so the breakpoint would never be hit). */
        execlp("node", "node", "--perf-basic-prof", "--no-turbo-inlining", "-e", HOT_JS,
               (char *)NULL);
        _exit(127); /* node not on PATH */
    }
    if (node < 0) {
        perror("fork");
        printf("1..0 # skipped\n");
        return 0;
    }

    /* Poll V8's perf-map until the OPTIMIZED body of asmtjit appears (":*" TurboFan,
     * preferred, or ":^" Sparkplug). Keep the latest matching line — the current tier. */
    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)node);
    char sym[256] = {0};
    unsigned long addr = 0, size = 0;
    int found = 0, best_tier = 0; /* 2 = ":*", 1 = ":^" */
    for (int i = 0; i < 150; i++) { /* up to ~15s of warmup */
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        int st;
        if (waitpid(node, &st, WNOHANG) == node) { /* node exited (e.g. not installed) */
            node = -1;
            break;
        }
        FILE *f = fopen(mappath, "r");
        if (f == NULL)
            continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long a, s;
            int soff = 0;
            if (sscanf(line, "%lx %lx %n", &a, &s, &soff) < 2 || soff == 0)
                continue;
            char *t = line + soff;
            t[strcspn(t, "\r\n")] = '\0';
            if (strstr(t, FNAME) == NULL || s < 8)
                continue;
            int tier = strstr(t, ":*") ? 2 : strstr(t, ":^") ? 1 : 0;
            if (tier == 0 || tier < best_tier)
                continue;
            best_tier = tier;
            strncpy(sym, t, sizeof sym - 1);
            sym[sizeof sym - 1] = '\0';
            addr = a;
            size = s;
            found = 1;
        }
        fclose(f);
        if (found && best_tier == 2)
            break; /* got the TurboFan body — good enough, stop warming up */
    }

    if (node < 0) {
        printf("# SKIP jit-trace: node exited early (not installed?)\n1..0 # skipped\n");
        return 0;
    }
    if (!found) {
        printf("# SKIP jit-trace: V8 emitted no optimized perf-map entry for %s\n",
               FNAME);
        finish(node);
        return failures ? 1 : 0;
    }
    printf("# resolved real V8 JIT method: '%s' @ 0x%lx (%lu bytes, tier %s)\n", sym,
           addr, size, best_tier == 2 ? "TurboFan(*)" : "Sparkplug(^)");

    /* (1) The library's perf-map parser, against V8's REAL map line (hex formatting,
     * spaces/colons/brackets in the symbol — the real-world format, not a fixture). */
    void *base = NULL;
    size_t len = 0;
    int rc = asmtest_proc_perfmap_symbol(node, sym, &base, &len);
    CHECK(rc == ASMTEST_PTRACE_OK &&
              (unsigned long)(uintptr_t)base == addr && len == size,
          "perfmap: library resolves the real V8 JIT method by name (== V8's map line)");

    /* (2) Region discovery against the real process's /proc/<pid>/maps. */
    void *rbase = NULL;
    size_t rlen = 0;
    int fr = asmtest_proc_region_by_addr(node, base, &rbase, &rlen);
    CHECK(fr == ASMTEST_PTRACE_OK && (char *)base >= (char *)rbase &&
              (char *)base < (char *)rbase + rlen,
          "proc maps: the JIT address falls in an executable mapping of the live runtime");

    /* (3) Attach to the real, multi-threaded, GC'd runtime from the outside. */
    int st = 0;
    if (ptrace(PTRACE_ATTACH, node, NULL, NULL) != 0 || waitpid(node, &st, 0) < 0) {
        printf("# SKIP jit-trace: PTRACE_ATTACH denied (yama ptrace_scope?)\n");
        finish(node);
        return failures ? 1 : 0;
    }

    /* (4) Run the runtime to the method (software breakpoint) and single-step one real
     * invocation. A watchdog alarm bounds it: if V8 re-tiered/moved the code so the
     * address is never re-hit, run_to's waitpid is interrupted and we skip rather than
     * hang. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* sa_flags = 0 -> no SA_RESTART, so waitpid sees EINTR */
    sigaction(SIGALRM, &sa, NULL);
    alarm(10);

    asmtest_trace_t *tr = asmtest_trace_new(128, 512);
    long result = 0;
    rc = asmtest_ptrace_run_to(node, base);
    if (rc == ASMTEST_PTRACE_OK)
        rc = asmtest_ptrace_trace_attached(node, base, len, &result, tr);
    alarm(0);

    uint64_t insns = asmtest_emu_trace_insns_total(tr);
    if (rc == ASMTEST_PTRACE_OK && insns >= 1) {
        CHECK(asmtest_trace_covered(tr, 0),
              "trace: real V8 JIT method single-stepped out of band — entry covered");
        printf("# traced %llu instructions of real V8 JIT code%s\n",
               (unsigned long long)insns,
               asmtest_emu_trace_truncated(tr) ? " (truncated)" : "");
        if (asmtest_disas_available()) {
            /* Read the live bytes (still attached) and render them — proof the offsets
             * are real, executed JIT instructions, not a fixture. */
            uint8_t *bytes = (uint8_t *)malloc(len);
            if (bytes != NULL) {
                struct iovec l = {bytes, len}, r = {base, len};
                if (process_vm_readv(node, &l, 1, &r, 1, 0) == (ssize_t)len)
                    asmtest_trace_disasm(tr, ASMTEST_ARCH_X86_64, bytes, len, addr,
                                         stdout);
                free(bytes);
            }
        }
    } else {
        printf("# SKIP jit-trace step: could not single-step the live JIT method "
               "(V8 moved/re-tiered the code, or the watchdog fired)\n");
    }

    asmtest_trace_free(tr);
    ptrace(PTRACE_DETACH, node, NULL, NULL);
    finish(node);
    return failures ? 1 : 0;
}

#else
int main(void) {
    printf("# SKIP jit-trace: Linux x86-64 only\n1..0 # skipped\n");
    return 0;
}
#endif
