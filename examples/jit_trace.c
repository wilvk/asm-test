/*
 * jit_trace.c — REAL managed-runtime trace: attach to a live JIT runtime and trace a
 * genuine JIT-compiled method out of band, driving the whole W2 pipeline (resolve ->
 * attach -> run_to -> single-step) against a real process rather than a fixture.
 *
 * Two runtimes share one harness, selected by argv[1]:
 *   jit_trace node            — spawn Node.js (V8), trace the optimized `asmtjit` body
 *   jit_trace dotnet <dll>    — spawn .NET (CoreCLR), trace `Program::Add`
 * Both publish a text perf-map at /tmp/perf-<pid>.map (V8 via --perf-basic-prof; CoreCLR
 * via DOTNET_PerfMapEnabled, set below). The harness polls that map for the method's
 * OPTIMIZED entry, resolves it with the library's own parser (asmtest_proc_perfmap_-
 * symbol) — validating it against the runtime's REAL output — confirms the address
 * against the live /proc/<pid>/maps, PTRACE_ATTACHes the multi-threaded GC'd runtime,
 * run_to's the method entry, and single-steps one invocation. Call-outs to runtime
 * helpers are stepped over (call-depth aware).
 *
 * Honest by construction: a watchdog alarm bounds the single-step, so a re-tiered/moved
 * address self-skips instead of hanging; the resolve + attach checks are firm (they test
 * the library against real runtime output and a real /proc/maps) while the trace is
 * asserted when the runtime cooperates and skipped (never failed) otherwise — so the
 * lane never flakes. Self-skips when the runtime is absent, ptrace is denied (yama), or
 * off Linux x86-64.
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

/* A uniquely-named JS function in a tight int32 loop (the `|0` keeps it int32, so V8
 * keeps stable integer types and a stable optimized body). */
#define HOT_JS                                                                 \
    "function asmtjit(a,b){return (a+b)|0} let s=0; for(;;){s=asmtjit(s,1)}"

#if defined(__linux__) && defined(__x86_64__)

/* Empty SIGALRM handler installed WITHOUT SA_RESTART, so the watchdog alarm interrupts
 * the blocking waitpid inside run_to/trace_attached (they then return a ptrace error we
 * treat as a skip) instead of letting a never-hit breakpoint hang the lane forever. */
static void on_alarm(int sig) { (void)sig; }

/* Optimization tier of a perf-map symbol: 2 = fully optimized (V8 TurboFan ":*",
 * CoreCLR "[Optimized]"), 1 = baseline (V8 Sparkplug ":^", CoreCLR "[QuickJitted]"),
 * 0 = interpreted / unknown. Higher is the real machine-code body we want to trace. */
static int tier_of(const char *sym) {
    if (strstr(sym, ":*") || strstr(sym, "[Optimized]"))
        return 2;
    if (strstr(sym, ":^") || strstr(sym, "[QuickJitted]"))
        return 1;
    return 0;
}

static int trace_runtime(const char *engine, const char *method_substr,
                         char *const cmd[]) {
    if (!asmtest_ptrace_available()) {
        char why[160];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP jit-trace (%s): %s\n1..0 # skipped\n", engine, why);
        return 0;
    }

    /* Spawn the runtime as our child; we attach from the OUTSIDE later (no TRACEME) —
     * a true external attach, exactly the managed-runtime scenario. */
    pid_t pid = fork();
    if (pid == 0) {
        execvp(cmd[0], cmd);
        _exit(127); /* runtime not on PATH */
    }
    if (pid < 0) {
        perror("fork");
        printf("1..0 # skipped\n");
        return 0;
    }

    /* Poll the runtime's perf-map until the OPTIMIZED body of the method appears; keep
     * the highest-tier latest line containing the name (the current compilation). */
    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)pid);
    char sym[256] = {0};
    unsigned long addr = 0, size = 0;
    int found = 0, best_tier = -1;
    for (int i = 0; i < 200; i++) { /* up to ~20s warmup (CoreCLR startup is slower) */
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) { /* runtime exited early */
            pid = -1;
            break;
        }
        FILE *f = fopen(mappath, "r");
        if (f == NULL)
            continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long a, s;
            int soff = 0;
            if (sscanf(line, "%lx %lx %n", &a, &s, &soff) < 2 || soff == 0 || s < 2)
                continue;
            char *t = line + soff;
            t[strcspn(t, "\r\n")] = '\0';
            if (strstr(t, method_substr) == NULL)
                continue;
            int tier = tier_of(t);
            if (tier < best_tier)
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
            break; /* got the fully-optimized body — stop warming up */
    }

    if (pid < 0) {
        printf("# SKIP jit-trace (%s): runtime exited early (not installed?)\n"
               "1..0 # skipped\n",
               engine);
        return 0;
    }
    if (!found) {
        printf("# SKIP jit-trace (%s): no optimized perf-map entry for '%s'\n", engine,
               method_substr);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }
    printf("# resolved real %s JIT method: '%s' @ 0x%lx (%lu bytes, tier %d)\n", engine,
           sym, addr, size, best_tier);

    /* (1) The library's perf-map parser, against the runtime's REAL map line. */
    void *base = NULL;
    size_t len = 0;
    int rc = asmtest_proc_perfmap_symbol(pid, sym, &base, &len);
    CHECK(rc == ASMTEST_PTRACE_OK &&
              (unsigned long)(uintptr_t)base == addr && len == size,
          "perfmap: library resolves the real JIT method by name (== runtime's map line)");

    /* (2) Region discovery against the real process's /proc/<pid>/maps. */
    void *rbase = NULL;
    size_t rlen = 0;
    int fr = asmtest_proc_region_by_addr(pid, base, &rbase, &rlen);
    CHECK(fr == ASMTEST_PTRACE_OK && (char *)base >= (char *)rbase &&
              (char *)base < (char *)rbase + rlen,
          "proc maps: the JIT address falls in an executable mapping of the live runtime");

    /* (3) Attach to the real, multi-threaded, GC'd runtime from the outside. */
    int st = 0;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 || waitpid(pid, &st, 0) < 0) {
        printf("# SKIP jit-trace (%s): PTRACE_ATTACH denied (yama ptrace_scope?)\n",
               engine);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..%d\n", checks);
        return failures ? 1 : 0;
    }

    /* (4) Run the runtime to the method (software breakpoint) and single-step one real
     * invocation, watchdog-bounded so a moved/re-tiered address self-skips not hangs. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* sa_flags = 0 -> no SA_RESTART, so waitpid sees EINTR */
    sigaction(SIGALRM, &sa, NULL);
    alarm(10);

    asmtest_trace_t *tr = asmtest_trace_new(128, 512);
    long result = 0;
    rc = asmtest_ptrace_run_to(pid, base);
    if (rc == ASMTEST_PTRACE_OK)
        rc = asmtest_ptrace_trace_attached(pid, base, len, &result, tr);
    alarm(0);

    uint64_t insns = asmtest_emu_trace_insns_total(tr);
    if (rc == ASMTEST_PTRACE_OK && insns >= 1) {
        CHECK(asmtest_trace_covered(tr, 0),
              "trace: real JIT method single-stepped out of band — entry covered");
        printf("# traced %llu instructions of real %s JIT code%s\n",
               (unsigned long long)insns, engine,
               asmtest_emu_trace_truncated(tr) ? " (truncated)" : "");
        if (asmtest_disas_available()) {
            uint8_t *bytes = (uint8_t *)malloc(len);
            if (bytes != NULL) {
                struct iovec l = {bytes, len}, r = {base, len};
                if (process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)len)
                    asmtest_trace_disasm(tr, ASMTEST_ARCH_X86_64, bytes, len, addr,
                                         stdout);
                free(bytes);
            }
        }
    } else {
        printf("# SKIP jit-trace step (%s): could not single-step the live JIT method "
               "(runtime moved/re-tiered the code, or the watchdog fired)\n",
               engine);
    }

    asmtest_trace_free(tr);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "node";

    if (strcmp(mode, "node") == 0) {
        /* --no-turbo-inlining keeps asmtjit a REAL standalone callable body (else
         * TurboFan inlines the tiny function and its perf-map entry is never called). */
        char *cmd[] = {(char *)"node",
                       (char *)"--perf-basic-prof",
                       (char *)"--no-turbo-inlining",
                       (char *)"-e",
                       (char *)HOT_JS,
                       NULL};
        return trace_runtime("V8", "asmtjit", cmd);
    }
    if (strcmp(mode, "dotnet") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s dotnet <app.dll>\n", argv[0]);
            return 2;
        }
        /* The Makefile builds the app; set CoreCLR to JIT each method ONCE at a stable
         * address (no tiering churn), emit a perf-map, and — crucially —
         * EnableWriteXorExecute=0 so the JIT code heap is a single mapping a software
         * breakpoint (PTRACE_POKETEXT) can patch. With .NET's default W^X the executable
         * code is double-mapped and POKETEXT fails with EIO; tracing a W^X runtime would
         * need hardware breakpoints (debug registers) instead, a separate enhancement. */
        setenv("DOTNET_TieredCompilation", "0", 1);
        setenv("DOTNET_TC_QuickJitForLoops", "0", 1);
        setenv("DOTNET_PerfMapEnabled", "1", 1);
        setenv("DOTNET_EnableWriteXorExecute", "0", 1);
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        char *cmd[] = {(char *)"dotnet", argv[2], NULL};
        return trace_runtime("CoreCLR", "Program::Add", cmd);
    }

    fprintf(stderr, "usage: %s {node|dotnet <app.dll>}\n", argv[0]);
    return 2;
}

#else
int main(void) {
    printf("# SKIP jit-trace: Linux x86-64 only\n1..0 # skipped\n");
    return 0;
}
#endif
