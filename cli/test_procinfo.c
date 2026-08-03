/* test_procinfo.c — the attach-free process snapshot (asmspy_procinfo).
 *
 * The contract that matters is NEGATIVE: this gatherer must never ptrace. A
 * test cannot prove absence directly, so it proves the consequence — the
 * snapshot succeeds against a target this process has no ptrace permission
 * for. Under Yama ptrace_scope=1 (this host) our own PARENT is such a target:
 * an attach would be refused, and a correct asmspy_procinfo still fills in.
 *
 * That premise — an attach to our parent really is refused — is verified BY
 * CONSTRUCTION rather than assumed from the environment: we actually attempt
 * PTRACE_ATTACH on our own parent before trusting the assertion it backs
 * (detaching cleanly if it unexpectedly succeeds). On a ptrace_scope=0 host,
 * or one running us with CAP_SYS_PTRACE, that attach would succeed, which
 * would turn the parent-snapshot check into a tautology any ptrace-based
 * implementation would also pass — so on such a host that specific check is
 * skipped LOUDLY, with a stated reason, instead of silently validating
 * nothing.
 *
 * The rest pins the fields a UI would silently render wrong: identity against
 * values we can independently name (getpid/getppid/getuid), the RAW counter
 * contract (jiffies + a monotonic stamp, never a pre-computed rate, with
 * plausibility bounds on utime/stime/pgid/sid/elapsed_s), and the caps +
 * their truncation flags — the children cap is exercised for real (we fork
 * more live children than ASMSPY_PI_CHILDREN_CAP and check the list actually
 * truncates, not just that the flag defaults to false).
 *
 * Links asmspy_proc.o directly, like test_symtab. No ptrace ENGINE, no
 * ncurses — this file makes exactly one raw ptrace(2) call of its own, to
 * verify the negative assertion's premise; asmspy_procinfo itself is never
 * expected to attach, which is the entire point of the feature.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "asmspy.h"

static int failures;

static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

/* Deliberately consume `ms` milliseconds of CPU time (not wall time — a
 * sleep() would advance neither utime nor stime at all). clock() reports
 * THIS PROCESS's accumulated CPU time in CLOCKS_PER_SEC units, so a tight
 * spin loop reliably advances it, letting the utime/stime plausibility
 * check below cross at least one whole jiffies tick deterministically
 * instead of racing the scheduler for one. */
static void burn_cpu_ms(long ms) {
    clock_t start = clock();
    if (start == (clock_t)-1)
        return; /* no CPU-time clock on this host; the check tolerates 0 */
    while ((double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC < (double)ms) {
        /* spin */
    }
}

int main(void) {
    asmspy_procinfo_t *pi =
        malloc(sizeof *pi); /* ~40 KB — never on the stack */
    if (!pi) {
        fprintf(stderr, "FAIL alloc\n");
        return 1;
    }

    /* --- self: every identity field is independently knowable --------- */
    check("self returns 0", asmspy_procinfo(getpid(), pi) == 0, "nonzero");
    check("self pid", pi->pid == (long)getpid(), "pid mismatch");
    check("self ppid", pi->ppid == (long)getppid(), "ppid mismatch");
    check("self uid", pi->uid == (long)getuid(), "uid mismatch");
    check("self user named", pi->user[0] != '\0', "username unresolved");
    check("self comm", strstr(pi->comm, "test_procinfo") != NULL, pi->comm);
    check("self argv", pi->argc >= 1, "argc < 1");
    check("self exe", strstr(pi->exe, "test_procinfo") != NULL, pi->exe);
    check("self cwd", pi->cwd[0] == '/', "cwd not absolute");
    check("self state", pi->state == 'R' || pi->state == 'S', "odd state");
    check("start_ticks", pi->start_ticks > 0, "no start time");
    check("self pgid positive", pi->pgid > 0, "pgid not positive");
    check("self sid positive", pi->sid > 0, "sid not positive");

    /* --- counters are RAW, and stamped ------------------------------- */
    check("clk_tck", pi->clk_tck > 0, "no tick rate");
    check("monotonic stamp", pi->ts_ns > 0, "no timestamp");
    check("rss", pi->rss_kb > 0, "no RSS");
    check("threads>=1", pi->threads >= 1, "no threads");

    /* utime/stime are a whole-TICK (jiffies) counter, not a fine-grained
     * one: on a fast host the handful of /proc reads asmspy_procinfo itself
     * does can complete inside a single HZ=100 (10ms) tick and round down
     * to zero — measured FALSE here without the burn below. So force real,
     * deterministic CPU consumption (never sleep(), which advances neither
     * utime nor stime at all) past a few ticks, then re-gather. */
    burn_cpu_ms(40);
    check("self returns 0 (after CPU burn)", asmspy_procinfo(getpid(), pi) == 0,
          "nonzero");
    check("cpu jiffies (utime+stime) nonzero", pi->utime + pi->stime > 0,
          "a process that has burned CPU should show >0 jiffies (utime+stime)");
    {
        /* elapsed_s = (uptime at gather time) - start_ticks/clk_tck, so it
         * must sit in [0, system uptime): the process started sometime
         * after boot and strictly before this SECOND, LATER, independent
         * read of /proc/uptime (uptime only ever increases). */
        double up_now = -1.0;
        FILE *uf = fopen("/proc/uptime", "r");
        if (uf) {
            if (fscanf(uf, "%lf", &up_now) != 1)
                up_now = -1.0;
            fclose(uf);
        }
        check("elapsed_s non-negative", pi->elapsed_s >= 0.0,
              "elapsed_s must not be negative");
        check("elapsed_s under a later uptime read",
              up_now < 0.0 || pi->elapsed_s < up_now,
              "elapsed_s should be strictly less than system uptime");
    }

    /* --- runtime rides along verbatim -------------------------------- */
    check("elf class", pi->fp.elf_class == 64 || pi->fp.elf_class == 32,
          "no ELF class");

    /* --- containment -------------------------------------------------- */
    check("pid ns read", pi->ns_pid != 0, "no pid namespace id");
    check("self ns matches self", pi->ns_differs == 0,
          "self differs from self");
    check("seccomp known-or-unknown", pi->seccomp >= -1 && pi->seccomp <= 2,
          "seccomp out of range");

    /* --- a cap + its truncation flag, ACTUALLY exercised --------------- */
    /* Fork more live children than ASMSPY_PI_CHILDREN_CAP so the children
     * list is forced to truncate for real, rather than merely checking that
     * the flag defaults to false. Each child just pause()s until reaped
     * below via SIGKILL — harmless, and gone before main() returns. */
    {
        enum { NKIDS = ASMSPY_PI_CHILDREN_CAP + 8 };
        pid_t kids[NKIDS];
        int spawned = 0;
        for (int i = 0; i < NKIDS; i++) {
            pid_t k = fork();
            if (k == 0) {
                pause();
                _exit(0);
            } else if (k > 0) {
                kids[spawned++] = k;
            }
        }
        check("cap-exercise: forked enough children",
              spawned > ASMSPY_PI_CHILDREN_CAP,
              "too few successful forks to exercise the children cap");

        check("self returns 0 (with children)",
              asmspy_procinfo(getpid(), pi) == 0, "nonzero");
        check("children cap truncated", pi->children_truncated != 0,
              "spawned more than the cap but children_truncated is false");
        check("children list sits exactly at the cap",
              pi->n_children == ASMSPY_PI_CHILDREN_CAP,
              "n_children should equal ASMSPY_PI_CHILDREN_CAP once truncated");

        for (int i = 0; i < spawned; i++) {
            kill(kids[i], SIGKILL);
            waitpid(kids[i], NULL, 0);
        }
    }

    /* --- THE contract: a target we could not ptrace still fills in ---- */
    /* Under ptrace_scope=1 our parent is not a descendant of us, so an
     * attach would be refused. A gatherer that quietly ptraced would fail
     * here; one that only reads /proc succeeds.
     *
     * The premise is verified BY CONSTRUCTION, not assumed: we attempt the
     * attach ourselves first. If it unexpectedly succeeds (ptrace_scope=0,
     * CAP_SYS_PTRACE, running as root...), the check below would pass even
     * for a gatherer that quietly ptraced — so it is skipped LOUDLY instead
     * of silently validating nothing. */
    {
        pid_t parent = getppid();
        errno = 0;
        long att = ptrace(PTRACE_ATTACH, parent, NULL, NULL);
        int premise_ok;
        if (att == 0) {
            /* Attach unexpectedly succeeded: the premise does NOT hold on
             * this host. The parent is a real, live process (our shell /
             * make / CI runner) — stop it cleanly, then let it go. */
            int status;
            waitpid(parent, &status, 0);
            ptrace(PTRACE_DETACH, parent, NULL, NULL);
            premise_ok = 0;
            fprintf(stderr,
                    "SKIP parent-premise: PTRACE_ATTACH to our own parent "
                    "SUCCEEDED (ptrace_scope=0, CAP_SYS_PTRACE, or root?) — "
                    "the parent-snapshot check would no longer prove the "
                    "never-ptrace contract on this host, so it is skipped "
                    "rather than passed on a premise that does not hold.\n");
        } else if (errno == EPERM) {
            premise_ok = 1; /* confirmed: an attach really is refused here */
        } else {
            premise_ok = 0;
            fprintf(stderr,
                    "SKIP parent-premise: PTRACE_ATTACH failed with '%s', "
                    "not EPERM — cannot confirm the refusal reason, so the "
                    "parent-snapshot check is skipped rather than trusted.\n",
                    strerror(errno));
        }

        if (premise_ok) {
            check("parent returns 0", asmspy_procinfo(parent, pi) == 0,
                  "nonzero");
            check("parent pid", pi->pid == (long)parent, "pid mismatch");
            check("parent comm", pi->comm[0] != '\0', "no comm");
        } else {
            fprintf(stderr,
                    "SKIP: the parent-snapshot premise did not hold on this "
                    "host, so the never-ptrace contract was NOT exercised "
                    "by this run.\n");
        }
    }

    /* --- a pid that cannot exist is an honest failure, not a blank ---- */
    check("dead pid refused", asmspy_procinfo(0x7ffffff, pi) < 0,
          "a nonexistent pid must fail, not return an empty snapshot");

    free(pi);
    if (failures) {
        fprintf(stderr, "test_procinfo: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_procinfo: all checks passed\n");
    return 0;
}
