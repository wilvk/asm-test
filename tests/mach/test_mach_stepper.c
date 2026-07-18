/*
 * test_mach_stepper.c — macOS out-of-process Mach stepper live test.
 * Driven by `make mach-stepper-test` (docs/internal/implementations/
 * macos-oop-mach-stepper.md). Darwin x86-64 only; self-skips everywhere else and
 * when task_for_pid is denied (no com.apple.security.cs.debugger entitlement, not
 * root — see scripts/codesign-debugger.sh).
 *
 * Exercises the three public entry points against the exact fixtures
 * examples/test_hwtrace.c uses for the in-process single-step backend, so the
 * expected streams are the SAME literal offsets asserted there:
 *   - asmtest_mach_trace_call     (T4): fork-and-trace a self-contained blob.
 *   - asmtest_mach_trace_attached (T3): trace an already-stopped foreign process.
 *   - asmtest_mach_run_to         (T5): resolve a method entry with timing NOT
 *     controlled by the tracer, then trace it — both the software int3 arm and,
 *     forced via ASMTEST_MACH_HW_BP, the DR0/DR7 hardware-breakpoint fallback.
 */
#include "asmtest_mach.h"
#include "asmtest_trace.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf(c ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);            \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret — examples/test_hwtrace.c's
 * shared ROUTINE fixture; add2(20,22)=42 takes the jle (skips the dec), yielding the
 * exact stream {0x0,0x3,0x6,0xc,0x11} (two blocks: {0, 0x11}). */
static const unsigned char ROUTINE[] = {0x48, 0x89, 0xf8, 0x48, 0x01, 0xf0,
                                        0x48, 0x3d, 0x64, 0x00, 0x00, 0x00,
                                        0x7e, 0x03, 0x48, 0xff, 0xc8, 0xc3};
static const uint64_t ROUTINE_EXPECT[] = {0x0, 0x3, 0x6, 0xc, 0x11};
#define ROUTINE_EXPECT_LEN (sizeof ROUTINE_EXPECT / sizeof ROUTINE_EXPECT[0])

/* mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret — examples/test_hwtrace.c's LOOP
 * fixture; loop(1,20)=20 over 62 insns (1 + 20*3 + 1), well past AMD LBR's 16-branch
 * window, proving the per-instruction single-step tier has no depth ceiling. */
static const unsigned char LOOP[] = {0x48, 0xc7, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x48,
                                     0x01, 0xf8, 0x48, 0xff, 0xce, 0x75, 0xf8, 0xc3};

typedef long (*fn2_t)(long, long);

static void *map_exec(const unsigned char *bytes, size_t len) {
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                   0);
    if (p == MAP_FAILED)
        return NULL;
    memcpy(p, bytes, len);
    mprotect(p, len, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)p, (char *)p + len);
    return p;
}

static int stream_matches(const asmtest_trace_t *t, const uint64_t *expect, size_t n) {
    if (t->insns_len != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (t->insns[i] != expect[i])
            return 0;
    return 1;
}

/* Fork a tracee that raises(SIGSTOP) before calling fn(a0, a1), and wait for that
 * stop. Returns the tracee's pid, or -1 (already reaped) on failure. */
static pid_t fork_stopped(void *fn_addr, long a0, long a1) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        raise(SIGSTOP);
        fn2_t fn = (fn2_t)fn_addr;
        fn(a0, a1);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, WUNTRACED) < 0 || !WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return -1;
    }
    return pid;
}

static void reap(pid_t pid) {
    int status;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

/* ------------------------------------------------------------------ */
/* T4 — asmtest_mach_trace_call: fork-and-trace a self-contained blob.  */
/* ------------------------------------------------------------------ */
static void test_trace_call(void) {
    void *routine = map_exec(ROUTINE, sizeof ROUTINE);
    void *loop = map_exec(LOOP, sizeof LOOP);
    CHECK(routine != NULL && loop != NULL, "trace_call: fixtures mapped executable");
    if (routine == NULL || loop == NULL)
        return;

    long args[2] = {20, 22}, result = -1;
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    int rc = asmtest_mach_trace_call(routine, sizeof ROUTINE, args, 2, &result, t);
    CHECK(rc == ASMTEST_MACH_OK, "trace_call: leaf fixture succeeds");
    CHECK(result == 42, "trace_call: leaf fixture returns 20+22");
    CHECK(stream_matches(t, ROUTINE_EXPECT, ROUTINE_EXPECT_LEN),
         "trace_call: leaf stream is [0x0,0x3,0x6,0xc,0x11]");
    CHECK(!t->truncated, "trace_call: leaf trace is complete");
    asmtest_trace_free(t);

    long largs[2] = {1, 20}, lresult = -1;
    asmtest_trace_t *lt = asmtest_trace_new(256, 64);
    int lrc = asmtest_mach_trace_call(loop, sizeof LOOP, largs, 2, &lresult, lt);
    CHECK(lrc == ASMTEST_MACH_OK, "trace_call: loop fixture succeeds");
    CHECK(lresult == 20, "trace_call: loop fixture returns sum of 1, 20 times");
    CHECK(asmtest_emu_trace_insns_total(lt) == 62,
         "trace_call: loop captures all 62 insns, no depth ceiling");
    CHECK(!lt->truncated, "trace_call: loop trace is complete");
    asmtest_trace_free(lt);

    asmtest_trace_t *nt = asmtest_trace_new(64, 64);
    int nrc = asmtest_mach_trace_call(routine, sizeof ROUTINE, args, 2, NULL, nt);
    CHECK(nrc == ASMTEST_MACH_OK, "trace_call: NULL result is accepted");
    asmtest_trace_free(nt);
}

/* ------------------------------------------------------------------ */
/* T3 — asmtest_mach_trace_attached: trace an already-stopped foreign   */
/* process directly, independent of trace_call's fork wrapper.          */
/* ------------------------------------------------------------------ */
static void test_trace_attached(void) {
    void *routine = map_exec(ROUTINE, sizeof ROUTINE);
    CHECK(routine != NULL, "trace_attached: fixture mapped executable");
    if (routine == NULL)
        return;

    pid_t pid = fork_stopped(routine, 20, 22);
    CHECK(pid > 0, "trace_attached: forked tracee stopped");
    if (pid <= 0)
        return;

    long result = -1;
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    int rc = asmtest_mach_trace_attached(pid, routine, sizeof ROUTINE, &result, t);
    CHECK(rc == ASMTEST_MACH_OK, "trace_attached: leaf fixture succeeds");
    CHECK(result == 42, "trace_attached: leaf fixture returns 20+22");
    CHECK(stream_matches(t, ROUTINE_EXPECT, ROUTINE_EXPECT_LEN),
         "trace_attached: leaf stream matches the in-process stepper exactly");
    asmtest_trace_free(t);
    reap(pid);
}

/* ------------------------------------------------------------------ */
/* T5 — asmtest_mach_run_to: resolve a method entry with uncontrolled    */
/* timing, then trace it. Runs twice: the default software-int3 arm,    */
/* and (ASMTEST_MACH_HW_BP=1) the DR0/DR7 hardware-breakpoint fallback,  */
/* forced deterministically over ordinary (non-W^X) memory.             */
/* ------------------------------------------------------------------ */
static void run_to_case(const char *label, int force_hw) {
    char msg[160];
    void *routine = map_exec(ROUTINE, sizeof ROUTINE);
    if (routine == NULL) {
        snprintf(msg, sizeof msg, "run_to (%s): fixture mapped executable", label);
        CHECK(0, msg);
        return;
    }

    pid_t pid = fork();
    snprintf(msg, sizeof msg, "run_to (%s): fork succeeds", label);
    CHECK(pid >= 0, msg);
    if (pid < 0)
        return;
    if (pid == 0) {
        raise(SIGSTOP);
        fn2_t fn = (fn2_t)routine;
        for (;;) {
            fn(20, 22); /* the program itself calls in; the tracer does not control
                        * WHEN, matching a real managed runtime's own timing */
            usleep(2000);
        }
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, WUNTRACED) < 0 || !WIFSTOPPED(status)) {
        snprintf(msg, sizeof msg, "run_to (%s): forked tracee stopped", label);
        CHECK(0, msg);
        reap(pid);
        return;
    }

    if (force_hw)
        setenv("ASMTEST_MACH_HW_BP", "1", 1);
    else
        unsetenv("ASMTEST_MACH_HW_BP");

    int rrc = asmtest_mach_run_to(pid, routine);
    snprintf(msg, sizeof msg, "run_to (%s): resolves the in-flight call", label);
    CHECK(rrc == ASMTEST_MACH_OK, msg);

    long result = -1;
    asmtest_trace_t *t = asmtest_trace_new(64, 64);
    int trc = asmtest_mach_trace_attached(pid, routine, sizeof ROUTINE, &result, t);
    snprintf(msg, sizeof msg, "run_to (%s): trace_attached recovers the call", label);
    CHECK(trc == ASMTEST_MACH_OK, msg);
    snprintf(msg, sizeof msg, "run_to (%s): result is 20+22", label);
    CHECK(result == 42, msg);
    snprintf(msg, sizeof msg,
            "run_to (%s): stream matches the direct trace_call path", label);
    CHECK(stream_matches(t, ROUTINE_EXPECT, ROUTINE_EXPECT_LEN), msg);
    asmtest_trace_free(t);

    unsetenv("ASMTEST_MACH_HW_BP");
    reap(pid);
}

static void test_run_to(void) {
    run_to_case("software-int3", 0);
    run_to_case("hardware-DR0", 1);
}

int main(void) {
    if (!asmtest_mach_available()) {
        char why[128];
        asmtest_mach_skip_reason(why, sizeof why);
        printf("# SKIP mach-stepper: %s\n1..0 # skipped\n", why);
        return 0;
    }

    /* Probe the entitlement/root gate once, cheaply, before running the full suite:
     * task_for_pid of our own forked child needs com.apple.security.cs.debugger
     * (scripts/codesign-debugger.sh's ad-hoc self-sign) or root. */
    void *probe = map_exec(ROUTINE, sizeof ROUTINE);
    if (probe != NULL) {
        long pargs[2] = {1, 1}, presult = -1;
        asmtest_trace_t *pt = asmtest_trace_new(8, 8);
        int prc = asmtest_mach_trace_call(probe, sizeof ROUTINE, pargs, 2, &presult, pt);
        asmtest_trace_free(pt);
        if (prc == ASMTEST_MACH_EPERM) {
            printf("# SKIP mach-stepper: task_for_pid denied (EPERM) — sign with "
                  "scripts/codesign-debugger.sh (one-time admin dialog on first "
                  "use) or run under sudo\n1..0 # skipped\n");
            return 0;
        }
    }

    test_trace_call();
    test_trace_attached();
    test_run_to();

    printf("1..%d\n# %d passed, %d failed\n", checks, checks - failures, failures);
    return failures == 0 ? 0 : 1;
}
