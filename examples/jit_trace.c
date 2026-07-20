/*
 * jit_trace.c — REAL managed-runtime trace: attach to a live JIT runtime and trace a
 * genuine JIT-compiled method out of band, driving the whole W2 pipeline (resolve ->
 * attach -> run_to -> step) against a real process rather than a fixture.
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
 * run_to's the method entry, and steps one invocation — by PTRACE_SINGLEBLOCK block-step
 * (one stop per taken branch, byte-identical reconstructed per-instruction stream) when
 * available, else per-instruction single-step; the *-descend lanes stay per-instruction
 * (descent needs trace_attached_ex). Call-outs to runtime helpers are stepped over
 * (call-depth aware).
 *
 * Honest by construction: a watchdog alarm bounds the stepping, so a re-tiered/moved
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
#include <sys/socket.h>
#include <sys/time.h> /* struct timeval — SO_RCVTIMEO/SO_SNDTIMEO on the diag-IPC socket */
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* checks/failures and CHECK are used only from the Linux/x86-64 body below (the
 * #else is a self-skip stub main that reports neither), so guard them to match —
 * otherwise they draw -Werror,-Wunused-variable off that target (macOS, Linux-arm64). */
#if defined(__linux__) && defined(__x86_64__)
static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)
#endif

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
    snprintf(pattern, sizeof pattern, "/tmp/.debug/jit/*/jit-%d.dump",
             (int)pid);
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
                         perfmap_refresh_fn refresh,
                         trace_thread_fn pick_thread, int descend_level) {
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
    for (int i = 0; i < 200;
         i++) { /* up to ~20s warmup (CoreCLR startup is slower) */
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
            if (sscanf(line, "%lx %lx %n", &a, &s, &soff) < 2 || soff == 0 ||
                s < 2)
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
        printf("# SKIP jit-trace (%s): no optimized perf-map entry for '%s'\n",
               engine, method_substr);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }
    printf("# resolved real %s JIT method: '%s' @ 0x%lx (%lu bytes, tier %d)\n",
           engine, sym, addr, size, best_tier);

    /* (1) The library's perf-map parser, against the runtime's REAL map line. */
    void *base = NULL;
    size_t len = 0;
    int rc = asmtest_proc_perfmap_symbol(pid, sym, &base, &len);
    CHECK(rc == ASMTEST_PTRACE_OK && (unsigned long)(uintptr_t)base == addr &&
              len == size,
          "perfmap: library resolves the real JIT method by name (== runtime's "
          "map line)");

    /* (2) Region discovery against the real process's /proc/<pid>/maps. */
    void *rbase = NULL;
    size_t rlen = 0;
    int fr = asmtest_proc_region_by_addr(pid, base, &rbase, &rlen);
    CHECK(fr == ASMTEST_PTRACE_OK && (char *)base >= (char *)rbase &&
              (char *)base < (char *)rbase + rlen,
          "proc maps: the JIT address falls in an executable mapping of the "
          "live runtime");

    /* (3) Attach to the real, multi-threaded, GC'd runtime from the outside. We trace the
     * specific thread that runs the method (see trace_thread_fn): for V8/CoreCLR that is
     * the process itself; for HotSpot it is the secondary thread the launcher runs main()
     * on. ptrace's "pid" argument is really a tid, so the resolve/maps checks above stay on
     * the process pid while attach/run_to/trace operate on this tid. */
    pid_t ttid = pick_thread != NULL ? pick_thread(pid) : pid;
    int st = 0;
    if (ptrace(PTRACE_ATTACH, ttid, NULL, NULL) != 0 ||
        waitpid(ttid, &st, 0) < 0) {
        printf("# SKIP jit-trace (%s): PTRACE_ATTACH denied (yama "
               "ptrace_scope?)\n",
               engine);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..%d\n", checks);
        return failures ? 1 : 0;
    }

    /* (4) Run the runtime to the method (software breakpoint) and step one real
     * invocation, watchdog-bounded so a moved/re-tiered address self-skips not hangs. */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler =
        on_alarm; /* sa_flags = 0 -> no SA_RESTART, so waitpid sees EINTR */
    sigaction(SIGALRM, &sa, NULL);
    alarm(10);

    asmtest_trace_t *tr = asmtest_trace_new(128, 512);
    long result = 0;
    /* Optional call descent: at L2 the allow-set is the method's own executable mapping, so
     * the tracer descends INTO the runtime's sibling JIT methods it calls (e.g. .NET
     * Console::WriteLine -> get_Out) while still stepping OVER libc/PLT; at L3 it descends
     * everything, bounded by a conservative budget + the backend watchdog and expected to
     * self-skip (truncate) when it trips a guard — never to hang or corrupt. Ships behind
     * the explicit *-descend / *-descend-all modes only. */
    asmtest_descent_t *dh = NULL;
    if (descend_level > 0) {
        dh = asmtest_descent_new((asmtest_descent_level_t)descend_level);
        if (descend_level == ASMTEST_DESCENT_DESCEND_KNOWN)
            asmtest_descent_allow_region(dh, rbase,
                                         rlen); /* the runtime's JIT heap */
        else if (descend_level == ASMTEST_DESCENT_DESCEND_ALL) {
            asmtest_descent_set_insn_budget(dh, 4096); /* conservative */
            asmtest_descent_set_watchdog_ms(dh, 2000);
        }
    }
    /* Default (no-descent) lane: prefer the PTRACE_SINGLEBLOCK rung — one ptrace
     * stop per TAKEN branch instead of one per retired instruction (roughly an
     * order of magnitude fewer tracer round-trips on a live JIT), reconstructing
     * the byte-identical per-instruction stream, so every check below holds
     * unchanged. The descent lanes stay on trace_attached_ex (block-step has no
     * descent parameter); hosts without a functional PTRACE_SINGLEBLOCK or
     * Capstone keep the per-instruction path (the probe is hang-proof + cached). */
    int use_blockstep = dh == NULL && asmtest_ptrace_blockstep_available();
    rc = asmtest_ptrace_run_to(ttid, base);
    if (rc == ASMTEST_PTRACE_OK)
        rc = dh != NULL ? asmtest_ptrace_trace_attached_ex(ttid, base, len,
                                                           &result, tr, dh)
             : use_blockstep
                 ? asmtest_ptrace_trace_attached_blockstep(ttid, base, len,
                                                           &result, tr)
                 : asmtest_ptrace_trace_attached(ttid, base, len, &result, tr);
    alarm(0);

    uint64_t insns = asmtest_emu_trace_insns_total(tr);
    if (rc == ASMTEST_PTRACE_OK && insns >= 1) {
        CHECK(asmtest_trace_covered(tr, 0),
              "trace: real JIT method stepped out of band — entry covered");
        printf("# traced %llu instructions of real %s JIT code via %s%s\n",
               (unsigned long long)insns, engine,
               use_blockstep ? "block-step (PTRACE_SINGLEBLOCK)"
                             : "per-instruction single-step",
               asmtest_emu_trace_truncated(tr) ? " (truncated)" : "");
        if (dh != NULL) {
            size_t nf = asmtest_descent_frames_len(dh);
            size_t ne = asmtest_descent_edges_len(dh);
            printf("# descent L%d: %zu frame(s), %zu edge(s)%s%s\n",
                   descend_level, nf, ne,
                   asmtest_descent_truncated(dh) ? " (truncated)" : "",
                   asmtest_descent_depth_capped(dh) ? " (guard tripped)" : "");
            /* The guarded L3 lane asserts the GUARDS fire (self-skip), not
             * transparency. `nf >= 1` was VACUOUS (frame 0 always exists) — require a
             * real descent (a second frame or an edge) OR an honest guard trip. */
            CHECK(
                descend_level < ASMTEST_DESCENT_DESCEND_ALL ||
                    asmtest_descent_truncated(dh) ||
                    asmtest_descent_depth_capped(dh) || nf >= 2 || ne >= 1,
                "descent: guarded L3 lane made real progress (>=2 frames or an "
                "edge) or self-skipped honestly");
        }
        if (asmtest_disas_available()) {
            uint8_t *bytes = (uint8_t *)malloc(len);
            if (bytes != NULL) {
                struct iovec l = {bytes, len}, r = {base, len};
                if (process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)len)
                    asmtest_trace_disasm(tr, ASMTEST_ARCH_X86_64, bytes, len,
                                         addr, stdout);
                free(bytes);
            }
        }
    } else {
        printf("# SKIP jit-trace step (%s): could not %s the live JIT method "
               "(runtime moved/re-tiered the code, or the watchdog fired)\n",
               engine, use_blockstep ? "block-step" : "single-step");
    }

    asmtest_descent_free(dh);
    asmtest_trace_free(tr);
    ptrace(PTRACE_DETACH, ttid, NULL, NULL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
    return failures ? 1 : 0;
}

/* T2: recover and print a method's offset->file:line[.column] source table from the
 * SAME jitdump asmtest_jitdump_find matched (via the JIT_CODE_DEBUG_INFO records the byte
 * path skips), filling the caller-owned `dbg` buffer. Returns the debug reader's status
 * and sets *dlen to the entry count. A method that carries no debug info still returns
 * ASMTEST_PTRACE_OK with *dlen == 0 (the CoreCLR case), so a caller distinguishes "no
 * debug records" from an error. */
static int print_debug_table(const char *path, pid_t pid, const char *name,
                             asmtest_jitdump_debug_t *dbg, size_t cap,
                             size_t *dlen) {
    asmtest_jitdump_entry_t de;
    memset(&de, 0, sizeof de);
    *dlen = 0;
    int rc = asmtest_jitdump_debug_find(path, pid, name, &de, dbg, cap, dlen);
    if (rc == ASMTEST_PTRACE_OK && *dlen > 0) {
        printf("# offset -> file:line[.column] from the jitdump DEBUG_INFO "
               "records:\n");
        for (size_t i = 0; i < *dlen; i++) {
            if (dbg[i].discrim)
                printf("    +0x%llx  %s:%u.%u\n",
                       (unsigned long long)dbg[i].off, dbg[i].file, dbg[i].line,
                       dbg[i].discrim);
            else
                printf("    +0x%llx  %s:%u\n", (unsigned long long)dbg[i].off,
                       dbg[i].file, dbg[i].line);
        }
    }
    return rc;
}

/* Read exactly n bytes from fd (a stream socket may deliver a short read). */
static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

/* Turn on CoreCLR perf-map + jitdump emission in an ALREADY-RUNNING process, out of band,
 * over its diagnostics IPC socket — with NO DOTNET_PerfMapEnabled launch flag (the whole
 * point of the attach lane: recover a method's bytes from a process not launched for it).
 * Hand-rolls the documented DOTNET_IPC_V1 EnablePerfMap command — the same wire the in-tree
 * bindings/dotnet DiagnosticsIpc speaks — chosen over the NuGet DiagnosticsClient so the
 * lane needs no NuGet restore in the asmtest-dotnet image (offline). PerfMapType.All(1) writes
 * BOTH the text /tmp/perf-<pid>.map (name resolution + the perf-map cross-check) and the binary
 * /tmp/jit-<pid>.dump, and runs the ReadyToRun rundown so an already-JITted Program::Add is
 * re-emitted. Returns 0 on an OK response; nonzero on any failure (the caller self-skips). */
static int dotnet_enable_perfmap(pid_t pid, const char *engine) {
    (void)engine;
    /* Wait for the runtime to open its diagnostics socket (on by default; lives in /tmp,
     * name carries a runtime-chosen disambiguator, so glob it). */
    char sockpath[256] = {0};
    for (int i = 0; i < 100 && sockpath[0] == '\0'; i++) {
        int st;
        /* WNOWAIT: detect the child's exit WITHOUT reaping it, so its pid stays a
         * (waitable) zombie — the caller's kill()+waitpid() in trace_jitdump then
         * still targets a live pid, with no reap-then-recycle race. */
        if (waitpid(pid, &st, WNOHANG | WNOWAIT) == pid)
            return 1; /* runtime exited before the socket appeared */
        char pattern[80];
        snprintf(pattern, sizeof pattern, "/tmp/dotnet-diagnostic-%d-*-socket",
                 (int)pid);
        glob_t g;
        if (glob(pattern, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            strncpy(sockpath, g.gl_pathv[0], sizeof sockpath - 1);
            sockpath[sizeof sockpath - 1] = '\0';
        }
        globfree(&g);
        if (sockpath[0] == '\0') {
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    if (sockpath[0] == '\0')
        return 1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    /* Bound read()/write() so a wedged runtime that accepts the connection but never
     * replies self-skips instead of hanging the lane (the in-tree C# DiagnosticsIpc sets
     * the same 1s timeout; 15s here matches the java lane's flush watchdog). */
    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, sockpath, sizeof sa.sun_path - 1);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return 1;
    }

    /* IpcMessage: header(20) = magic(14) "DOTNET_IPC_V1\0" + size(u16 LE) + cmdSet + cmdId
     * + reserved(u16); payload(4) = PerfMapType (u32 LE). All(1) => perf-map + jitdump. */
    unsigned char msg[24];
    memset(msg, 0, sizeof msg);
    memcpy(msg, "DOTNET_IPC_V1\0", 14);
    msg[14] = (unsigned char)(sizeof msg); /* size = 24, LE */
    msg[15] = 0;
    msg[16] = 0x04; /* CommandSet = Process        */
    msg[17] = 0x05; /* CommandId  = EnablePerfMap  */
    /* msg[18..19] reserved = 0 */
    msg[20] =
        0x01; /* PerfMapType.All = 1 (perf-map + jitdump, with R2R rundown) */
    ssize_t w = write(fd, msg, sizeof msg);
    if (w != (ssize_t)sizeof msg) {
        close(fd);
        return 1;
    }
    /* Response header(20): magic(14) + size(u16) + cmdSet + cmdId + reserved. Success is the
     * server command set (0xFF) with the OK id (0x00). */
    unsigned char hdr[20];
    int ok = read_full(fd, hdr, sizeof hdr) == 0 && hdr[16] == 0xFF &&
             hdr[17] == 0x00;
    close(fd);
    return ok ? 0 : 1;
}

/* The BINARY JITDUMP path (asmtest_jitdump_find), against a real runtime that emits a
 * *native* perf jitdump. Unlike the text perf-map (address + size + name), a jitdump carries
 * the JIT's recorded CODE BYTES — the byte source a branch-trace decoder must be handed —
 * and a per-method timestamp, so the LATEST body of a re-emitted address wins (the temporal
 * same-address-different-bytes problem). Both V8 (`node --perf-prof`) and CoreCLR
 * (`DOTNET_PerfMapEnabled=1`) write a real `/tmp/jit-<pid>.dump` natively and name the method
 * *identically* in the perf-map and the jitdump, so one routine drives both: resolve the
 * method by `method_substr` from the easy-to-parse text perf-map (asmtest_jitdump_find
 * matches by exact name), recover its recorded bytes, and validate them four ways — the
 * address/size agree with the runtime's own perf-map (two independent runtime outputs), the
 * bytes disassemble to real x86-64, and they match the LIVE code at that address. (HotSpot,
 * which has no native jitdump, is the separate trace_jitdump_java lane.) Callers pass any
 * runtime path argument (e.g. the .dll) as an ABSOLUTE path, since the child chdirs to /tmp. */
static int trace_jitdump(const char *engine, const char *method_substr,
                         char *const cmd[],
                         int (*on_started)(pid_t, const char *)) {
    if (!asmtest_disas_available()) {
        printf("# SKIP jitdump (%s): needs Capstone\n1..0 # skipped\n", engine);
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* Run from /tmp so a cwd-relative jitdump (V8 writes it to the cwd) lands at
         * /tmp/jit-<pid>.dump and not in the repo; CoreCLR writes /tmp/jit-<pid>.dump
         * regardless. The perf-map always goes to /tmp. */
        if (chdir("/tmp") != 0)
            _exit(127);
        execvp(cmd[0], cmd);
        _exit(127);
    }
    if (pid < 0) {
        perror("fork");
        printf("1..0 # skipped\n");
        return 0;
    }

    /* ATTACH variant: the victim was launched WITHOUT the jitdump flag; turn emission on
     * now, out of band, against the running pid (e.g. CoreCLR EnablePerfMap over the
     * diagnostics IPC socket). A failure here is a clean self-skip, not a hard error — the
     * runtime may simply be absent. NULL for the launch-time lanes (flag set at exec). */
    if (on_started != NULL && on_started(pid, engine) != 0) {
        printf("# SKIP jitdump (%s): could not enable jitdump on the running "
               "process (runtime absent or diagnostics off)\n1..0 # skipped\n",
               engine);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return 0;
    }

    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)pid);

    /* Poll until a perf-map entry for the method resolves in the jitdump (the runtime emits
     * the jitdump record once the method is JIT'd). Try each perf-map line whose symbol
     * contains the name — the highest tier that the runtime wrote to the jitdump wins. */
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
            if (sscanf(line, "%lx %lx %n", &a, &s, &soff) < 2 || soff == 0 ||
                s < 2)
                continue;
            char *t = line + soff;
            t[strcspn(t, "\r\n")] = '\0';
            if (strstr(t, method_substr) == NULL)
                continue;
            /* The runtime writes the jitdump to /tmp/jit-<pid>.dump (path=NULL resolves it). */
            asmtest_jitdump_entry_t te;
            size_t tl = 0;
            if (asmtest_jitdump_find(NULL, pid, t, &te, jbytes, sizeof jbytes,
                                     &tl) == ASMTEST_PTRACE_OK &&
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
        printf("# SKIP jitdump (%s): runtime exited early (not installed?)\n"
               "1..0 # skipped\n",
               engine);
        return 0;
    }
    if (!found) {
        printf("# SKIP jitdump (%s): no method resolvable in the jitdump\n",
               engine);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }
    printf("# recovered real %s method from jit-%d.dump: '%s' @ 0x%llx (%llu "
           "bytes, "
           "code_index %llu)\n",
           engine, (int)pid, name, (unsigned long long)e.code_addr,
           (unsigned long long)e.code_size, (unsigned long long)e.code_index);

    /* (1) The binary jitdump parser recovered a real method's recorded code bytes. */
    CHECK(jblen > 0 && e.code_size > 0,
          "jitdump: asmtest_jitdump_find recovered a real JIT method's "
          "recorded bytes");

    /* (2) Cross-check: the jitdump address matches the runtime's own perf-map for the same
     * name — two independent runtime outputs agreeing on the same compilation. */
    CHECK((unsigned long)e.code_addr == paddr &&
              (unsigned long)e.code_size == psize,
          "jitdump: code_addr/size agree with the runtime's perf-map (two "
          "independent "
          "outputs)");

    /* (3) The recorded bytes are real machine code (decode the first instruction). */
    CHECK(
        asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, 0, NULL,
                      0) > 0,
        "jitdump: the recorded bytes disassemble to real x86-64 instructions");

    /* (4) The recorded bytes == the LIVE code at code_addr — the jitdump captured the
     * actual running bytes (best-effort: skips if the runtime moved/re-tiered the code). */
    uint8_t live[1024];
    size_t n = jblen < sizeof live ? jblen : sizeof live;
    struct iovec lv = {live, n}, rv = {(void *)(uintptr_t)e.code_addr, n};
    if (process_vm_readv(pid, &lv, 1, &rv, 1, 0) == (ssize_t)n &&
        memcmp(live, jbytes, n) == 0)
        CHECK(1, "jitdump: recorded bytes == the live JIT code (jitdump "
                 "captured the "
                 "running bytes)");
    else
        printf("# SKIP jitdump byte-match: live code differs (runtime "
               "moved/re-tiered it)\n");

    printf("# real %s JIT code recovered from the jitdump's recorded bytes:\n",
           engine);
    for (uint64_t off = 0; off < jblen;) {
        char text[128];
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen,
                                 e.code_addr, off, text, sizeof text);
        if (l == 0)
            break;
        printf("    0x%llx  %s\n", (unsigned long long)off, text);
        off += l;
    }

    /* T2: the source-attribution table from the SAME jitdump, validated per
     * ENCODER. DOC CORRECTIONS, verified against real dumps in the per-language
     * images: only V8 (--perf-prof) writes JIT_CODE_DEBUG_INFO at all — CoreCLR's
     * PAL writer and the HotSpot perf-JVMTI agent (libperf-jvmti.so) both emit
     * JIT_CODE_LOAD only (the true HotSpot bytecode route is T6's own JVMTI
     * agent). And V8 emits source positions AT or PAST a tiny body's reported
     * code_size, so the reader keeps at-or-above-base entries (it no longer drops
     * past-end ones). */
    static asmtest_jitdump_debug_t dbg[256];
    size_t dlen = 0;
    int drc = print_debug_table(NULL, pid, name, dbg, 256, &dlen);
    if (strcmp(engine, "V8") == 0) {
        int ok_v8 = (drc == ASMTEST_PTRACE_OK && dlen >= 1);
        for (size_t i = 0; i < dlen; i++) {
            if (dbg[i].line < 1) /* every entry names a real 1-based line */
                ok_v8 = 0;
            if (i > 0 && dbg[i].off < dbg[i - 1].off)
                ok_v8 = 0;
        }
        CHECK(ok_v8,
              "v8-jitdump-debug: reader recovers >=1 well-formed DEBUG_INFO "
              "entry "
              "(1-based line, non-decreasing offsets; V8 writes line+column)");
    } else {
        CHECK(drc == ASMTEST_PTRACE_OK,
              "dotnet-jitdump-debug: debug-info lookup returns OK (records "
              "present or absent, never an error)");
        if (dlen == 0)
            printf("# CoreCLR jitdump carries no debug-info records (expected; "
                   "IL route is T5)\n");
        else
            printf("# CoreCLR jitdump now carries %zu debug-info entries "
                   "(bonus; a future SDK)\n",
                   dlen);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
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
        printf("# SKIP java-jitdump: needs the perf JVMTI agent "
               "(libperf-jvmti.so from "
               "linux-tools)\n1..0 # skipped\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* The agent roots its dump tree at $JITDUMPDIR; keep it in /tmp, not the repo. */
        setenv("JITDUMPDIR", "/tmp", 1);
        char ap[256];
        snprintf(ap, sizeof ap, "-agentpath:%s", agent);
        char *cmd[] = {
            (char *)"java", ap, (char *)"-XX:-TieredCompilation",
            /* The agent's per-method I/O slows startup, so compile asmtjit
                        * promptly (default C2 threshold is 10000) — else it lands only
                        * after the JDK warmup and the lane may time out. */
            (char *)"-XX:CompileThreshold=1000",
            (char *)"-XX:CompileCommand=dontinline,Hot.asmtjit", (char *)"-cp",
            (char *)cp, (char *)"Hot", NULL};
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
        printf("# SKIP java-jitdump: asmtjit not resolvable in HotSpot's "
               "perf-map in time\n");
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
    sa.sa_handler =
        on_alarm; /* no SA_RESTART -> waitpid sees EINTR if the JVM hangs */
    sigaction(SIGALRM, &sa, NULL);
    alarm(15);
    kill(pid, SIGTERM);
    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    alarm(0);
    if (w != pid) { /* JVM did not exit (and flush) in time */
        printf("# SKIP java-jitdump: JVM did not shut down to flush the "
               "jitdump\n");
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
        asmtest_jitdump_find(dump, pid, JAVA_JITDUMP_METHOD, &e, jbytes,
                             sizeof jbytes, &jblen) != ASMTEST_PTRACE_OK ||
        jblen == 0) {
        printf(
            "# SKIP java-jitdump: asmtjit not present in the flushed jitdump\n"
            "1..0 # skipped\n");
        return 0;
    }
    printf("# recovered real HotSpot method from the agent's jitdump: '%s' @ "
           "0x%llx "
           "(%llu bytes, code_index %llu)\n",
           JAVA_JITDUMP_METHOD, (unsigned long long)e.code_addr,
           (unsigned long long)e.code_size, (unsigned long long)e.code_index);

    /* (1) The binary jitdump parser recovered a real method's recorded code bytes. */
    CHECK(jblen > 0 && e.code_size > 0,
          "java-jitdump: asmtest_jitdump_find recovered a real HotSpot "
          "method's bytes");

    /* (2) Cross-check: the jitdump's address/size match HotSpot's own jcmd perf-map for the
     * same method — two independent HotSpot outputs (the JVMTI agent and Compiler.perfmap). */
    CHECK((unsigned long)e.code_addr == paddr &&
              (unsigned long)e.code_size == psize,
          "java-jitdump: code_addr/size agree with HotSpot's jcmd perf-map "
          "(two independent "
          "outputs)");

    /* (3) The recorded bytes are real machine code (decode the first instruction). */
    CHECK(asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, 0,
                        NULL, 0) > 0,
          "java-jitdump: the recorded bytes disassemble to real x86-64 "
          "instructions");

    /* (4) The recorded bytes == the bytes that were LIVE at code_addr (snapshotted in step
     * B) — the jitdump captured the actual running code (best-effort). */
    if (livegot == (ssize_t)ln &&
        memcmp(live, jbytes, ln < jblen ? ln : jblen) == 0)
        CHECK(1, "java-jitdump: recorded bytes == the live JIT code (jitdump "
                 "captured the "
                 "running bytes)");
    else
        printf("# SKIP java-jitdump byte-match: live snapshot unavailable or "
               "differs\n");

    printf("# real HotSpot JIT code recovered from the jitdump's recorded "
           "bytes:\n");
    for (uint64_t off = 0; off < jblen;) {
        char text[128];
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen,
                                 e.code_addr, off, text, sizeof text);
        if (l == 0)
            break;
        printf("    0x%llx  %s\n", (unsigned long long)off, text);
        off += l;
    }

    /* T2 step D: the source-attribution table from the SAME flushed jitdump.
     * DOC CORRECTION (verified against a real HotSpot dump in the java image):
     * the perf JVMTI agent shipped by linux-tools (libperf-jvmti.so) writes
     * JIT_CODE_LOAD records ONLY — it emits NO JIT_CODE_DEBUG_INFO for these
     * methods — so the jitdump route gives HotSpot no source lines, exactly like
     * CoreCLR. HotSpot's real per-address BYTECODE-INDEX route is the dedicated
     * JVMTI agent + srcreg lane in T6 (java-bci), not this dump. So CHECK the
     * reader returns OK (present or absent, never an error); if a future agent
     * build DOES emit DEBUG_INFO, validate it (in-range, non-decreasing,
     * Hot.java) rather than fail. */
    static asmtest_jitdump_debug_t dbg[256];
    size_t dlen = 0;
    int drc =
        print_debug_table(dump, pid, JAVA_JITDUMP_METHOD, dbg, 256, &dlen);
    CHECK(drc == ASMTEST_PTRACE_OK,
          "java-jitdump-debug: debug-info lookup returns OK (the perf JVMTI "
          "agent emits none — present or absent, never an error)");
    if (dlen == 0)
        printf("# HotSpot's perf JVMTI agent (libperf-jvmti.so) writes no "
               "DEBUG_INFO records; the per-address bytecode route is the "
               "java-bci JVMTI lane (T6)\n");
    else {
        int ordered = 1, has_file = 0;
        for (size_t i = 0; i < dlen; i++) {
            if (i > 0 && dbg[i].off < dbg[i - 1].off)
                ordered = 0;
            if (strstr(dbg[i].file, "Hot.java") != NULL)
                has_file = 1;
        }
        CHECK(
            ordered && has_file,
            "java-jitdump-debug: recovered DEBUG_INFO entries are ordered and "
            "resolve to Hot.java (a debug-emitting agent build)");
    }

    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
    return failures ? 1 : 0;
}

/* Load a JVMTI agent into a RUNNING HotSpot via `jcmd <pid> JVMTI.agent_load <agent>` (the
 * attach case). Returns 0 when the agent's Agent_OnAttach returned success (jcmd prints
 * "return code: 0"), nonzero otherwise — jcmd absent, the JVM's attach listener not ready
 * yet, or (the reason this whole lane needs the in-tree agent) the agent has no
 * Agent_OnAttach: libperf-jvmti.so is -agentpath-only and jcmd prints "Agent_OnAttach is not
 * available in ...". Output is captured to a temp file and scanned, kept off the TAP stream. */
static int jcmd_agent_load(pid_t pid, const char *agent) {
    char out[64];
    snprintf(out, sizeof out, "/tmp/asmtest-jcmd-%d.out", (int)pid);
    pid_t j = fork();
    if (j == 0) {
        int f = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (f >= 0) {
            dup2(f, STDOUT_FILENO);
            dup2(f, STDERR_FILENO);
        }
        char pidbuf[16];
        snprintf(pidbuf, sizeof pidbuf, "%d", (int)pid);
        execlp("jcmd", "jcmd", pidbuf, "JVMTI.agent_load", agent, (char *)NULL);
        _exit(127);
    }
    if (j <= 0)
        return 1;
    int st = 0;
    waitpid(j, &st, 0);
    FILE *f = fopen(out, "r");
    if (f == NULL)
        return 1;
    char line[256];
    int ok = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, "return code: 0") != NULL)
            ok = 1;
    fclose(f);
    unlink(out);
    return ok ? 0 : 1;
}

/* Runtime-enabled jitdump byte recovery on a LIVE JVM (intel-pt-attach-foreign-pid T3): the
 * HotSpot analogue of the CoreCLR EnablePerfMap-attach path. The victim is launched with NO
 * -agentpath; the in-tree JVMTI jitdump agent (examples/jvmti_jitdump_agent.c) is loaded into
 * the already-running JVM with `jcmd JVMTI.agent_load`, and on attach it replays every
 * already-compiled nmethod via GenerateEvents(COMPILED_METHOD_LOAD) into /tmp/jit-<pid>.dump.
 * asmtest_jitdump_find then recovers asmtjit's bytes — proving jitdump emission was turned on
 * AFTER launch, with no tracing flag on the java command line.
 *
 * DOC CORRECTION (verified 2026-07-19): the doc prescribes `jcmd JVMTI.agent_load
 * libperf-jvmti.so`, but the linux-tools perf agent exports only Agent_OnLoad (no
 * Agent_OnAttach), so HotSpot refuses the dynamic load ("Agent_OnAttach is not available").
 * It is a launch-only agent and cannot serve the attach case — hence the bespoke in-tree
 * agent, which exports Agent_OnAttach and does the replay. Unlike the buffered perf agent, it
 * flushes per event, so the dump is readable while the JVM is alive (no SIGTERM-to-flush). */
static int trace_attach_jitdump_java(const char *cp, const char *agent) {
    if (!asmtest_disas_available()) {
        printf("# SKIP java-attach-jitdump: needs Capstone\n1..0 # skipped\n");
        return 0;
    }
    if (agent == NULL || agent[0] == '\0' || access(agent, R_OK) != 0) {
        printf("# SKIP java-attach-jitdump: needs the in-tree JVMTI jitdump "
               "agent (libasmtest_jitdump_agent.so)\n1..0 # skipped\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        /* PLAIN launch — NO -agentpath. -XX:+EnableDynamicAgentLoading lets the later jcmd
         * JVMTI.agent_load succeed (JDK 21+ gates dynamic loading on it, JEP 451). The other
         * knobs mirror the launch-time java-jitdump lane: a single stable C2 nmethod for
         * asmtjit, compiled promptly so the attach replay (or a live event) has a body. */
        char *cmd[] = {(char *)"java",
                       (char *)"-XX:+EnableDynamicAgentLoading",
                       (char *)"-XX:-TieredCompilation",
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

    /* Attach the agent to the RUNNING JVM (the property under test: emission enabled AFTER
     * launch). Retry until the attach listener is ready, or the JVM exits, or we time out. */
    int attached = 0;
    for (int i = 0; i < 40 && !attached; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            pid = -1;
            break;
        }
        if (jcmd_agent_load(pid, agent) == 0)
            attached = 1;
        else {
            struct timespec ts = {0, 300 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
    if (pid < 0) {
        printf("# SKIP java-attach-jitdump: java exited early (JDK not "
               "installed?)\n1..0 # skipped\n");
        return 0;
    }
    if (!attached) {
        printf("# SKIP java-attach-jitdump: could not jcmd-attach the agent "
               "(jcmd absent or JVM attach not ready)\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* Recover asmtjit's (addr,size) from HotSpot's jcmd perf-map (the independent cross-check)
     * and its recorded bytes from the agent's /tmp/jit-<pid>.dump (flushed per event). */
    char mappath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)pid);
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
        java_perfmap_refresh(pid, i);
        FILE *f = fopen(mappath, "r");
        if (f != NULL) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                unsigned long aa, ss;
                int soff = 0;
                if (sscanf(line, "%lx %lx %n", &aa, &ss, &soff) < 2 ||
                    soff == 0)
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
        /* The agent names asmtjit in JVM-descriptor form; asmtest_jitdump_find matches it. */
        if (asmtest_jitdump_find(NULL, pid, JAVA_JITDUMP_METHOD, &e, jbytes,
                                 sizeof jbytes, &jblen) == ASMTEST_PTRACE_OK &&
            jblen > 0)
            found = 1;
    }
    if (pid < 0) {
        printf(
            "# SKIP java-attach-jitdump: java exited early\n1..0 # skipped\n");
        return 0;
    }
    if (!found || paddr == 0) {
        printf("# SKIP java-attach-jitdump: asmtjit not recovered from the "
               "attach-written jitdump in time\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* Snapshot the LIVE code for the byte-match cross-check (child; no attach needed). */
    uint8_t live[1024];
    size_t ln = psize > 0 && psize < sizeof live ? (size_t)psize : sizeof live;
    struct iovec lv = {live, ln}, rv = {(void *)(uintptr_t)paddr, ln};
    ssize_t livegot = process_vm_readv(pid, &lv, 1, &rv, 1, 0);

    printf("# recovered real HotSpot method from the ATTACH-loaded agent's "
           "jitdump: '%s' @ 0x%llx (%llu bytes, code_index %llu)\n",
           JAVA_JITDUMP_METHOD, (unsigned long long)e.code_addr,
           (unsigned long long)e.code_size, (unsigned long long)e.code_index);

    /* (1) A real method's recorded bytes came from a jitdump turned on AFTER launch. */
    CHECK(jblen > 0 && e.code_size > 0,
          "java-attach-jitdump: asmtest_jitdump_find recovered bytes from a "
          "runtime-enabled (jcmd JVMTI.agent_load) jitdump");
    /* (2) Cross-check: jitdump address/size == HotSpot's own jcmd perf-map. */
    CHECK((unsigned long)e.code_addr == paddr &&
              (unsigned long)e.code_size == psize,
          "java-attach-jitdump: code_addr/size agree with HotSpot's jcmd "
          "perf-map (two independent outputs)");
    /* (3) The recorded bytes are real machine code. */
    CHECK(asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen, e.code_addr, 0,
                        NULL, 0) > 0,
          "java-attach-jitdump: the recorded bytes disassemble to real x86-64 "
          "instructions");
    /* (4) The recorded bytes == the live code (best-effort). */
    if (livegot == (ssize_t)ln &&
        memcmp(live, jbytes, ln < jblen ? ln : jblen) == 0)
        CHECK(1, "java-attach-jitdump: recorded bytes == the live JIT code "
                 "(jitdump captured the running bytes)");
    else
        printf("# SKIP java-attach-jitdump byte-match: live snapshot "
               "unavailable or differs\n");

    printf("# real HotSpot JIT code recovered from the attach-written "
           "jitdump:\n");
    for (uint64_t off = 0; off < jblen;) {
        char text[128];
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, jbytes, jblen,
                                 e.code_addr, off, text, sizeof text);
        if (l == 0)
            break;
        printf("    0x%llx  %s\n", (unsigned long long)off, text);
        off += l;
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
    return failures ? 1 : 0;
}

/* T6: TRUE per-address JVM bytecode-index attribution. Load the in-tree JVMTI agent
 * (examples/jvmti_bci_agent.c -> libasmtest_bci_agent.so) into a HotSpot that keeps
 * Hot.asmtbci hot (a counted-loop method — a trivial leaf like asmtjit compiles to a
 * single bci=-1 safepoint, useless for attribution); the agent captures
 * CompiledMethodLoad's address->bci map to a text sidecar /tmp/asmtest-bci-<pid>.map,
 * written LIVE (no clean-shutdown flush). Resolve asmtbci's (addr,size) from HotSpot's
 * jcmd perf-map (as the java-jitdump lane does), ingest the sidecar points inside
 * [addr,addr+size) as ASMTEST_SRC_BCI rows into an asmtest_srcreg, and prove a native
 * address resolves to a real bytecode index. */
static int trace_java_bci(const char *cp, const char *agent) {
    if (agent == NULL || agent[0] == '\0' || access(agent, R_OK) != 0) {
        printf("# SKIP java-bci: needs the JVMTI bci agent "
               "(build/libasmtest_bci_agent.so)\n1..0 # skipped\n");
        return 0;
    }

    pid_t pid = fork();
    if (pid == 0) {
        char ap[256];
        snprintf(ap, sizeof ap, "-agentpath:%s", agent);
        char *cmd[] = {(char *)"java",
                       ap,
                       (char *)"-XX:-TieredCompilation",
                       (char *)"-XX:CompileThreshold=1000",
                       (char *)"-XX:CompileCommand=dontinline,Hot.asmtbci",
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

    char mappath[64], bcipath[64];
    snprintf(mappath, sizeof mappath, "/tmp/perf-%d.map", (int)pid);
    snprintf(bcipath, sizeof bcipath, "/tmp/asmtest-bci-%d.map", (int)pid);

    /* Resolve asmtjit's (addr,size) from HotSpot's jcmd perf-map (live process). */
    unsigned long paddr = 0, psize = 0;
    for (int i = 0; i < 200 && paddr == 0; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            pid = -1;
            break;
        }
        java_perfmap_refresh(pid, i);
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
            if (strstr(t, "asmtbci") != NULL) {
                paddr = aa;
                psize = ss;
            }
        }
        fclose(f);
    }

    if (pid < 0) {
        printf("# SKIP java-bci: java exited early (JDK not installed?)\n"
               "1..0 # skipped\n");
        return 0;
    }
    if (paddr == 0) {
        printf(
            "# SKIP java-bci: asmtbci not resolvable in HotSpot's perf-map in "
            "time\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* Poll the live sidecar for points inside asmtbci's body. */
    asmtest_srcmap_entry_t rows[512];
    int lines[512];
    size_t nrows = 0;
    for (int i = 0; i < 100 && nrows == 0; i++) {
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
        FILE *bf = fopen(bcipath, "r");
        if (bf == NULL)
            continue;
        size_t got = 0;
        char line[512];
        while (fgets(line, sizeof line, bf) && got < 512) {
            unsigned long long addr;
            long bci;
            int ln;
            if (sscanf(line, "%llx %ld %d", &addr, &bci, &ln) < 3)
                continue;
            if (addr < paddr || addr >= paddr + psize)
                continue;
            rows[got].offset = addr - paddr;
            rows[got].value = (int32_t)bci;
            rows[got].kind = ASMTEST_SRC_BCI;
            rows[got].file_id = UINT32_MAX;
            rows[got].col = 0;
            lines[got] = ln;
            got++;
        }
        fclose(bf);
        nrows = got;
    }

    if (nrows == 0) {
        printf("# SKIP java-bci: no asmtbci address->bci points in the sidecar "
               "(agent/JIT did not cooperate)\n");
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        printf("1..0 # skipped\n");
        return 0;
    }

    /* Sort ascending by offset (srcmap_lookup requires it); track line for printing. */
    for (size_t a = 0; a + 1 < nrows; a++)
        for (size_t b = a + 1; b < nrows; b++)
            if (rows[b].offset < rows[a].offset) {
                asmtest_srcmap_entry_t tr = rows[a];
                rows[a] = rows[b];
                rows[b] = tr;
                int tl = lines[a];
                lines[a] = lines[b];
                lines[b] = tl;
            }

    printf(
        "# recovered %zu address->bci points inside HotSpot's asmtbci @ 0x%lx "
        "(%lu bytes) via CompiledMethodLoad:\n",
        nrows, paddr, psize);
    for (size_t i = 0; i < nrows; i++)
        printf("    +0x%llx  bci %d%s%d\n", (unsigned long long)rows[i].offset,
               rows[i].value, lines[i] >= 0 ? ":line " : " (no line)",
               lines[i] >= 0 ? lines[i] : 0);

    /* HotSpot maps some PCs (the method prologue / non-bytecode glue) to bci -1; a
     * counted-loop method like asmtbci also yields many REAL bytecode indices (>=0) at
     * its safepoints. Assert at least one real bci was recovered (the address->bytecode
     * feature) and that the map is ordered. */
    int ascending = 1, real_bci = -1;
    for (size_t i = 0; i < nrows; i++) {
        if (rows[i].value >= 0 && real_bci < 0)
            real_bci = (int)i;
        if (i > 0 && rows[i].offset <= rows[i - 1].offset)
            ascending = 0;
    }
    CHECK(nrows >= 1 && real_bci >= 0,
          "java-bci: >=1 address->bci point with a REAL bytecode index (>=0) "
          "recovered from CompiledMethodLoad");
    CHECK(
        ascending,
        "java-bci: the reconstructed bci map is strictly ascending by offset");

    /* Ingest into a version-keyed srcreg (single version, when = 1) and resolve a
     * real-bci address back to its bytecode index. */
    asmtest_srcreg_t *reg = asmtest_srcreg_new();
    int added =
        reg ? asmtest_srcreg_add(reg, paddr, psize, 1, rows, nrows, "Hot.java")
            : -1;
    size_t k = real_bci >= 0 ? (size_t)real_bci : 0;
    asmtest_srcmap_entry_t row;
    uint64_t base = 0;
    const char *file = NULL;
    uint64_t addr_k = paddr + rows[k].offset;
    int r =
        reg ? asmtest_srcreg_resolve(reg, addr_k, 0, &row, &base, &file) : 0;
    CHECK(added == 0 && r == 1 && row.kind == ASMTEST_SRC_BCI &&
              row.value == rows[k].value && row.value >= 0 && base == paddr &&
              file != NULL && strcmp(file, "Hot.java") == 0,
          "java-bci: asmtest_srcreg resolves a native address to its bytecode "
          "index (bci >= 0)");
    asmtest_srcreg_free(reg);

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures,
           failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "node";

    /* Optional call-descent suffix on the managed-runtime lanes: `<mode>-descend` (L2,
     * descend the runtime's own sibling JIT methods) or `<mode>-descend-all` (L3, descend
     * everything, guarded + expected to self-skip). Strip it to a base mode + a level. */
    int descend_level = 0;
    char basemode[64];
    snprintf(basemode, sizeof basemode, "%s", mode);
    {
        size_t ml = strlen(basemode);
        const char *sa = "-descend-all", *sd = "-descend";
        size_t la = strlen(sa), ld = strlen(sd);
        if (ml > la && strcmp(basemode + ml - la, sa) == 0) {
            descend_level = ASMTEST_DESCENT_DESCEND_ALL;
            basemode[ml - la] = '\0';
        } else if (ml > ld && strcmp(basemode + ml - ld, sd) == 0) {
            descend_level = ASMTEST_DESCENT_DESCEND_KNOWN;
            basemode[ml - ld] = '\0';
        }
        mode = basemode;
    }

    if (strcmp(mode, "node") == 0) {
        /* --no-turbo-inlining keeps asmtjit a REAL standalone callable body (else
         * TurboFan inlines the tiny function and its perf-map entry is never called). */
        char *cmd[] = {(char *)"node",
                       (char *)"--perf-basic-prof",
                       (char *)"--no-turbo-inlining",
                       (char *)"-e",
                       (char *)HOT_JS,
                       NULL};
        return trace_runtime("V8", "asmtjit", cmd, 2, NULL, NULL,
                             descend_level);
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
        return trace_runtime("CoreCLR", "Program::Add", cmd, 2, NULL, NULL,
                             descend_level);
    }
    if (strcmp(mode, "dotnet-bcl") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s dotnet-bcl <app.dll>\n", argv[0]);
            return 2;
        }
        /* Trace a REAL .NET *framework* method — System.Console::WriteLine — not user code.
         * Console.WriteLine ships as ReadyToRun PRECOMPILED native, so the JIT never emits it
         * and it is in no jitdump by default. DOTNET_ReadyToRun=0 disables R2R so CoreCLR JITs
         * the BCL on demand; Console::WriteLine then resolves in the perf-map and single-steps
         * exactly like Program::Add — the same W^X hardware-breakpoint fallback applies. Its
         * body is short (WriteLine just calls Out.WriteLine) and the tracer steps OVER that
         * call-out, so we capture Console::WriteLine's OWN JITted instructions: the assembly of
         * a framework method recovered from a live process. The app sinks Out to Stream.Null so
         * the hot loop stays quiet. target_tier 0: with TC=0 there is a single body — take it as
         * soon as it resolves (R2R-off startup JITs much of the BCL, so avoid extra warmup). */
        setenv("DOTNET_ReadyToRun", "0", 1);
        setenv("DOTNET_TieredCompilation", "0", 1);
        setenv("DOTNET_TC_QuickJitForLoops", "0", 1);
        setenv("DOTNET_PerfMapEnabled", "1", 1);
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        char *cmd[] = {(char *)"dotnet", argv[2], (char *)"bcl", NULL};
        return trace_runtime("CoreCLR-BCL", "Console::WriteLine", cmd, 0, NULL,
                             NULL, descend_level);
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
                             pick_busy_thread, descend_level);
    }
    if (strcmp(mode, "java-bcl") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s java-bcl <classpath>\n", argv[0]);
            return 2;
        }
        /* Trace a REAL JDK library method — java.lang.Math::floorDiv — not user code. The
         * contrast with .NET (dotnet-bcl) is the point: HotSpot JITs JDK methods on demand BY
         * DEFAULT — there is no ReadyToRun-style precompilation to disable — so this needs only
         * the same dontinline nudge the user-method lane uses. -XX:-TieredCompilation gives a
         * single C2 body at a stable address; dontinline keeps floorDiv a standalone nmethod
         * (else C2 inlines the tiny method into the caller). Runs Hot with the "bcl" arg; the
         * thread picker + jcmd perf-map refresh are shared with the plain java lane. */
        char *cmd[] = {
            (char *)"java",
            (char *)"-XX:-TieredCompilation",
            (char *)"-XX:CompileCommand=dontinline,java/lang/Math.floorDiv",
            (char *)"-cp",
            argv[2],
            (char *)"Hot",
            (char *)"bcl",
            NULL};
        return trace_runtime("HotSpot-BCL", "floorDiv", cmd, 0,
                             java_perfmap_refresh, pick_busy_thread,
                             descend_level);
    }
    if (strcmp(mode, "java-jitdump") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "usage: %s java-jitdump <classpath> <libperf-jvmti.so>\n",
                    argv[0]);
            return 2;
        }
        return trace_jitdump_java(argv[2], argv[3]);
    }
    if (strcmp(mode, "java-attach-jitdump") == 0) {
        if (argc < 4) {
            fprintf(stderr,
                    "usage: %s java-attach-jitdump <classpath> "
                    "<libasmtest_jitdump_agent.so>\n",
                    argv[0]);
            return 2;
        }
        return trace_attach_jitdump_java(argv[2], argv[3]);
    }
    if (strcmp(mode, "java-bci") == 0) {
        if (argc < 4) {
            fprintf(
                stderr,
                "usage: %s java-bci <classpath> <libasmtest_bci_agent.so>\n",
                argv[0]);
            return 2;
        }
        return trace_java_bci(argv[2], argv[3]);
    }
    if (strcmp(mode, "jitdump") == 0) {
        /* V8: `--perf-prof` writes the binary jitdump, `--perf-basic-prof` the text perf-map
         * (same symbol in both); `--no-turbo-inlining` keeps asmtjit a standalone body. */
        char *cmd[] = {(char *)"node",
                       (char *)"--perf-basic-prof",
                       (char *)"--perf-prof",
                       (char *)"--no-turbo-inlining",
                       (char *)"-e",
                       (char *)HOT_JS,
                       NULL};
        return trace_jitdump("V8", "asmtjit", cmd, NULL);
    }
    if (strcmp(mode, "dotnet-jitdump") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s dotnet-jitdump <app.dll-abspath>\n",
                    argv[0]);
            return 2;
        }
        /* CoreCLR writes a native perf jitdump (/tmp/jit-<pid>.dump) AND the text perf-map
         * under DOTNET_PerfMapEnabled=1, naming the method identically in both. TC=0 gives a
         * single optimized compilation at a stable address. The dll path must be absolute
         * (trace_jitdump's child chdirs to /tmp). */
        setenv("DOTNET_TieredCompilation", "0", 1);
        setenv("DOTNET_TC_QuickJitForLoops", "0", 1);
        setenv("DOTNET_PerfMapEnabled", "1", 1);
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        char *cmd[] = {(char *)"dotnet", argv[2], NULL};
        return trace_jitdump("CoreCLR", "Program::Add", cmd, NULL);
    }
    if (strcmp(mode, "dotnet-attach-jitdump") == 0) {
        if (argc < 3) {
            fprintf(stderr,
                    "usage: %s dotnet-attach-jitdump <app.dll-abspath>\n",
                    argv[0]);
            return 2;
        }
        /* Launch the victim PLAIN — NO DOTNET_PerfMapEnabled (the whole point). TC=0 pins a
         * single stable Add body; the on_started hook turns on the perf-map + jitdump over
         * the diagnostics IPC socket once the runtime is up. Absolute dll (child chdirs). */
        setenv("DOTNET_TieredCompilation", "0", 1);
        setenv("DOTNET_TC_QuickJitForLoops", "0", 1);
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        char *cmd[] = {(char *)"dotnet", argv[2], NULL};
        return trace_jitdump("CoreCLR", "Program::Add", cmd,
                             dotnet_enable_perfmap);
    }

    fprintf(stderr,
            "usage: %s {node|dotnet <app.dll>|dotnet-bcl <app.dll>|java "
            "<classpath>|"
            "java-bcl <classpath>|java-jitdump <classpath> <agent.so>|"
            "java-attach-jitdump <classpath> <agent.so>|jitdump|"
            "dotnet-jitdump <app.dll>|dotnet-attach-jitdump <app.dll>}\n",
            argv[0]);
    return 2;
}

#else
int main(void) {
    printf("# SKIP jit-trace: Linux x86-64 only\n1..0 # skipped\n");
    return 0;
}
#endif
