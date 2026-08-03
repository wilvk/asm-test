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
 * ncurses — this file makes exactly two raw ptrace(2) calls of its own.
 * The first verifies the never-attach premise against a NON-descendant (our
 * own parent, refused under this host's Yama scope). The second (Task 3, the
 * per-mode capability verdict) constructs a controlled, portable "already
 * traced" FACT about a DESCENDANT instead: a child we fork and
 * PTRACE_ATTACH ourselves, which Yama ptrace_scope<=1 always permits for a
 * descendant regardless of whether the first premise holds. Without it, the
 * verdict's "a refused mode states why" coverage would depend on an
 * incidental host fact (this dev box actually has AMD IBS silicon, so the
 * one gate that does NOT need attach refusal — --sample without IBS — gives
 * no coverage here either, hence that check is separately host-conditioned
 * below). asmspy_procinfo itself is never expected to attach, which is the
 * entire point of the feature.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "asmspy.h"
#include "asmspy_tidsort.h"

static int failures;

static void check(const char *what, int cond, const char *why) {
    if (!cond) {
        fprintf(stderr, "FAIL %s: %s\n", what, why);
        failures++;
    }
}

/* --- worker threads for the per-thread-state section below -------------
 * A single-threaded self snapshot (n_threads_v == 1) cannot exercise the
 * sort, the cap/truncation path, or the "running" why-message, and a lone
 * thread reading its OWN /proc/.../syscall always succeeds, so it can never
 * observe a resolved-but-blocked syscall either. Real worker threads make
 * all of that reachable. */

/* Never enters a syscall, so /proc/<tid>/syscall for THIS tid reads
 * "running in user mode" — the whole point of spawning it.
 *
 * `atomic_int`, not `volatile int`: `volatile` prevents register-caching,
 * it is not a synchronization primitive, and main()'s write here races
 * (per the C11 memory model) with this loop's read of the SAME object —
 * ThreadSanitizer flags it as a genuine data race, reproducibly, even
 * though it is benign in practice on every architecture this project
 * targets (an aligned int, no torn reads). Match this file's own house
 * style for a poll/stop flag (cli/asmspy.c's atomic_bool g_sigstop /
 * atomic_int running): plain atomic_load/atomic_store (the default
 * sequentially-consistent ordering), rather than an explicit relaxed
 * ordering found nowhere else in this codebase. */
static atomic_int g_spin_stop;
static void *spin_worker(void *arg) {
    (void)arg;
    volatile long sink = 0;
    while (!atomic_load(&g_spin_stop))
        sink++;
    return NULL;
}

/* Blocks in read() on an empty pipe, so /proc/<tid>/syscall resolves a REAL
 * in-flight syscall (name "read") — the payoff the header extraction and
 * generator change in this task exist for. */
static void *block_worker(void *arg) {
    int fd = *(int *)arg;
    char c;
    ssize_t n = read(fd, &c, 1); /* unblocked by a write from main() */
    (void)n;
    return NULL;
}

/* Polls a shared stop flag; used only to inflate the live thread count past
 * ASMSPY_PI_THREADS_CAP so the cap + truncation flag are exercised for real,
 * mirroring the children-cap block's forked pause()rs. Same atomic_int
 * reasoning as g_spin_stop above: a plain `volatile int` here is the
 * identical data race, just far less likely to be SAMPLED by a tool like
 * TSan (this loop only reads the flag once per 20ms nanosleep, a much
 * narrower window) — sampling luck, not correctness. */
static atomic_int g_cap_stop;
static void *cap_worker(void *arg) {
    (void)arg;
    struct timespec nap = {0, 20L * 1000 * 1000}; /* 20 ms */
    while (!atomic_load(&g_cap_stop))
        nanosleep(&nap, NULL);
    return NULL;
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
    /* --- asmspy_tidsort_leader_first: pure, so unit-tested here on
     * SYNTHETIC arrays no live /proc listing can produce. Linux hands live
     * tids back already ascending (creation order), so a real snapshot's
     * input to this function is always pre-sorted with the leader already
     * at index 0 — the sort body and the leader-rotate's "not the smallest"
     * branch are BOTH unreachable from any live process short of pid
     * wraparound (measured: disabling the sort entirely produces zero new
     * failures in the live per-thread section further below). These arrays
     * exercise exactly what a live process cannot: unsorted/descending
     * input, the leader mid-array/last/largest, duplicate tids, n<=1, and a
     * leader absent from the array. Each expected result was independently
     * computed by running this exact function against these exact inputs
     * (not hand-derived) before being written here. */
    {
        long a[4] = {10, 20, 30, 40}; /* leader in the middle */
        asmspy_tidsort_leader_first(a, 4, 30);
        check("tidsort: leader in the middle",
              a[0] == 30 && a[1] == 10 && a[2] == 20 && a[3] == 40,
              "expected [30,10,20,40]");
    }
    {
        long a[4] = {30, 10, 40, 20}; /* leader last / numerically largest */
        asmspy_tidsort_leader_first(a, 4, 40);
        check("tidsort: leader last / numerically largest",
              a[0] == 40 && a[1] == 10 && a[2] == 20 && a[3] == 30,
              "expected [40,10,20,30]");
    }
    {
        long a[5] = {50, 40, 30, 20, 10}; /* descending input */
        asmspy_tidsort_leader_first(a, 5, 20);
        check("tidsort: descending input",
              a[0] == 20 && a[1] == 10 && a[2] == 30 && a[3] == 40 &&
                  a[4] == 50,
              "expected [20,10,30,40,50]");
    }
    {
        long a[4] = {20, 10, 20, 30}; /* duplicate tids */
        asmspy_tidsort_leader_first(a, 4, 20);
        check("tidsort: duplicate tids",
              a[0] == 20 && a[1] == 10 && a[2] == 20 && a[3] == 30,
              "expected [20,10,20,30]");
    }
    {
        long a[1] = {777}; /* n == 0: must not touch the array at all */
        asmspy_tidsort_leader_first(a, 0, 777);
        check("tidsort: n == 0 is a no-op", a[0] == 777,
              "n==0 must not touch the array");
    }
    {
        long a[1] = {42}; /* n == 1, leader present */
        asmspy_tidsort_leader_first(a, 1, 42);
        check("tidsort: n == 1", a[0] == 42, "expected [42]");
    }
    {
        long a[3] = {30, 10, 20}; /* leader absent from the array */
        asmspy_tidsort_leader_first(a, 3, 999);
        check("tidsort: leader absent leaves the sort intact",
              a[0] == 10 && a[1] == 20 && a[2] == 30,
              "expected [10,20,30] (sorted, un-rotated)");
    }

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
     * of silently validating nothing.
     *
     * `parent`/`premise_ok` live at FUNCTION scope, not just this block: the
     * per-thread section below reuses this same, already-confirmed premise
     * to assert the parent's syscall_why specifically names ptrace
     * permission, rather than making a second real ptrace(2) call — this
     * file's own header promises exactly one. */
    pid_t parent = getppid();
    int premise_ok;
    {
        errno = 0;
        long att = ptrace(PTRACE_ATTACH, parent, NULL, NULL);
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

    /* --- threads: the attach-free "what is it doing now" ------------- */
    check("self reread", asmspy_procinfo(getpid(), pi) == 0, "nonzero");
    check("thread rows", pi->n_threads_v >= 1, "no thread rows");
    check("row 0 is the leader", pi->threads_v[0].tid == (long)getpid(),
          "leader is not first");
    check("row 0 comm", pi->threads_v[0].comm[0] != '\0', "no comm");
    check("row 0 state",
          pi->threads_v[0].state == 'R' || pi->threads_v[0].state == 'S',
          "odd thread state");
    check("rows within cap", pi->n_threads_v <= ASMSPY_PI_THREADS_CAP,
          "cap exceeded");
    check("truncation is stated",
          pi->threads_truncated == (pi->threads > ASMSPY_PI_THREADS_CAP),
          "truncated flag disagrees with the thread count");

    /* An absent syscall row must always carry its reason: a blank cell is
     * indistinguishable from "it is doing nothing", which is never true.
     * WEAK here on purpose: this process is single-threaded at this point,
     * and a task can always read its OWN /proc/.../syscall, so have_syscall
     * == 1 for every row and the `||` never actually inspects syscall_why.
     * The parent-scoped loop further below is the one that can fail. */
    for (int i = 0; i < pi->n_threads_v; i++)
        check("absent syscall states why",
              pi->threads_v[i].have_syscall || pi->threads_v[i].syscall_why[0],
              "have_syscall == 0 with an empty syscall_why");

    /* --- threads: sort order, syscall_name resolution, and the "running"
     * why-message — all unreachable with n_threads_v == 1. One worker spins
     * (never enters a syscall, so its OWN /proc/.../syscall reads "running
     * in user mode"); one blocks in read() on an empty pipe (so its
     * /proc/.../syscall resolves a REAL in-flight syscall) — the entire
     * payoff of the header extraction + generator change in this task. --- */
    {
        int pfd[2];
        if (pipe(pfd) != 0) {
            check("pipe() for the blocked worker", 0, strerror(errno));
        } else {
            atomic_store(&g_spin_stop, 0);
            pthread_t spin_t, block_t;
            int have_spin =
                (pthread_create(&spin_t, NULL, spin_worker, NULL) == 0);
            int have_block =
                (pthread_create(&block_t, NULL, block_worker, &pfd[0]) == 0);
            check("spin worker started", have_spin, "pthread_create failed");
            check("block worker started", have_block, "pthread_create failed");

            /* let the block worker actually reach read() before sampling */
            struct timespec settle = {0, 20L * 1000 * 1000}; /* 20 ms */
            nanosleep(&settle, NULL);

            check("self reread (with workers)",
                  asmspy_procinfo(getpid(), pi) == 0, "nonzero");
            check("thread rows include the workers", pi->n_threads_v >= 3,
                  "expected the leader plus at least 2 workers");
            check("row 0 is still the leader",
                  pi->threads_v[0].tid == (long)getpid(),
                  "leader is not first");

            /* sort: strictly ascending by tid for every row after the leader.
             * INTEGRATION sanity check only, not the sort's real coverage:
             * Linux's readdir("/proc/<pid>/task") already hands live tids
             * back in creation order (ascending), so this passes whether or
             * not asmspy_tidsort_leader_first's sort actually runs — a
             * disabled sort produces zero new failures here (measured). The
             * synthetic-array tests below are what actually exercise the
             * sort and the leader-rotate on inputs a live process cannot
             * produce (unsorted/descending, leader not-smallest, ...). */
            int sorted = 1;
            for (int i = 2; i < pi->n_threads_v; i++)
                if (pi->threads_v[i - 1].tid >= pi->threads_v[i].tid)
                    sorted = 0;
            check("rows after the leader are ascending by tid", sorted,
                  "threads_v[1..] is not strictly ascending by tid");

            int saw_running = 0, saw_named_syscall = 0;
            for (int i = 1; i < pi->n_threads_v; i++) {
                if (!pi->threads_v[i].have_syscall &&
                    !strcmp(pi->threads_v[i].syscall_why,
                            "running in user mode"))
                    saw_running = 1;
                if (pi->threads_v[i].have_syscall &&
                    !strcmp(pi->threads_v[i].syscall_name, "read"))
                    saw_named_syscall = 1;
            }
            check("a spinning worker reads \"running in user mode\"",
                  saw_running, "no worker row showed the running why-message");
            check("a blocked worker resolves its syscall_name",
                  saw_named_syscall,
                  "no worker row resolved have_syscall with "
                  "syscall_name==\"read\"");

            atomic_store(&g_spin_stop, 1);
            char one = 'x';
            ssize_t wn = write(pfd[1], &one, 1); /* unblock the read() */
            (void)wn;
            if (have_spin)
                pthread_join(spin_t, NULL);
            if (have_block)
                pthread_join(block_t, NULL);
            close(pfd[0]);
            close(pfd[1]);
        }
    }

    /* --- threads: the cap + its truncation flag, ACTUALLY exercised ---
     * Mirrors the children-cap block above: spawn more LIVE threads than
     * ASMSPY_PI_THREADS_CAP so the thread list is forced to truncate for
     * real, rather than merely checking that the flag defaults to false.
     * Each just polls a shared stop flag — harmless, gone before main()
     * returns. */
    {
        enum { NTHREADS = ASMSPY_PI_THREADS_CAP + 8 };
        pthread_t thr[NTHREADS];
        int spawned = 0;
        atomic_store(&g_cap_stop, 0);
        for (int i = 0; i < NTHREADS; i++) {
            if (pthread_create(&thr[i], NULL, cap_worker, NULL) != 0)
                break;
            spawned++;
        }
        check("cap-exercise: spawned enough threads",
              spawned > ASMSPY_PI_THREADS_CAP,
              "too few successful pthread_creates to exercise the threads cap");

        check("self returns 0 (with many threads)",
              asmspy_procinfo(getpid(), pi) == 0, "nonzero");
        check("threads cap truncated", pi->threads_truncated != 0,
              "spawned more than the cap but threads_truncated is false");
        check("thread rows sit exactly at the cap",
              pi->n_threads_v == ASMSPY_PI_THREADS_CAP,
              "n_threads_v should equal ASMSPY_PI_THREADS_CAP once truncated");

        atomic_store(&g_cap_stop, 1);
        for (int i = 0; i < spawned; i++)
            pthread_join(thr[i], NULL);
    }

    /* wchan on a SLEEPING task names the kernel function it sleeps in.
     * Our parent (a shell awaiting us) is reliably sleeping; a running
     * task correctly has no wchan, so only assert on a sleeper. */
    check("parent reread", asmspy_procinfo(getppid(), pi) == 0, "nonzero");
    for (int i = 0; i < pi->n_threads_v; i++)
        if (pi->threads_v[i].state == 'S')
            check("sleeping thread names its wchan",
                  pi->threads_v[i].wchan[0] != '\0',
                  "empty wchan on a sleeper");

    /* The SAME invariant as above, but exercised where have_syscall really
     * CAN be 0: `premise_ok` (established once, above) confirms an attach to
     * our parent is genuinely refused, so its syscall read is denied too —
     * this is precisely the shape C1 got wrong. The file is mode 0400, so a
     * same-uid non-descendant's read fails at fgets()/EPERM, not at fopen()
     * (ptrace_may_access is checked at READ) — a regression that reports
     * that as "empty" instead of naming the permission reason fails here. */
    if (premise_ok) {
        int saw_denied = 0;
        for (int i = 0; i < pi->n_threads_v; i++) {
            check("parent: absent syscall states why",
                  pi->threads_v[i].have_syscall ||
                      pi->threads_v[i].syscall_why[0],
                  "have_syscall == 0 with an empty syscall_why");
            if (!pi->threads_v[i].have_syscall) {
                saw_denied = 1;
                check("parent: denial names ptrace permission, not \"empty\"",
                      strstr(pi->threads_v[i].syscall_why,
                             "ptrace permission") != NULL,
                      pi->threads_v[i].syscall_why);
            }
        }
        check("parent: the denied path was actually exercised", saw_denied,
              "expected have_syscall == 0 for a parent thread under a "
              "confirmed ptrace-refusal premise");
    } else {
        for (int i = 0; i < pi->n_threads_v; i++)
            check("parent: absent syscall states why",
                  pi->threads_v[i].have_syscall ||
                      pi->threads_v[i].syscall_why[0],
                  "have_syscall == 0 with an empty syscall_why");
    }

    /* --- code surface + modules -------------------------------------- */
    check("self reread 2", asmspy_procinfo(getpid(), pi) == 0, "nonzero");
    check("symbols found", pi->syms_total > 0, "no symbols in our own image");
    check("modules found", pi->n_modules > 0, "no modules");
    check("modules within cap", pi->n_modules <= ASMSPY_PI_MODULES_CAP,
          "cap exceeded");
    check("module 0 named", pi->modules[0].name[0] != '\0', "unnamed module");
    check("modules ranked by symbol count",
          pi->n_modules < 2 || pi->modules[0].syms >= pi->modules[1].syms,
          "modules are not symbol-count-descending");
    check("no JIT here", pi->jit_methods == 0, "a static C test has no JIT");

    /* Per-module symbol counts must be genuinely PER MODULE: a counter that
     * stamps the same value into every module (all-equal) or never
     * increments at all (all-zero) would still satisfy the
     * ranked-descending check above (N >= N throughout, or 0 >= 0
     * throughout) — this is the exact shape of bug the brief warns a
     * per-module scan must not hide. Our own image reliably links at least
     * libc (many exported symbols) alongside a module or two with far fewer
     * (the executable itself, the dynamic linker), so real distinctness is
     * expected here, not just plausible. `size` is checked too: it is the
     * one field this task must POPULATE that the brief's own snippet never
     * exercises. */
    {
        int any_nonzero = 0, all_same = 1;
        unsigned long first = pi->modules[0].syms;
        unsigned long long any_size = 0;
        for (int i = 0; i < pi->n_modules; i++) {
            if (pi->modules[i].syms > 0)
                any_nonzero = 1;
            if (pi->modules[i].syms != first)
                all_same = 0;
            any_size += pi->modules[i].size;
        }
        check("per-module symbol counts are not all zero", any_nonzero,
              "every module reported zero symbols");
        check("per-module symbol counts are not all identical",
              pi->n_modules < 2 || !all_same,
              "every module reported the identical symbol count (looks like "
              "a broken per-module counter)");
        check("module sizes are populated", any_size > 0,
              "every module reported a zero mapped size");
    }

    /* --- pc_sym: a thread's pc resolves to a real function name ---------
     * pc_sym is gated on have_syscall (pc itself is only known from
     * /proc/<tid>/syscall, which reports NOTHING but the literal string
     * "running" outside a syscall — see pi_read_code_and_modules), so the
     * one thread we can reliably put INSIDE a syscall (blocked in read() on
     * an empty pipe, as above) is also the one whose pc_sym resolution is
     * actually checkable. A fresh worker is spawned here rather than reused
     * from the earlier block, which has already been joined and its pipe
     * closed. */
    {
        int pfd[2];
        if (pipe(pfd) != 0) {
            check("pipe() for the pc_sym worker", 0, strerror(errno));
        } else {
            pthread_t block_t;
            int have_block =
                (pthread_create(&block_t, NULL, block_worker, &pfd[0]) == 0);
            check("pc_sym worker started", have_block, "pthread_create failed");

            struct timespec settle = {0, 20L * 1000 * 1000}; /* 20 ms */
            nanosleep(&settle, NULL);

            check("self reread (for pc_sym)",
                  asmspy_procinfo(getpid(), pi) == 0, "nonzero");

            int saw_resolved_pc = 0;
            for (int i = 0; i < pi->n_threads_v; i++)
                if (pi->threads_v[i].have_syscall &&
                    !strcmp(pi->threads_v[i].syscall_name, "read") &&
                    pi->threads_v[i].pc_sym[0] != '\0')
                    saw_resolved_pc = 1;
            check("a thread blocked in a real syscall resolves pc_sym",
                  saw_resolved_pc,
                  "expected a have_syscall==1 \"read\" row with a non-empty "
                  "pc_sym");

            char one = 'x';
            ssize_t wn = write(pfd[1], &one, 1); /* unblock the read() */
            (void)wn;
            if (have_block)
                pthread_join(block_t, NULL);
            close(pfd[0]);
            close(pfd[1]);
        }
    }

    /* --- module cap + truncation, ACTUALLY exercised ---------------------
     * Mirrors the children/threads cap blocks above: force real truncation
     * instead of merely checking the flag defaults to false. scan_modules
     * (and so pi_read_code_and_modules) counts any DISTINCT absolute path
     * mapped at file offset 0 — it does not require the file to be a valid
     * ELF, only that the kernel shows it as an ordinary file-backed mapping
     * — so a pile of throwaway regular files, mmap'd PROT_READ at offset 0
     * under distinct paths, count as real, distinct "modules" without
     * needing 64+ real shared libraries to be installed on whatever host
     * runs this test. */
    {
        char dir[] = "/tmp/asmspy_pi_modcap_XXXXXX";
        char *tmpdir = mkdtemp(dir);
        check("modcap: scratch dir created", tmpdir != NULL, strerror(errno));
        if (tmpdir) {
            enum { NDUMMY = ASMSPY_PI_MODULES_CAP + 8 };
            int fds[NDUMMY];
            void *maps[NDUMMY];
            int nmapped = 0;
            for (int i = 0; i < NDUMMY; i++) {
                char path[128];
                snprintf(path, sizeof path, "%s/m%d.bin", tmpdir, i);
                int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
                if (fd < 0)
                    continue;
                if (ftruncate(fd, 4096) != 0) {
                    close(fd);
                    unlink(path);
                    continue;
                }
                void *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
                if (m == MAP_FAILED) {
                    close(fd);
                    unlink(path);
                    continue;
                }
                fds[nmapped] = fd;
                maps[nmapped] = m;
                nmapped++;
            }
            check("modcap: mapped enough dummy files",
                  nmapped > ASMSPY_PI_MODULES_CAP,
                  "too few successful dummy mmaps to exercise the module "
                  "cap");

            check("self reread (with dummy modules)",
                  asmspy_procinfo(getpid(), pi) == 0, "nonzero");
            check("module cap truncated", pi->modules_truncated != 0,
                  "mapped more than the cap but modules_truncated is false");
            check("module list sits exactly at the cap",
                  pi->n_modules == ASMSPY_PI_MODULES_CAP,
                  "n_modules should equal ASMSPY_PI_MODULES_CAP once "
                  "truncated");

            for (int i = 0; i < nmapped; i++) {
                char path[128];
                snprintf(path, sizeof path, "%s/m%d.bin", tmpdir, i);
                munmap(maps[i], 4096);
                close(fds[i]);
                unlink(path);
            }
            rmdir(tmpdir);
        }
    }

    /* --- the capability verdict -------------------------------------- */
    /* Our own pid: an engine cannot trace its own tracer thread, but the
     * verdict is about the TARGET's facts, and we are a native 64-bit
     * process nothing else traces — so the portable modes are available. */
    check("attachable known",
          pi->attachable == 1 || pi->attachable == 0 || pi->attachable == -1,
          "verdict out of range");
    check("why never empty", pi->attach_why[0] != '\0', "empty why");
    for (int m = 0; m < ASMSPY_MODE__COUNT; m++)
        check("refused mode states why",
              pi->mode_ok[m] || pi->mode_why[m][0] != '\0',
              "a refused mode with an empty reason");
    check("mode names", strcmp(asmspy_mode_name(ASMSPY_MODE_LOG), "log") == 0,
          "mode name mismatch");
    check("dataflow ok on a native 64-bit target",
          pi->fp.elf_class != 64 || pi->mode_ok[ASMSPY_MODE_DATAFLOW],
          "dataflow refused on a 64-bit native target");

    /* --- a refused mode ACTUALLY carries its reason ----------------------
     * On THIS test process every mode is normally OK (native, same-uid,
     * nothing else traces it), so the generic "refused mode states why"
     * loop just above never actually inspects a non-empty mode_why — it
     * short-circuits on mode_ok[m] every time, exactly the tautology shape
     * the brief warns a first draft is likely to have. Force a REAL
     * refusal instead of trusting an incidental host fact: fork a child and
     * PTRACE_ATTACH it ourselves (this file's SECOND and last raw ptrace(2)
     * call — see the file-header comment). Yama ptrace_scope<=1 always
     * permits attaching to one's own DESCENDANT, so this does not depend on
     * the non-descendant premise the first ptrace call tests, and is
     * expected to succeed on any host this project's other tests run on. */
    {
        pid_t vchild = fork();
        if (vchild == 0) {
            pause();
            _exit(0);
        } else if (vchild < 0) {
            check("fork for the verdict-forcing child", 0, strerror(errno));
        } else {
            errno = 0;
            long att = ptrace(PTRACE_ATTACH, vchild, NULL, NULL);
            if (att != 0) {
                fprintf(stderr,
                        "SKIP verdict-forcing: PTRACE_ATTACH to our own "
                        "child failed with '%s' — the forced-refusal check "
                        "is skipped rather than trusted on a host this "
                        "locked down.\n",
                        strerror(errno));
            } else {
                int status;
                waitpid(vchild, &status, 0); /* reap the attach-stop */

                check("victim reread", asmspy_procinfo(vchild, pi) == 0,
                      "nonzero");
                check("already-traced target reports attachable == 0",
                      pi->attachable == 0,
                      "expected attachable==0 for an already-traced target");
                check("already-traced target's why names the tracer",
                      strstr(pi->attach_why, "already traced") != NULL,
                      pi->attach_why);

                int any_ok = 0, any_missing_reason = 0;
                for (int m = 0; m < ASMSPY_MODE__COUNT; m++) {
                    if (pi->mode_ok[m])
                        any_ok = 1;
                    if (!pi->mode_ok[m] && !pi->mode_why[m][0])
                        any_missing_reason = 1;
                }
                check("every mode is refused on an already-traced target",
                      !any_ok, "expected every mode refused (attachable==0)");
                check("every refused mode states why", !any_missing_reason,
                      "a refused mode on the forced-victim target has an "
                      "empty reason");

                ptrace(PTRACE_DETACH, vchild, NULL, NULL);
            }
            kill(vchild, SIGKILL);
            waitpid(vchild, NULL, 0);
        }
    }

    /* The SAMPLE mode's OWN gate (AMD IBS silicon — a HOST fact, checked
     * independently of the attach verdict): assert its shape only when we
     * can predict which side of the gate this host is on, by checking the
     * exact same condition pi_verdict itself checks, rather than assuming
     * one way or the other. */
    {
        check("self reread (for SAMPLE gate)",
              asmspy_procinfo(getpid(), pi) == 0, "nonzero");
        int have_ibs = (access("/sys/devices/ibs_op", F_OK) == 0);
        if (!have_ibs)
            check("SAMPLE mode refused without IBS states why",
                  !pi->mode_ok[ASMSPY_MODE_SAMPLE] &&
                      pi->mode_why[ASMSPY_MODE_SAMPLE][0] != '\0',
                  "SAMPLE should be refused (no ibs_op PMU) with a reason");
        else
            fprintf(stderr,
                    "SKIP: host has an ibs_op PMU — SAMPLE mode's no-IBS "
                    "refusal path isn't exercised by this check.\n");
    }

    free(pi);
    if (failures) {
        fprintf(stderr, "test_procinfo: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_procinfo: all checks passed\n");
    return 0;
}
