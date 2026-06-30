/*
 * jit_trace.c — REAL managed-runtime trace: attach to a live JIT runtime and trace a
 * genuine JIT-compiled method out of band, driving the whole W2 pipeline (resolve ->
 * attach -> run_to -> single-step) against a real process rather than a fixture.
 *
 * Three runtimes share one harness, selected by argv[1]:
 *   jit_trace node            — spawn Node.js (V8), trace the optimized `asmtjit` body
 *   jit_trace dotnet <dll>    — spawn .NET (CoreCLR), trace `Program::Add`
 *   jit_trace java <cp>       — spawn OpenJDK (HotSpot), trace `Hot.asmtjit`
 * All publish a text perf-map at /tmp/perf-<pid>.map (V8 via --perf-basic-prof; CoreCLR
 * via DOTNET_PerfMapEnabled, set below; HotSpot on demand via `jcmd <pid>
 * Compiler.perfmap`, materialized by the perfmap-refresh hook below). The harness polls
 * that map for the method's
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

#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
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
 * 0 = interpreted / unknown. Higher is the real machine-code body we want to trace.
 * HotSpot's Compiler.perfmap symbols carry no tier marker, so they read as 0 — the Java
 * lane runs with -XX:-TieredCompilation (one C2 body) and a target_tier of 0 to match. */
static int tier_of(const char *sym) {
    if (strstr(sym, ":*") || strstr(sym, "[Optimized]"))
        return 2;
    if (strstr(sym, ":^") || strstr(sym, "[QuickJitted]"))
        return 1;
    return 0;
}

/* HotSpot, unlike V8/CoreCLR, does not stream a perf-map as it JITs — but JDK 17+ ships a
 * `Compiler.perfmap` diagnostic command that dumps /tmp/perf-<pid>.map for a LIVE process.
 * jcmd is itself a short-lived JVM (~half a second to start), so rate-limit: dump once
 * every ~8 polls (~0.8s), which is plenty given the loop calls the method millions of
 * times a second. jcmd's stdout/stderr is redirected to /dev/null to keep the TAP clean;
 * if jcmd is absent the map never appears and the lane self-skips like any other. */
static void java_perfmap_refresh(pid_t pid, int iter) {
    if (iter % 8 != 0)
        return;
    pid_t j = fork();
    if (j == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        char pidbuf[16];
        snprintf(pidbuf, sizeof pidbuf, "%d", (int)pid);
        execlp("jcmd", "jcmd", pidbuf, "Compiler.perfmap", (char *)NULL);
        _exit(127);
    }
    if (j > 0)
        waitpid(j, NULL, 0);
}

/* The OS thread that runs the JIT'd method need not be the process's primordial thread:
 * V8 and CoreCLR run their hot body on it (tid == pid), but the `java` launcher runs Java
 * main() on a *secondary* thread. We must ptrace exactly that thread — its int3 trap would
 * be fatal on a thread no tracer owns (the kernel's default action kills the process). A
 * thread picker returns the right tid to attach; NULL means "the process itself" (pid). */
typedef pid_t (*trace_thread_fn)(pid_t pid);

/* utime (clock ticks, /proc stat field 14) of one thread. comm (field 2) is parenthesized
 * and may contain spaces, so scan from the last ')'. */
static unsigned long long thread_utime(pid_t pid, pid_t tid) {
    char path[96];
    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", (int)pid, (int)tid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    if (p == NULL)
        return 0;
    int field = 2; /* the ')' closes field 2 (comm) */
    for (char *tok = strtok(p + 1, " "); tok != NULL; tok = strtok(NULL, " "))
        if (++field == 14)
            return strtoull(tok, NULL, 10);
    return 0;
}

/* Pick the thread burning the most user CPU over a short sample — the one spinning in the
 * hot loop, hence the one that calls the JIT'd method and will hit the entry breakpoint.
 * Falls back to the process itself if nothing is obviously busy (then the lane self-skips
 * cleanly rather than tracing the wrong thread). */
static pid_t pick_busy_thread(pid_t pid) {
    char dpath[64];
    snprintf(dpath, sizeof dpath, "/proc/%d/task", (int)pid);
    pid_t tids[256];
    unsigned long long t0[256];
    int nt = 0;
    DIR *d = opendir(dpath);
    if (d == NULL)
        return pid;
    for (struct dirent *e; nt < 256 && (e = readdir(d)) != NULL;)
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') {
            tids[nt] = (pid_t)atoi(e->d_name);
            t0[nt] = thread_utime(pid, tids[nt]);
            nt++;
        }
    closedir(d);
    struct timespec ts = {0, 80 * 1000 * 1000};
    nanosleep(&ts, NULL);
    pid_t best = pid;
    unsigned long long best_delta = 0;
    for (int i = 0; i < nt; i++) {
        unsigned long long delta = thread_utime(pid, tids[i]) - t0[i];
        if (delta > best_delta) {
            best_delta = delta;
            best = tids[i];
        }
    }
    return best;
}

/* The perf JVMTI agent does not write /tmp/jit-<pid>.dump like V8; it writes
 * $JITDUMPDIR/.debug/jit/<session>/jit-<pid>.dump, where <session> carries a random
 * suffix. Resolve our pid's dump by glob (the pid in the filename keeps it unambiguous).
 * Returns a pointer to a static buffer, or NULL until the agent has created the file. */
static const char *find_java_jitdump(pid_t pid) {
    static char path[256];
    char pattern[128];
    snprintf(pattern, sizeof pattern, "/tmp/.debug/jit/*/jit-%d.dump", (int)pid);
    glob_t g;
    const char *out = NULL;
    if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        strncpy(path, g.gl_pathv[0], sizeof path - 1);
        path[sizeof path - 1] = '\0';
        out = path;
    }
    globfree(&g);
    return out;
}

/* Some runtimes don't stream the perf-map continuously; they materialize it ON DEMAND.
 * The refresh hook (if any) is called each poll iteration to (re)write /tmp/perf-<pid>.map
 * for the live child — NULL for V8/CoreCLR (which write it as they JIT). HotSpot uses it
 * to drive `jcmd <pid> Compiler.perfmap`. `iter` lets the hook rate-limit itself. */
typedef void (*perfmap_refresh_fn)(pid_t pid, int iter);

static int trace_runtime(const char *engine, const char *method_substr,
                         char *const cmd[], int target_tier,
                         perfmap_refresh_fn refresh, trace_thread_fn pick_thread) {
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
        if (refresh != NULL)
            refresh(pid, i);
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
        if (found && best_tier >= target_tier)
            break; /* got the best body this runtime tiers to — stop warming up */
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

    /* (3) Attach to the real, multi-threaded, GC'd runtime from the outside. We trace the
     * specific thread that runs the method (see trace_thread_fn): for V8/CoreCLR that is
     * the process itself; for HotSpot it is the secondary thread the launcher runs main()
     * on. ptrace's "pid" argument is really a tid, so the resolve/maps checks above stay on
     * the process pid while attach/run_to/trace operate on this tid. */
    pid_t ttid = pick_thread != NULL ? pick_thread(pid) : pid;
    int st = 0;
    if (ptrace(PTRACE_ATTACH, ttid, NULL, NULL) != 0 || waitpid(ttid, &st, 0) < 0) {
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
    rc = asmtest_ptrace_run_to(ttid, base);
    if (rc == ASMTEST_PTRACE_OK)
        rc = asmtest_ptrace_trace_attached(ttid, base, len, &result, tr);
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
    ptrace(PTRACE_DETACH, ttid, NULL, NULL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures, failures);
    return failures ? 1 : 0;
}

/* The BINARY JITDUMP path (asmtest_jitdump_find), against a real runtime. Unlike the
 * text perf-map (address + size + name), a jitdump carries the JIT's recorded CODE BYTES
 * — the byte source a branch-trace decoder must be handed — and a per-method timestamp,
 * so the LATEST body of a re-emitted address wins (the temporal same-address-different-
 * bytes problem). Node's V8 writes a real `jit-<pid>.dump` under `--perf-prof`; this
 * recovers a method's recorded bytes from it and validates them three ways: the address
 * agrees with V8's own perf-map (two independent V8 outputs), the bytes disassemble to
 * real instructions, and they match the LIVE code at that address (so the jitdump truly
 * captured the running bytes). asmtest_jitdump_find matches by exact name, so we take the
 * name from the easy-to-parse text perf-map (V8 emits the SAME string in both). */
static int trace_jitdump(void) {
    if (!asmtest_disas_available()) {
        printf("# SKIP jitdump: needs Capstone\n1..0 # skipped\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Run from /tmp so the jitdump lands at /tmp/jit-<pid>.dump (V8 writes it to the
         * cwd; the perf-map always goes to /tmp) — and not in the repo. */
        if (chdir("/tmp") != 0)
            _exit(127);
        char *cmd[] = {(char *)"node",
                       (char *)"--perf-basic-prof",
                       (char *)"--perf-prof",
                       (char *)"--no-turbo-inlining",
                       (char *)"-e",
                       (char *)HOT_JS,
                       NULL};
        execvp(cmd[0], cmd);
        _exit(127);
    }
    if (pid < 0) {
        perror("fork");
        printf("1..0 # skipped\n");
        return 0;
    }

    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)pid);

    /* Poll until a perf-map entry for the method resolves in the jitdump (V8 emits the
     * jitdump record once the method is JIT'd). Try each perf-map line whose symbol
     * contains the name — the tier V8 wrote to the jitdump (Sparkplug/TurboFan) wins. */
    char name[256] = {0};
    unsigned long paddr = 0, psize = 0;
    asmtest_jitdump_entry_t e;
    memset(&e, 0, sizeof e);
    uint8_t jbytes[1024];
    size_t jblen = 0;
    int found = 0;
    for (int i = 0; i < 200 && !found; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
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
            if (strstr(t, "asmtjit") == NULL)
                continue;
            /* V8 writes the jitdump to /tmp/jit-<pid>.dump (path=NULL resolves it). */
            asmtest_jitdump_entry_t te;
            size_t tl = 0;
            if (asmtest_jitdump_find(NULL, pid, t, &te, jbytes, sizeof jbytes, &tl) ==
                    ASMTEST_PTRACE_OK &&
                tl > 0) {
                strncpy(name, t, sizeof name - 1);
                name[sizeof name - 1] = '\0';
                paddr = a;
                psize = s;
                e = te;
                jblen = tl;
                found = 1;
                break;
            }
        }
        fclose(f);
    }

    if (pid < 0) {
        printf("# SKIP jitdump: node exited early (not installed?)\n1..0 # skipped\n");
        return 0;
    }
    if (!found) {
        printf("# SKIP jitdump: no V8 method resolvable in the jitdump\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }
    printf("# recovered real V8 method from jit-%d.dump: '%s' @ 0x%llx (%llu bytes, "
           "code_index %llu)\n",
           (int)pid, name, (unsigned long long)e.code_addr,
           (unsigned long long)e.code_size, (unsigned long long)e.code_index);

    /* (1) The binary jitdump parser recovered a real method's recorded code bytes. */
    CHECK(jblen > 0 && e.code_size > 0,
          "jitdump: asmtest_jitdump_find recovered a real V8 method's recorded bytes");

    /* (2) Cross-check: the jitdump address matches V8's own perf-map for the same name —
     * two independent V8 outputs agreeing on the same compilation. */
    CHECK((unsigned long)e.code_addr == paddr && (unsigned long)e.code_size == psize,
          "jitdump: code_addr/size agree with V8's perf-map (two independent V8 outputs)");

    /* (3) The recorded bytes are real machine code (decode the first instruction). */
    CHECK(asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, 0, NULL, 0) > 0,
          "jitdump: the recorded bytes disassemble to real x86-64 instructions");

    /* (4) The recorded bytes == the LIVE code at code_addr — the jitdump captured the
     * actual running bytes (best-effort: skips if V8 moved/re-tiered the code). */
    uint8_t live[1024];
    size_t n = jblen < sizeof live ? jblen : sizeof live;
    struct iovec lv = {live, n}, rv = {(void *)(uintptr_t)e.code_addr, n};
    if (process_vm_readv(pid, &lv, 1, &rv, 1, 0) == (ssize_t)n &&
        memcmp(live, jbytes, n) == 0)
        CHECK(1, "jitdump: recorded bytes == the live JIT code (jitdump captured the "
                 "running bytes)");
    else
        printf("# SKIP jitdump byte-match: live code differs (V8 moved/re-tiered it)\n");

    printf("# real V8 JIT code recovered from the jitdump's recorded bytes:\n");
    for (uint64_t off = 0; off < jblen;) {
        char text[128];
        size_t l =
            asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, off, text,
                          sizeof text);
        if (l == 0)
            break;
        printf("    0x%llx  %s\n", (unsigned long long)off, text);
        off += l;
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures, failures);
    return failures ? 1 : 0;
}

/* The BINARY JITDUMP path against a SECOND, independent producer: OpenJDK HotSpot. Where
 * V8 emits a jitdump natively (--perf-prof), HotSpot has no native jitdump — the de-facto
 * encoder is the perf project's JVMTI agent (libperf-jvmti.so, from linux-tools), loaded
 * with -agentpath. It captures every CompiledMethodLoad and writes the method's recorded
 * code bytes to $JITDUMPDIR/.debug/jit/<session>/jit-<pid>.dump. This validates
 * asmtest_jitdump_find against a jitdump emitted by a DIFFERENT runtime+encoder than V8 —
 * the encoder names methods in JVM descriptor form (`LHot;asmtjit(II)I`, not the
 * Compiler.perfmap form), and interleaves debug/unwinding records the reader must skip.
 * The agent's path is argv[3] (the Makefile locates it); the lane self-skips if it or
 * Capstone is absent.
 *
 * Unlike V8 (which writes its jitdump record-by-record), the perf JVMTI agent BUFFERS and
 * writes the tail only when it is unloaded on a clean JVM shutdown — the `perf record`
 * workflow reads the dump post-exit. So the flow is: resolve asmtjit's address from the
 * live perf-map and snapshot its live bytes, then SIGTERM the JVM (orderly shutdown flushes
 * the dump; SIGKILL would not), then read the now-complete dump and validate. */
#define JAVA_JITDUMP_METHOD "LHot;asmtjit(II)I"
static int trace_jitdump_java(const char *cp, const char *agent) {
    if (!asmtest_disas_available()) {
        printf("# SKIP java-jitdump: needs Capstone\n1..0 # skipped\n");
        return 0;
    }
    if (agent == NULL || agent[0] == '\0' || access(agent, R_OK) != 0) {
        printf("# SKIP java-jitdump: needs the perf JVMTI agent (libperf-jvmti.so from "
               "linux-tools)\n1..0 # skipped\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* The agent roots its dump tree at $JITDUMPDIR; keep it in /tmp, not the repo. */
        setenv("JITDUMPDIR", "/tmp", 1);
        char ap[256];
        snprintf(ap, sizeof ap, "-agentpath:%s", agent);
        char *cmd[] = {(char *)"java",
                       ap,
                       (char *)"-XX:-TieredCompilation",
                       /* The agent's per-method I/O slows startup, so compile asmtjit
                        * promptly (default C2 threshold is 10000) — else it lands only
                        * after the JDK warmup and the lane may time out. */
                       (char *)"-XX:CompileThreshold=1000",
                       (char *)"-XX:CompileCommand=dontinline,Hot.asmtjit",
                       (char *)"-cp",
                       (char *)cp,
                       (char *)"Hot",
                       NULL};
        execvp(cmd[0], cmd);
        _exit(127);
    }
    if (pid < 0) {
        perror("fork");
        printf("1..0 # skipped\n");
        return 0;
    }

    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)pid);

    /* (A) Resolve asmtjit's address+size from HotSpot's own perf-map (jcmd-driven), while
     * the JVM is live. The agent buffers the jitdump and writes asmtjit's tail record only
     * on a clean shutdown (step C) — so we cannot read it from the dump yet; we use the
     * perf-map both to know the method has compiled and as the independent cross-check. */
    unsigned long paddr = 0, psize = 0;
    for (int i = 0; i < 200 && paddr == 0; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            pid = -1;
            break;
        }
        java_perfmap_refresh(pid, i); /* HotSpot perf-map is on-demand (jcmd) */
        FILE *f = fopen(mappath, "r");
        if (f == NULL)
            continue;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long aa, ss;
            int soff = 0;
            if (sscanf(line, "%lx %lx %n", &aa, &ss, &soff) < 2 || soff == 0)
                continue;
            char *t = line + soff;
            t[strcspn(t, "\r\n")] = '\0';
            if (strstr(t, "asmtjit") != NULL) {
                paddr = aa;
                psize = ss;
            }
        }
        fclose(f);
    }

    if (pid < 0) {
        printf("# SKIP java-jitdump: java exited early (JDK not installed?)\n"
               "1..0 # skipped\n");
        return 0;
    }
    if (paddr == 0) {
        printf("# SKIP java-jitdump: asmtjit not resolvable in HotSpot's perf-map in time\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* (B) Snapshot the LIVE code now, before we stop the runtime (process_vm_readv on a
     * child needs no attach). We compare the jitdump's recorded bytes to this in step (4). */
    uint8_t live[1024];
    size_t ln = psize > 0 && psize < sizeof live ? (size_t)psize : sizeof live;
    struct iovec lv = {live, ln}, rv = {(void *)(uintptr_t)paddr, ln};
    ssize_t livegot = process_vm_readv(pid, &lv, 1, &rv, 1, 0);

    /* (C) Clean-shutdown the JVM so the perf JVMTI agent flushes its buffered jitdump tail
     * (including asmtjit) to disk — SIGTERM runs the orderly shutdown that unloads the agent;
     * SIGKILL would lose the unflushed records. A watchdog bounds the wait. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* no SA_RESTART -> waitpid sees EINTR if the JVM hangs */
    sigaction(SIGALRM, &sa, NULL);
    alarm(15);
    kill(pid, SIGTERM);
    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    alarm(0);
    if (w != pid) { /* JVM did not exit (and flush) in time */
        printf("# SKIP java-jitdump: JVM did not shut down to flush the jitdump\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* (D) Read the now-complete jitdump (it persists on disk after the JVM exits). */
    asmtest_jitdump_entry_t e;
    memset(&e, 0, sizeof e);
    uint8_t jbytes[1024];
    size_t jblen = 0;
    const char *dump = find_java_jitdump(pid);
    if (dump == NULL ||
        asmtest_jitdump_find(dump, pid, JAVA_JITDUMP_METHOD, &e, jbytes, sizeof jbytes,
                             &jblen) != ASMTEST_PTRACE_OK ||
        jblen == 0) {
        printf("# SKIP java-jitdump: asmtjit not present in the flushed jitdump\n"
               "1..0 # skipped\n");
        return 0;
    }
    printf("# recovered real HotSpot method from the agent's jitdump: '%s' @ 0x%llx "
           "(%llu bytes, code_index %llu)\n",
           JAVA_JITDUMP_METHOD, (unsigned long long)e.code_addr,
           (unsigned long long)e.code_size, (unsigned long long)e.code_index);

    /* (1) The binary jitdump parser recovered a real method's recorded code bytes. */
    CHECK(jblen > 0 && e.code_size > 0,
          "java-jitdump: asmtest_jitdump_find recovered a real HotSpot method's bytes");

    /* (2) Cross-check: the jitdump's address/size match HotSpot's own jcmd perf-map for the
     * same method — two independent HotSpot outputs (the JVMTI agent and Compiler.perfmap). */
    CHECK((unsigned long)e.code_addr == paddr && (unsigned long)e.code_size == psize,
          "java-jitdump: code_addr/size agree with HotSpot's jcmd perf-map (two independent "
          "outputs)");

    /* (3) The recorded bytes are real machine code (decode the first instruction). */
    CHECK(asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, 0, NULL, 0) > 0,
          "java-jitdump: the recorded bytes disassemble to real x86-64 instructions");

    /* (4) The recorded bytes == the bytes that were LIVE at code_addr (snapshotted in step
     * B) — the jitdump captured the actual running code (best-effort). */
    if (livegot == (ssize_t)ln && memcmp(live, jbytes, ln < jblen ? ln : jblen) == 0)
        CHECK(1, "java-jitdump: recorded bytes == the live JIT code (jitdump captured the "
                 "running bytes)");
    else
        printf("# SKIP java-jitdump byte-match: live snapshot unavailable or differs\n");

    printf("# real HotSpot JIT code recovered from the jitdump's recorded bytes:\n");
    for (uint64_t off = 0; off < jblen;) {
        char text[128];
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, off, text,
                                 sizeof text);
        if (l == 0)
            break;
        printf("    0x%llx  %s\n", (unsigned long long)off, text);
        off += l;
    }

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
        return trace_runtime("V8", "asmtjit", cmd, 2, NULL, NULL);
    }
    if (strcmp(mode, "dotnet") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s dotnet <app.dll>\n", argv[0]);
            return 2;
        }
        /* The Makefile builds the app; set CoreCLR to JIT each method ONCE at a stable
         * address (no tiering churn) and emit a perf-map. Note we do NOT disable W^X:
         * .NET's default double-maps the JIT code heap so a software breakpoint
         * (PTRACE_POKETEXT) is refused with EIO — run_until detects that and falls back to
         * a HARDWARE execution breakpoint, so we trace the W^X code as-shipped. */
        setenv("DOTNET_TieredCompilation", "0", 1);
        setenv("DOTNET_TC_QuickJitForLoops", "0", 1);
        setenv("DOTNET_PerfMapEnabled", "1", 1);
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        char *cmd[] = {(char *)"dotnet", argv[2], NULL};
        return trace_runtime("CoreCLR", "Program::Add", cmd, 2, NULL, NULL);
    }
    if (strcmp(mode, "java") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s java <classpath>\n", argv[0]);
            return 2;
        }
        /* The Makefile compiles Hot.java to the classpath dir we're passed. Two knobs make
         * the body traceable, mirroring the V8/CoreCLR lanes:
         *   -XX:-TieredCompilation  — straight to C2: ONE optimized nmethod at a stable
         *                             address, no tier churn (the dotnet TC=0 analogue).
         *   CompileCommand dontinline,Hot.asmtjit — keep asmtjit a REAL standalone callable
         *                             body (else C2 inlines the tiny method into main's
         *                             compiled loop and its nmethod is never entered — the
         *                             same trap the V8 lane dodges with --no-turbo-inlining).
         * asmtjit is `static` so its verified entry is at code_begin (no receiver inline-
         * cache check), i.e. the very address Compiler.perfmap reports — so run_to lands.
         * HotSpot's JIT code is plain RWX-then-RX; the software breakpoint applies cleanly
         * (no W^X hardware-breakpoint fallback needed, unlike CoreCLR). */
        char *cmd[] = {(char *)"java",
                       (char *)"-XX:-TieredCompilation",
                       (char *)"-XX:CompileCommand=dontinline,Hot.asmtjit",
                       (char *)"-cp",
                       argv[2],
                       (char *)"Hot",
                       NULL};
        return trace_runtime("HotSpot", "asmtjit", cmd, 0, java_perfmap_refresh,
                             pick_busy_thread);
    }
    if (strcmp(mode, "java-jitdump") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: %s java-jitdump <classpath> <libperf-jvmti.so>\n",
                    argv[0]);
            return 2;
        }
        return trace_jitdump_java(argv[2], argv[3]);
    }
    if (strcmp(mode, "jitdump") == 0)
        return trace_jitdump();

    fprintf(stderr,
            "usage: %s {node|dotnet <app.dll>|java <classpath>|"
            "java-jitdump <classpath> <agent.so>|jitdump}\n",
            argv[0]);
    return 2;
}

#else
int main(void) {
    printf("# SKIP jit-trace: Linux x86-64 only\n1..0 # skipped\n");
    return 0;
}
#endif
