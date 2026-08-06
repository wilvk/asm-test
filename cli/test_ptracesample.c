/* test_ptracesample.c — the lane for the PERF-FREE region picker
 * (cli/asmspy_ptracesample.c: asmspy_ptrace_sample).
 *
 * WHY THIS TEST EXISTS AT ALL. `asmspy --dataflow <pid> --auto` picks its region
 * with an out-of-band sampler, and both of the samplers that existed before this
 * one reach the same perf_event_open (src/ibs_backend.c:369). On a stock Ubuntu
 * box — kernel.perf_event_paranoid = 4, a compiled-in default no file sets —
 * that syscall is refused, so `--auto` records ZERO events and `--sampler=sw` is
 * not an escape hatch. Everything AFTER the picker is already perf-free: given an
 * explicit `func`, the same host emits codeimage + df_step + regstate + df_edge.
 * The gate is exclusively in the picker, and this module is the picker that has
 * no gate.
 *
 * The checks below are not a tour of the implementation; each one is a defect a
 * prototype of this module actually shipped, that measurement caught:
 *
 *   §1  Capstone gates the call-target expansion, and phase 2 must SAY so
 *       rather than silently degrading to a residency ranking;
 *   §2  three phases, because residency alone is measurably WRONG (394:5 the
 *       wrong way on auto_victim);
 *   §3  signals must be RE-INJECTED, because an unconditional PTRACE_CONT(sig=0)
 *       destroyed 89% of a target's SIGALRMs — and did so INVISIBLY against a
 *       signal-free spinner;
 *   §4  new threads must be traced, because an unseized thread reaching the
 *       shared int3 takes a SIGTRAP with no tracer and KILLS the user's process.
 *
 * Spawns its own victims. Built + run by `make cli-smoke` (mk/cli.mk,
 * cli/cli_smoke.sh).
 *
 * Usage: test_ptracesample [build-dir]      (default "build")
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "asmspy_ptracesample.h"
#include "libasmspy.h"

static int failures = 0;
static int checks = 0;
#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            printf("not ok - %s\n", (msg));                                    \
            failures++;                                                        \
        } else                                                                 \
            printf("ok - %s\n", (msg));                                        \
    } while (0)

/* ------------------------------------------------------------------------- */
/* victim plumbing                                                            */
/* ------------------------------------------------------------------------- */

/* Spawn `path` and return its pid, or -1. The child's stderr is piped so we can
 * WAIT for its "<name> pid=" banner: every victim here prints that line AFTER
 * its prctl(PR_SET_PTRACER_ANY), so reading it removes the attach race that a
 * fixed sleep would only paper over (the test_libasmspy idiom).
 *
 * When `out_fd` is non-NULL the child's STDOUT is piped too and the read end is
 * handed back non-blocking — that is the sigload victim's tick channel. */
static pid_t spawn_victim(const char *path, const char *banner, int *out_fd) {
    int epipe[2], opipe[2] = {-1, -1};
    if (pipe(epipe) != 0)
        return -1;
    if (out_fd && pipe(opipe) != 0) {
        close(epipe[0]);
        close(epipe[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(epipe[0]);
        dup2(epipe[1], 2);
        close(epipe[1]);
        if (opipe[1] >= 0) {
            close(opipe[0]);
            dup2(opipe[1], 1);
            close(opipe[1]);
        }
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    close(epipe[1]);
    if (opipe[1] >= 0)
        close(opipe[1]);

    char buf[256];
    ssize_t n = read(epipe[0], buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    /* The read end stays OPEN for the victim's lifetime, and that is a fix, not
     * a leak. Closing it after the banner used to kill any victim that printed
     * to stderr TWICE: tid_victim's two workers each announce themselves from
     * their own thread, so whenever the second line was written after our close
     * it took SIGPIPE and the whole process died BEFORE the sampler ever
     * attached — which then surfaced as "no resolvable function symbols" and a
     * dead victim, i.e. as a sampler bug. MEASURED at 2-3 runs in 10. A handful
     * of fds held until this short-lived test exits costs nothing. */
    if (n <= 0 || strstr(buf, banner) == NULL) {
        close(epipe[0]);
        int st;
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
        if (opipe[0] >= 0)
            close(opipe[0]);
        return -1;
    }
    if (out_fd) {
        fcntl(opipe[0], F_SETFL, O_NONBLOCK);
        *out_fd = opipe[0];
    }
    return pid;
}

static void kill_victim(pid_t pid, int fd) {
    int st;
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    if (fd >= 0)
        close(fd);
}

static long mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Count the victim's SIGALRM ticks over `ms`, by reading the `ticks=<n>` lines
 * it prints at ~20 Hz, and returning last - first.
 *
 * poll()-driven with a real monotonic deadline rather than the obvious blocking
 * fgets loop, and that is deliberate: the defect this measurement exists to
 * catch (a tracer that swallows the target's signals) can also FREEZE the
 * target, and a blocking read against a frozen victim would HANG the test
 * instead of failing it. Here a frozen victim yields no lines and scores 0.
 *
 * Fewer than two lines => 0, which is the correct reading either way: no
 * observed forward progress. */
static long sigload_ticks_over(int fd, int ms) {
    char acc[512];
    size_t used = 0;
    long first = -1, last = -1;
    long deadline = mono_ms() + ms;
    for (;;) {
        long left = deadline - mono_ms();
        if (left <= 0)
            break;
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, (int)left);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break; /* timeout, or the pipe broke: report what we have */
        }
        char buf[256];
        ssize_t got = read(fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; i++) {
            if (buf[i] != '\n') {
                if (used + 1 < sizeof acc)
                    acc[used++] = buf[i];
                continue;
            }
            acc[used] = '\0';
            used = 0;
            long v = 0;
            if (sscanf(acc, "ticks=%ld", &v) == 1) {
                if (first < 0)
                    first = v;
                last = v;
            }
        }
    }
    return (first < 0 || last < 0) ? 0 : last - first;
}

/* Count whole lines arriving on `fd` over `ms`. Distinct from
 * sigload_ticks_over's first/last DELTA on purpose: forkhot_victim's children
 * each print one line carrying a counter, and a child killed by an inherited
 * trap prints NOTHING — leaving a hole in the sequence that a delta would count
 * as if the child had lived. Lines are the survivors. */
static long count_lines_over(int fd, int ms) {
    long lines = 0, deadline = mono_ms() + ms;
    for (;;) {
        long left = deadline - mono_ms();
        if (left <= 0)
            break;
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, (int)left);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        char buf[512];
        ssize_t got = read(fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; i++)
            if (buf[i] == '\n')
                lines++;
    }
    return lines;
}
/* Count forkhot_victim's children over `ms`, and how many are MISSING.
 *
 * A RATE comparison cannot do this job. forkhot_victim re-bases its cadence from
 * the measured clock, so the fork rate drifts under load, and a band wide enough
 * to absorb that drift (97% of 240 is 7 forks) is barely wider than the defect
 * it must catch (10 of 240). Widen it and the check goes hollow again — the
 * original 60% band tolerated 40% child mortality.
 *
 * So count the HOLES instead. Every child prints `kid=<seq>` carrying the
 * parent's fork counter, so the sequence is contiguous by construction and a
 * child killed by an inherited trap leaves a gap. `missing` is
 * (last - first + 1) - seen, which is invariant under any change of RATE at all:
 * a slower victim forks fewer children, not fewer of the ones it forked. */
static void forkhot_gaps_over(int fd, int ms, long *seen, long *missing) {
    char acc[512];
    size_t used = 0;
    long first = -1, last = -1, count = 0;
    long deadline = mono_ms() + ms;
    for (;;) {
        long left = deadline - mono_ms();
        if (left <= 0)
            break;
        struct pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, (int)left);
        if (r <= 0) {
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        char buf[512];
        ssize_t got = read(fd, buf, sizeof buf);
        if (got <= 0)
            break;
        for (ssize_t i = 0; i < got; i++) {
            if (buf[i] != '\n') {
                if (used + 1 < sizeof acc)
                    acc[used++] = buf[i];
                continue;
            }
            acc[used] = '\0';
            used = 0;
            long v = 0;
            if (sscanf(acc, "kid=%ld", &v) != 1)
                continue;
            if (first < 0)
                first = v;
            last = v;
            count++;
        }
    }
    *seen = count;
    *missing = (first < 0) ? 0 : (last - first + 1) - count;
}

/* The sampler on its own thread, so a measurement can run WHILE it is attached.
 * That is not a convenience: the prototype's signal damage was TRANSIENT — the
 * victim recovered its full rate the instant the tracer detached — so a
 * before/after comparison bracketing the call cannot see it at all. */
typedef struct {
    pid_t pid;
    const asmspy_symtab_t *syms;
    const char *module;
    asmspy_autocand_t *out;
    int max, window_ms;
    char why[192];
    int rc;
} job_t;

static void *run_sampler(void *p) {
    job_t *j = (job_t *)p;
    j->why[0] = '\0';
    j->rc = asmspy_ptrace_sample(j->pid, j->syms, j->module, j->out, j->max,
                                 j->window_ms, j->why, sizeof j->why);
    return NULL;
}

/* Does `name` appear anywhere in the ranked list? */
static int listed(const asmspy_autocand_t *c, int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (c[i].name && strcmp(c[i].name, name) == 0)
            return 1;
    return 0;
}

/* One numeric field out of /proc/<tid>/status ("Name:\tvalue"), or -1. */
static int status_field(pid_t tid, const char *key) {
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/status", (int)tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t klen = strlen(key);
    int v = -1;
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, klen) == 0 && sscanf(line + klen, "%d", &v) == 1)
            break;
    fclose(f);
    return v;
}

/* Is `tid` traced by THIS PROCESS?
 *
 * Not `TracerPid == getpid()`: /proc prints task_pid_nr_ns(tracer), which is the
 * tracer THREAD's tid, not its tgid. The sampler runs on a worker thread here
 * (it has to — see run_sampler), so a getpid() comparison reads 0-for-untraced
 * against a perfectly good tracer and would have failed this check for the
 * wrong reason. Resolve the tracer's own Tgid instead. */
static int traced_by_us(pid_t tid) {
    int tp = status_field(tid, "TracerPid:");
    if (tp <= 0)
        return 0;
    return status_field((pid_t)tp, "Tgid:") == (int)getpid();
}

/* Is `pid` ALIVE — not merely present?
 *
 * `kill(pid, 0) == 0` is not a liveness test: it succeeds on a ZOMBIE, and a
 * victim killed by an orphaned int3 is exactly that until someone reaps it.
 * MEASURED — with the trap-restore disabled, every auto_victim check still
 * passed while the process was a corpse. /proc/<pid>/stat's state field is the
 * real answer: 'Z' is dead-and-unreaped, 'X' is exiting. */
static char proc_state(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return '?';
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *p = strrchr(buf, ')');
    return (p && p[1] == ' ') ? p[2] : '?';
}

static int is_alive(pid_t pid) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* the comm field is parenthesised and may contain spaces/parens */
    char *p = strrchr(buf, ')');
    if (!p || p[1] != ' ')
        return 0;
    return p[2] != 'Z' && p[2] != 'X';
}

/* THE CHECK THE MODULE'S OWN COMMENTS CALL FATAL, asked directly.
 *
 * `kill(pid, 0) == 0` only says the process existed at that instant, and an
 * orphaned int3 kills on the target's NEXT ARRIVAL — microseconds later, but
 * after the assertion has already passed. So read the byte back out of the
 * target's text: 0xcc at a region entry we armed means the sampler returned
 * success while leaving a trap that will kill this process.
 *
 * Reads through process_vm_readv, which needs no ptrace attach at all (the same
 * /proc access asmspy_symtab_load already used), so it cannot itself perturb
 * what it measures. Returns 1 if the first byte at `addr` is an int3. */
static int has_trap_byte(pid_t pid, uint64_t addr) {
    unsigned char b = 0;
    struct iovec l = {&b, 1};
    struct iovec r = {(void *)(uintptr_t)addr, 1};
    if (process_vm_readv(pid, &l, 1, &r, 1, 0) != 1)
        return 0; /* unreadable: cannot claim a trap is there */
#if defined(__aarch64__)
    unsigned char w[4];
    struct iovec l4 = {w, 4};
    struct iovec r4 = {(void *)(uintptr_t)addr, 4};
    if (process_vm_readv(pid, &l4, 1, &r4, 1, 0) != 4)
        return 0;
    return w[0] == 0x00 && w[1] == 0x00 && w[2] == 0x20 && w[3] == 0xd4;
#else
    return b == 0xcc;
#endif
}

/* The tids of `pid` right now, into `out` (capacity `cap`). Returns the count. */
static int task_tids(pid_t pid, pid_t *out, int cap) {
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%d/task", (int)pid);
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        out[n++] = (pid_t)atoi(e->d_name);
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    const char *bdir = argc > 1 ? argv[1] : "build";
    char p_auto[256], p_sig[256], p_clone[256];
    snprintf(p_auto, sizeof p_auto, "%s/auto_victim", bdir);
    snprintf(p_sig, sizeof p_sig, "%s/sigload_victim", bdir);
    snprintf(p_clone, sizeof p_clone, "%s/clone_victim", bdir);

    /* --------------------------------------------------------------------- */
    /* §1  The Capstone gate, as a pure decision (correction 5).              */
    /*                                                                       */
    /* asmtest_disas_call_target is #ifdef ASMTEST_HAVE_CAPSTONE and returns  */
    /* 0 SILENTLY without it — indistinguishable, from phase 2's side, from   */
    /* "this function makes no direct calls". A build without Capstone would  */
    /* therefore degrade to a pure residency ranking, which is the ONE answer */
    /* the three-phase design exists to reject, and it would do it quietly.   */
    /* The disposition is a pure function precisely so BOTH branches are      */
    /* pinned here, on a host that has Capstone and can never take the other. */
    /* --------------------------------------------------------------------- */
    CHECK(asmspy_ps_expand_note(1) == NULL,
          "with a disassembler, phase 2 has nothing to report");
    CHECK(asmspy_ps_expand_note(0) != NULL &&
              strstr(asmspy_ps_expand_note(0), "Capstone") != NULL,
          "without a disassembler, phase 2 NAMES Capstone rather than "
          "degrading silently to a residency ranking");

    /* --------------------------------------------------------------------- */
    /* §2  THE test, and the whole reason this sampler has three phases.      */
    /*                                                                       */
    /* auto_victim's shape is built so residency and entry DISAGREE:          */
    /* grind_forever is entered ONCE and burns all the time, while            */
    /* entered_often is called from its inner loop. A residency ranking picks */
    /* grind_forever — measured 394:5 — and feeding that to the capture arms  */
    /* an int3 at an entry that can never be reached again, so the capture    */
    /* HANGS.                                                                 */
    /* --------------------------------------------------------------------- */
    {
        pid_t vpid = spawn_victim(p_auto, "auto_victim pid=", NULL);
        if (vpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_auto);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(vpid, &syms) != 0 || syms.n == 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_auto);
            kill_victim(vpid, -1);
            return 1;
        }

        asmspy_autocand_t cands[8];
        char why[192] = "";
        int n = asmspy_ptrace_sample(vpid, &syms, NULL, cands, 8, 400, why,
                                     sizeof why);
        printf("# auto_victim: rc=%d why='%s'\n", n, why);
        for (int i = 0; i < n && i < 8; i++)
            printf("#   [%d] %-20s arrivals=%llu first=%lluus sites=%u "
                   "size=%llu\n",
                   i, cands[i].name ? cands[i].name : "?", cands[i].arrivals,
                   cands[i].first_us, cands[i].sites,
                   (unsigned long long)cands[i].size);

        CHECK(n > 0, "the sampler observed something in its window");
        CHECK(n > 0 && cands[0].name &&
                  strcmp(cands[0].name, "entered_often") == 0,
              "the winner must be the function that is ARRIVED AT, not the one "
              "time is spent in -- residency alone ranks grind_forever");
        CHECK(n > 0 && cands[0].size > 0,
              "a zero-sized candidate is the vacuity trap "
              "(asmspy_autoregion.h)");
        /* Phase 3 is a CONFIRMATION, not a re-ordering: a candidate whose entry
         * was never reached must be DROPPED, because arming it is exactly the
         * hang this whole module exists to avoid. grind_forever is the strongest
         * possible case — it wins phase 1 outright and can never be re-entered. */
        CHECK(n > 0 && !listed(cands, n, "grind_forever"),
              "grind_forever wins the residency phase and can never be entered "
              "again, so phase 3 must DROP it, not merely rank it lower");
        CHECK(!listed(cands, n, "quiet_helper"),
              "quiet_helper is never called: a picker must not name a function "
              "it did not observe");
        /* first_us is phase 3's ranking key (correction 4: under a per-candidate
         * hit budget the COUNTS saturate and tie — measured tiny_callee 50 vs
         * libc sched_yield 50 — and time-to-first-arrival separates them
         * cleanly, 105us vs 6125us). A zero here means no arrival was timed,
         * i.e. the rank was not actually earned. */
        CHECK(n > 0 && cands[0].first_us > 0,
              "the winner carries a MEASURED time-to-first-arrival, which is "
              "the key it was ranked on");
        CHECK(n > 0 && cands[0].arrivals > 0,
              "the winner was observed arriving at its entry at least once");
        /* PHASE 2, asserted rather than assumed — and this check exists because
         * the obvious one does not work. Deleting phase 2 entirely still leaves
         * the winner check above green (MEASURED): entered_often is ~5% of
         * auto_victim's time, so a 400-sample residency window does land in it,
         * and phase 3 drops grind_forever on its own. What ONLY phase 2 can
         * produce is the call-site evidence: a `call entered_often` instruction
         * inside the body residency nominated. sites == 0 is exactly the
         * signature of a picker that never looked. */
        CHECK(n > 0 && cands[0].sites > 0,
              "phase 2 found a DIRECT CALL naming the winner's entry inside a "
              "body observed running -- evidence a residency histogram cannot "
              "produce, and the only thing that distinguishes an expansion "
              "phase that ran from one that was deleted");
        CHECK(why[0] == '\0',
              "a Capstone build reports no degradation (an empty `why` on a "
              "successful pick)");
        /* TEARDOWN, asked three ways — because `kill(pid, 0)` alone is not a
         * teardown check at all. It says the process existed at that instant,
         * and both fatal outcomes are LATER: an orphaned int3 kills on the next
         * arrival, and a task left seized hangs on its next signal. A liveness
         * check fired microseconds after the sampler returns races the very
         * failure it is supposed to detect. */
        for (int i = 0; i < n; i++)
            CHECK(!has_trap_byte(vpid, cands[i].addr),
                  "no candidate entry still holds an int3: a trap the sampler "
                  "could not remove kills the target on its NEXT arrival, long "
                  "after we reported success");
        {
            pid_t tids[128];
            int nt = task_tids(vpid, tids, 128), still = 0;
            for (int i = 0; i < nt; i++)
                if (traced_by_us(tids[i]))
                    still++;
            CHECK(still == 0,
                  "every task was released: PTRACE_DETACH is refused on a "
                  "RUNNING tracee, and a thread left seized with our pump gone "
                  "hangs on its next signal, permanently");
        }
        /* Now let it RUN. entered_often is arrived at thousands of times in
         * 200 ms, so a trap we failed to remove is fatal well before this
         * assertion — which is the whole point of waiting. */
        {
            struct timespec nap = {0, 200 * 1000 * 1000};
            nanosleep(&nap, NULL);
        }
        CHECK(is_alive(vpid),
              "the victim survived 200ms of running FREE after the sampler "
              "detached — thousands of arrivals at the entry we armed");

        /* The module filter is the same rule --module= uses. A filter matching
         * nothing must yield NOTHING: if it were dropped on the floor this
         * would come back with the same winner as above. */
        char why2[192] = "";
        int n2 = asmspy_ptrace_sample(vpid, &syms, "no-such-module-xyz", cands,
                                      8, 200, why2, sizeof why2);
        CHECK(n2 == 0,
              "a module filter that matches nothing admits no candidates");
        /* An empty window is a retry, not a verdict — but a bare 0 is useless to
         * whoever has to decide what to retry. This one has a KNOWN cause and
         * the sampler must name it rather than report the generic "nothing was
         * entered", which would point the operator at the target's behaviour
         * when the answer is their own filter. */
        CHECK(strstr(why2, "module filter") != NULL,
              "an empty window says WHICH stage came up empty -- here, the "
              "module filter, not the target");
        CHECK(is_alive(vpid), "the victim survived the filtered run too");

        asmspy_symtab_free(&syms);
        kill_victim(vpid, -1);
    }

    /* --------------------------------------------------------------------- */
    /* §3  Correction 1, made visible.                                       */
    /*                                                                       */
    /* An unconditional PTRACE_CONT with sig=0 destroyed 89% of this victim's */
    /* SIGALRMs in the prototype and collapsed its throughput ~99%. A sampler */
    /* that re-injects costs a few percent; one that eats them fails here.    */
    /* 20% is a generous band chosen so scheduler noise on a loaded CI box    */
    /* cannot trip it — the defect it catches is an order of magnitude bigger.*/
    /*                                                                       */
    /* MEASURED DURING the attach, not around it: the damage is transient     */
    /* (the victim recovers the moment the tracer detaches), so a before/after*/
    /* pair bracketing the call would score a broken sampler as clean. All    */
    /* three windows are the SAME length so the counts compare directly.      */
    /* --------------------------------------------------------------------- */
    {
        int vout = -1;
        pid_t spid = spawn_victim(p_sig, "sigload_victim pid=", &vout);
        if (spid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_sig);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(spid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_sig);
            kill_victim(spid, vout);
            return 1;
        }

        asmspy_autocand_t cands[8];
        long before = sigload_ticks_over(vout, 800);

        job_t job = {0};
        job.pid = spid;
        job.syms = &syms;
        job.module = NULL;
        job.out = cands;
        job.max = 8;
        /* Long enough that the whole `during` window lands inside phase 1,
         * which is where the ~997 Hz interrupt traffic — and therefore the
         * signal-swallowing opportunity — actually is. */
        job.window_ms = 1500;
        pthread_t th;
        if (pthread_create(&th, NULL, run_sampler, &job) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            kill_victim(spid, vout);
            return 1;
        }
        long during = sigload_ticks_over(vout, 800);
        pthread_join(th, NULL);
        /* Drain first. Nobody read the pipe between the `during` window and the
         * join, so the lines buffered there describe victim-time we already
         * spent; measuring them as `after` would report a rate over a window
         * that never happened and make this check unfailable. */
        (void)sigload_ticks_over(vout, 60);
        long after = sigload_ticks_over(vout, 800);

        printf("# sigload ticks/800ms: before=%ld during=%ld after=%ld "
               "(rc=%d why='%s')\n",
               before, during, after, job.rc, job.why);

        CHECK(before > 50, "the signal victim ticked before we attached");
        CHECK(during * 100 >= before * 80,
              "the victim's signal rate must survive the sampler: re-inject "
              "with PTRACE_GETSIGINFO/PTRACE_EVENT_STOP rather than "
              "PTRACE_CONT(sig=0)");
        CHECK(after * 100 >= before * 80,
              "the victim's signal rate is intact after detach (no timer or "
              "handler left disarmed)");
        CHECK(is_alive(spid), "the signal victim survived the sampler");
        CHECK(job.rc >= 0,
              "a busy, signal-driven target is not a self-skip for this "
              "sampler");

        asmspy_symtab_free(&syms);
        kill_victim(spid, vout);
    }

    /* --------------------------------------------------------------------- */
    /* §4  Correction 2: PTRACE_O_TRACECLONE, or you kill the user's process. */
    /*                                                                       */
    /* cli/asmspy_engine.c:2954-2957 states the consequence outright: the     */
    /* phase-3 int3 is SHARED process text, so a thread that was never seized */
    /* and reaches it "would take a SIGTRAP with no tracer and DIE" — and     */
    /* SIGTRAP's default action takes the whole process with it.              */
    /*                                                                       */
    /* Asserted DIRECTLY rather than through survival, because survival is a  */
    /* lottery: it only fires if a post-attach thread happens to reach a      */
    /* candidate entry inside the window. TracerPid in /proc/<tid>/status is  */
    /* the option's observable — a thread created AFTER the seize is traced   */
    /* by us iff PTRACE_O_TRACECLONE was set. clone_victim exists to create   */
    /* exactly those threads: it stays single-threaded for 1.5 s, so every    */
    /* worker is unambiguously post-attach.                                   */
    /* --------------------------------------------------------------------- */
    {
        pid_t cpid = spawn_victim(p_clone, "clone_victim pid=", NULL);
        if (cpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_clone);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(cpid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_clone);
            kill_victim(cpid, -1);
            return 1;
        }

        asmspy_autocand_t cands[8];
        job_t job = {0};
        job.pid = cpid;
        job.syms = &syms;
        job.out = cands;
        job.max = 8;
        /* clone_victim naps 1.5 s before it spawns anything, so the attach has
         * to outlast that nap by a real margin: a window that merely reaches
         * the first pthread_create would make this check depend on a race it
         * has no business depending on. 2.5 s leaves ~1 s of spawning (~16
         * threads at its 60 ms cadence) inside the attach. */
        job.window_ms = 2500;
        pthread_t th;
        if (pthread_create(&th, NULL, run_sampler, &job) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            kill_victim(cpid, -1);
            return 1;
        }

        /* Everything present now is pre-attach (the leader alone). Anything
         * that shows up later had to be FOLLOWED. */
        pid_t known[128];
        int nknown = task_tids(cpid, known, 128);
        int new_seen = 0, new_traced = 0;
        long deadline = mono_ms() + 2600; /* inside the attach, never past it */
        while (mono_ms() < deadline) {
            pid_t now[128];
            int nn = task_tids(cpid, now, 128);
            for (int i = 0; i < nn; i++) {
                int seen = 0;
                for (int j = 0; j < nknown; j++)
                    if (known[j] == now[i])
                        seen = 1;
                if (seen)
                    continue;
                if (nknown < 128)
                    known[nknown++] = now[i];
                new_seen++;
                if (traced_by_us(now[i]))
                    new_traced++;
            }
            struct timespec nap = {0, 2 * 1000 * 1000}; /* 2 ms */
            nanosleep(&nap, NULL);
        }
        pthread_join(th, NULL);

        printf("# clone_victim: post-attach threads seen=%d, traced-by-us=%d "
               "(rc=%d why='%s')\n",
               new_seen, new_traced, job.rc, job.why);
        CHECK(new_seen > 0,
              "clone_victim really did create threads during the window (the "
              "check below is vacuous otherwise)");
        CHECK(new_traced > 0,
              "a thread created AFTER the seize is traced by us: without "
              "PTRACE_O_TRACECLONE it reaches the shared int3 with no tracer "
              "and takes the whole process down");
        CHECK(is_alive(cpid), "the cloning victim survived the sampler");

        asmspy_symtab_free(&syms);
        kill_victim(cpid, -1);
    }

    /* --------------------------------------------------------------------- */
    /* §5  A TRUNCATED SEIZE MUST NEVER ARM.                                  */
    /*                                                                       */
    /* Phase 3's int3 is one byte in SHARED process text. Every task of the   */
    /* target can execute it, so every task must be ours: a thread that was   */
    /* never seized reaches it, takes a SIGTRAP with no tracer, and SIGTRAP's */
    /* default action takes the WHOLE PROCESS down. "We seized most of them"  */
    /* is not a weaker safety, it is none.                                    */
    /*                                                                       */
    /* The truncation is real — PS_MAX_THREADS is 512 and a JVM, a browser or */
    /* any thread-pool server clears that routinely — but it cannot be        */
    /* provoked from outside without a 513-thread victim, so it could only be */
    /* argued rather than demonstrated. ASMSPY_PS_TEST_CAP caps the table, on */
    /* the model of the engine's ASMSPY_TEST_THR_OOM lever                    */
    /* (cli/asmspy_engine.c:1929), which exists for the same reason.          */
    /* --------------------------------------------------------------------- */
    CHECK(asmspy_ps_arm_note(1) == NULL,
          "a fully seized thread set may arm the entry breakpoint");
    CHECK(asmspy_ps_arm_note(0) != NULL &&
              strstr(asmspy_ps_arm_note(0), "UNCONFIRMED") != NULL,
          "a partially seized thread set may NOT arm, and the candidates it "
          "does return are labelled unconfirmed rather than passed off as "
          "measured");
    {
        char p_threads[256];
        snprintf(p_threads, sizeof p_threads, "%s/tid_victim", bdir);
        /* 2 of tid_victim's 3 tasks, so `beta` is a real task of the target
         * that we do NOT hold. tid_victim rather than threads_victim because
         * its workers spin at ~99% duty: threads_victim naps between bursts and
         * MEASURED, under the load of a full cli-smoke run its 400 ms window
         * observed nothing at all, which made every check below vacuous. */
        setenv("ASMSPY_PS_TEST_CAP", "2", 1);
        /* "tid=", not "alpha tid=": tid_victim's two workers race to print
         * their banners and EITHER may land in our first read. Matching the
         * specific one made the spawn fail outright about 1 run in 10. Either
         * line proves the same thing — a thread is running, so main's prctl has
         * already happened. */
        pid_t tpid = spawn_victim(p_threads, "tid=", NULL);
        if (tpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_threads);
            return 1;
        }
        /* Wait for the third task before sampling. tid_victim's two workers are
         * created back to back and only the FIRST prints a banner, so without
         * this the sampler can attach to a 2-task process — which fits the cap,
         * which is not the case under test. */
        for (long t0 = mono_ms(); mono_ms() - t0 < 2000;) {
            pid_t tt[16];
            if (task_tids(tpid, tt, 16) >= 3)
                break;
            struct timespec nap = {0, 2 * 1000 * 1000};
            nanosleep(&nap, NULL);
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(tpid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_threads);
            kill_victim(tpid, -1);
            return 1;
        }
        asmspy_autocand_t cands[8];
        char why[256] = "";
        int n = asmspy_ptrace_sample(tpid, &syms, NULL, cands, 8, 800, why,
                                     sizeof why);
        printf("# tid_victim (seize capped at 2 of 3): rc=%d why='%s'\n", n,
               why);
        for (int i = 0; i < n && i < 8; i++)
            printf("#   [%d] %-20s arrivals=%llu first=%lluus\n", i,
                   cands[i].name ? cands[i].name : "?", cands[i].arrivals,
                   cands[i].first_us);

        CHECK(n > 0,
              "the capped run still observed something (the checks below are "
              "vacuous otherwise)");
        CHECK(strstr(why, "fully seized") != NULL,
              "a truncated seize is REPORTED, not silently treated as full "
              "coverage");
        int armed = 0, confirmed = 0;
        for (int i = 0; i < n; i++) {
            if (has_trap_byte(tpid, cands[i].addr))
                armed = 1;
            if (cands[i].arrivals > 0 || cands[i].first_us > 0)
                confirmed = 1;
        }
        CHECK(!confirmed,
              "phase 3 did NOT run over a thread set we do not fully hold: no "
              "candidate carries an arrival or a measured first-arrival time");
        CHECK(!armed, "and no candidate entry holds a trap byte");
        /* Let it run: any int3 planted over the two unseized workers is fatal
         * within microseconds, and `worker` is entered constantly. */
        {
            struct timespec nap = {0, 300 * 1000 * 1000};
            nanosleep(&nap, NULL);
        }
        CHECK(is_alive(tpid),
              "the partially seized victim survived, running free — the "
              "unseized threads never met a trap");

        asmspy_symtab_free(&syms);
        kill_victim(tpid, -1);
        unsetenv("ASMSPY_PS_TEST_CAP");
    }

    /* --------------------------------------------------------------------- */
    /* §6  A fork inside an armed window must not hand the child our trap.    */
    /*                                                                       */
    /* fork(2) gives the child a copy-on-write copy of the target's TEXT, and */
    /* during phase 3 that text contains our int3. The child is a different   */
    /* process with no tracer, so the first time it reaches that entry it     */
    /* takes a SIGTRAP whose default action kills it. The armed windows total */
    /* ~800 ms and a fork-per-request server is an ordinary target.           */
    /*                                                                       */
    /* Counted by LINES, not by the counter value: a child killed by SIGTRAP  */
    /* prints nothing at all, and the counter it would have printed is simply */
    /* missing from the sequence — so a first/last delta would score a dead   */
    /* child as a live one.                                                   */
    /* --------------------------------------------------------------------- */
    {
        char p_fork[256];
        snprintf(p_fork, sizeof p_fork, "%s/forkhot_victim", bdir);
        int fout = -1;
        pid_t fpid = spawn_victim(p_fork, "forkhot_victim pid=", &fout);
        if (fpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_fork);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(fpid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_fork);
            kill_victim(fpid, fout);
            return 1;
        }
        asmspy_autocand_t cands[8];
        long seen_b = 0, miss_b = 0, seen_d = 0, miss_d = 0, seen_a = 0,
             miss_a = 0;
        forkhot_gaps_over(fout, 1200, &seen_b, &miss_b);

        job_t job = {0};
        job.pid = fpid;
        job.syms = &syms;
        job.out = cands;
        job.max = 8;
        /* THE MEASUREMENT WINDOW MUST CONTAIN AN ARMED, LIVE ENTRY, and both
         * halves of that are hard-won.
         *
         * It must overlap PHASE 3: with a 900 ms residency window the whole
         * 700 ms measurement once sat inside PHASE 1, before a single byte had
         * been planted, and the check scored 140/140 with the fork-child release
         * disabled entirely.
         *
         * And phase 3 must actually reach `slow_entry`, which is the only entry
         * here whose armed window stays open long enough for a fork to land in
         * it — a hot entry collects its four arrivals in microseconds and is
         * disarmed; a never-entered one holds the full ASMSPY_PS_CONFIRM_MS but
         * the child never executes it. MEASURED at window_ms = 400: phase 3's
         * own budget (also window_ms) ran out among the never-entered candidates
         * first, only __clock_gettime and hot_entry ever confirmed, and this
         * check missed the release mutation entirely. 900 ms reaches slow_entry;
         * the `during` window below is widened to span the whole call. */
        job.window_ms = 900;
        pthread_t th;
        if (pthread_create(&th, NULL, run_sampler, &job) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            kill_victim(fpid, fout);
            return 1;
        }
        forkhot_gaps_over(fout, 2600, &seen_d, &miss_d);
        pthread_join(th, NULL);
        (void)count_lines_over(fout, 60); /* drain the unread backlog */
        forkhot_gaps_over(fout, 1200, &seen_a, &miss_a);

        printf(
            "# forkhot children: before/1200ms=%ld(gaps %ld) "
            "during+teardown/2600ms=%ld(gaps %ld) after/1200ms=%ld(gaps %ld) "
            "(rc=%d why='%s')\n",
            seen_b, miss_b, seen_d, miss_d, seen_a, miss_a, job.rc, job.why);
        for (int i = 0; i < job.rc && i < 8; i++)
            printf("#   [%d] %-16s arrivals=%llu first=%lluus\n", i,
                   cands[i].name ? cands[i].name : "?", cands[i].arrivals,
                   cands[i].first_us);
        CHECK(seen_b > 100 && miss_b == 0,
              "children were completing before we attached, with an unbroken "
              "sequence — the measurement itself is sound");
        /* 90%, not 60%. The loose band was the whole reason this check could
         * not see the follow-up defect: a fork child that reaches phase 3's
         * generic arrival test is re-planted into the WRONG address space and
         * released carrying an int3, and 40% child mortality sailed through.
         * ZERO tolerance, and it CAN be zero because a gap is not a rate: a child
         * either printed its sequence number or it did not. Measured, a healthy
         * sampler leaves 0 holes in ~240 children every run; disabling the
         * fork-child release leaves ~10, the children forked inside slow_entry's
         * one armed window. One unit of slack for a line straddling the window
         * edge, and no more. No rate band could do this — 60/90/95/97% either
         * tolerated the defect or would trip on a loaded box, because the defect
         * and the drift are the same size. */
        /* `seen_d > 100` is the FLOOR, and without it this check passes under
         * TOTAL child mortality: forkhot_gaps_over reports missing = 0 when it
         * saw no `kid=` lines at all (there is no sequence to have a hole in),
         * so a sampler that killed every child scored a clean 0 gaps. A gap
         * count is only evidence when there is a sequence to count gaps in. */
        CHECK(seen_d > 300 && miss_d <= 1,
              "children forked while an entry was ARMED still complete: a "
              "child inherits a copy-on-write copy of the int3 and dies of it "
              "unless PTRACE_O_TRACEFORK is set, its copy restored, and it is "
              "never mistaken for an ARRIVAL of the target");
        CHECK(seen_a > 100 && miss_a <= 1,
              "and the fork sequence is unbroken after detach");
        CHECK(is_alive(fpid), "the forking victim survived the sampler");

        asmspy_symtab_free(&syms);
        kill_victim(fpid, fout);
    }

    /* --------------------------------------------------------------------- */
    /* §7  Job control: a ^Z'd target must not be resumed, and must still be   */
    /*     handed back.                                                       */
    /*                                                                       */
    /* PTRACE_SEIZE makes group-stops OURS to end, so a SIGSTOP arriving mid- */
    /* window is a stop only the sampler can resolve. Two ways to get it      */
    /* wrong, and the sampler had one of each:                                */
    /*                                                                       */
    /*  - treat the LISTENed thread as one whose ptrace-stop we hold, and     */
    /*    ps_resume_all CONTs a process the user deliberately suspended;      */
    /*  - keep LISTENing it at teardown, where PTRACE_DETACH is refused ESRCH */
    /*    (MEASURED with a standalone probe) and the retry re-LISTENs on every */
    /*    round, so it never converges: 3 x 200 ms burned and a false "could   */
    /*    not be handed back clean" for a target that is merely stopped.       */
    /*                                                                       */
    /* Everything here was previously reasoned and never executed.            */
    /* --------------------------------------------------------------------- */
    {
        /* A MULTI-threaded victim, deliberately. SIGSTOP group-stops every
         * thread, so the teardown sees group-stops for threads it is NOT
         * waiting on — and that is the path that used to fall through to
         * ps_cont and restart a process the user had suspended. With a
         * single-threaded victim there is only ever the awaited thread, and the
         * defect is invisible: MEASURED, the mutation that restores it changed
         * nothing while this check used sigload_victim. */
        char p_job[256];
        snprintf(p_job, sizeof p_job, "%s/hotthreads_victim", bdir);
        int vout = -1;
        pid_t jpid = spawn_victim(p_job, "hotthreads_victim pid=", &vout);
        if (jpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_job);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(jpid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_job);
            kill_victim(jpid, vout);
            return 1;
        }
        asmspy_autocand_t cands[8];
        job_t job = {0};
        job.pid = jpid;
        job.syms = &syms;
        job.out = cands;
        job.max = 8;
        job.window_ms = 900;
        pthread_t th;
        if (pthread_create(&th, NULL, run_sampler, &job) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            kill_victim(jpid, vout);
            return 1;
        }
        /* ^Z it mid-window and HOLD it stopped THROUGH the teardown. Releasing
         * it before the join (which is what this check did first) means the
         * thread is no longer in PTRACE_LISTEN when ps_detach_all runs, and the
         * whole detach-under-LISTEN path — the one that cannot converge — is
         * never entered. MEASURED: with the early SIGCONT, removing the
         * teardown's LISTEN suppression changed nothing. */
        struct timespec nap = {0, 250 * 1000 * 1000};
        nanosleep(&nap, NULL);
        kill(jpid, SIGSTOP);
        nanosleep(&nap, NULL);
        char st_while = proc_state(jpid);

        long t0 = mono_ms();
        pthread_join(th, NULL);
        long joined_ms = mono_ms() - t0;
        char st_after = proc_state(jpid);
        /* Sample EVERY thread's state before the SIGCONT — after it they are all
         * running by design, and the check would be measuring nothing. */
        int nt_after = 0, running_after = 0;
        {
            pid_t tids[128];
            nt_after = task_tids(jpid, tids, 128);
            for (int i = 0; i < nt_after; i++) {
                char st = proc_state(tids[i]);
                if (st != 'T' && st != 't' && st != '?')
                    running_after++;
            }
        }
        kill(jpid, SIGCONT);
        (void)count_lines_over(vout, 60);
        long after = count_lines_over(vout, 600);

        printf("# job control: state under SIGSTOP='%c' after detach='%c' "
               "(%d/%d threads still stopped) join=%ldms after=%ld "
               "(rc=%d why='%s')\n",
               st_while, st_after, nt_after - running_after, nt_after,
               joined_ms, after, job.rc, job.why);

        CHECK(st_while == 'T' || st_while == 't',
              "a SIGSTOPped target STAYS stopped under the sampler: "
              "PTRACE_LISTEN honours the stop the user asked for, and nothing "
              "may PTRACE_CONT it back to life");
        CHECK(job.rc >= 0,
              "a ^Z'd target is not a self-skip and not a damaged one: the "
              "teardown must interrupt it out of LISTEN and detach, not report "
              "that it could not be handed back");
        CHECK(strstr(job.why, "handed back clean") == NULL,
              "and it is not reported as a target we damaged");
        CHECK(joined_ms < 3000,
              "the teardown CONVERGES: re-LISTENing on every detach retry "
              "burns 3 x 200ms per thread and never reaches a ptrace-stop");
        CHECK(st_after == 'T' || st_after == 't',
              "and it is handed back STILL STOPPED — detaching out of "
              "group-stop leaves the process suspended, exactly as the user "
              "left it, for EVERY thread and not just the one we awaited");
        /* Every thread, not just the leader: the defect resumes exactly the ones
         * the teardown was NOT waiting on, so a leader-only check cannot see
         * it. */
        CHECK(
            nt_after > 1 && running_after == 0,
            "no thread of the suspended target was restarted by the teardown");
        /* The property under test is "it moves again", not the rate — §3
         * measures the rate. hotthreads_victim beats at ~20 Hz. */
        CHECK(after > 3, "and the victim is running again after SIGCONT");
        CHECK(is_alive(jpid), "the job-controlled victim survived");

        asmspy_symtab_free(&syms);
        kill_victim(jpid, vout);
    }

    /* --------------------------------------------------------------------- */
    /* §8  CONCURRENT arrivals: several threads at ONE armed entry.           */
    /*                                                                       */
    /* Every other victim that gets far enough to be armed is single-threaded, */
    /* so the concurrent half of phase 3 was never executed. Two things live   */
    /* only here: a thread trapping while another is mid-step, whose stop is   */
    /* consumed by a pump that is NOT collecting arrivals (it must be rewound  */
    /* and RELEASED — handing back a SIGTRAP that an int3's si_code makes look */
    /* like the target's own kills a process with no handler), and a trap      */
    /* already in flight when the byte is restored.                            */
    /* --------------------------------------------------------------------- */
    {
        char p_hot[256];
        snprintf(p_hot, sizeof p_hot, "%s/hotthreads_victim", bdir);
        int hout = -1;
        pid_t hpid = spawn_victim(p_hot, "hotthreads_victim pid=", &hout);
        if (hpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_hot);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(hpid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_hot);
            kill_victim(hpid, hout);
            return 1;
        }
        asmspy_autocand_t cands[8];
        long before = count_lines_over(hout, 500);

        job_t job = {0};
        job.pid = hpid;
        job.syms = &syms;
        job.out = cands;
        job.max = 8;
        job.window_ms = 500;
        pthread_t th;
        if (pthread_create(&th, NULL, run_sampler, &job) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            kill_victim(hpid, hout);
            return 1;
        }
        long during = count_lines_over(hout, 500);
        pthread_join(th, NULL);
        (void)count_lines_over(hout, 60);
        long after = count_lines_over(hout, 500);
        int n = job.rc;

        printf("# hotthreads: rc=%d beats/500ms before=%ld during=%ld "
               "after=%ld why='%s'\n",
               n, before, during, after, job.why);
        for (int i = 0; i < n && i < 8; i++)
            printf("#   [%d] %-16s arrivals=%llu first=%lluus\n", i,
                   cands[i].name ? cands[i].name : "?", cands[i].arrivals,
                   cands[i].first_us);

        /* PRESENT and CONFIRMED, not RANKED FIRST. Ranking is §2's job, and it
         * is sound there because the rival entry is a thousand times slower.
         * Here both `shared_entry` and the leader's `mono_ms` are entered
         * thousands of times per millisecond, so which one is reached first
         * after arming is scheduler noise — measured 16us vs 20us, and the
         * order flips run to run. Asserting a winner between two equally hot
         * entries would be asserting the noise. What §8 is actually about is
         * that concurrent arrivals are handled at all. */
        CHECK(listed(cands, n, "shared_entry"),
              "the entry every thread arrives at is CONFIRMED on a "
              "multi-threaded target: several threads trapping at one address "
              "at once, each rewound, stepped over and re-armed");
        CHECK(n > 0 && cands[0].arrivals > 0,
              "and phase 3 really ran here (arrivals were counted)");
        CHECK(before > 3 && during * 100 >= before * 50,
              "the target keeps making progress while an entry is armed and "
              "several threads are trapping on it at once");
        for (int i = 0; i < n; i++)
            CHECK(!has_trap_byte(hpid, cands[i].addr),
                  "no entry is left holding an int3 after concurrent arrivals");
        {
            pid_t tids[128];
            int nt = task_tids(hpid, tids, 128), still = 0;
            for (int i = 0; i < nt; i++)
                if (traced_by_us(tids[i]))
                    still++;
            CHECK(still == 0, "and every one of its threads was released");
        }
        {
            struct timespec nap = {0, 200 * 1000 * 1000};
            nanosleep(&nap, NULL);
        }
        CHECK(is_alive(hpid),
              "the multi-threaded victim survived 200ms of running FREE — "
              "every thread re-entering the address we armed");
        CHECK(after > 3, "and it is still making progress");

        asmspy_symtab_free(&syms);
        kill_victim(hpid, hout);
    }

    /* --------------------------------------------------------------------- */
    /* §9  AN UNKNOWN IS NOT A LICENCE TO ACT.                                */
    /*                                                                       */
    /* The sampler asks /proc whose address space each task is in. When that  */
    /* read FAILS the answer is neither "ours" nor "a fork child" — and the   */
    /* code used to resolve it as the latter, which routed a real thread of   */
    /* the target through the RELEASE path: detached, dropped from the table, */
    /* and — because the release path never touched `covered` — leaving the   */
    /* arm gate satisfied. An int3 then went into shared text a now-untraced  */
    /* thread was about to execute. One failed open(2) in the CALLING process */
    /* (this ships in libasmspy.a; the caller is long-lived and may be at its */
    /* fd limit) was enough to kill the target.                                */
    /*                                                                       */
    /* Unreachable from outside without a lever — /proc is readable for any   */
    /* task we have seized — so ASMSPY_PS_TEST_TGID_FAIL forces it, on the    */
    /* model of ASMSPY_PS_TEST_CAP and the engine's ASMSPY_TEST_THR_OOM. A    */
    /* branch that is fatal when it resolves the wrong way does not get to be */
    /* merely argued about.                                                    */
    /* --------------------------------------------------------------------- */
    {
        char p_hot2[256];
        snprintf(p_hot2, sizeof p_hot2, "%s/hotthreads_victim", bdir);
        setenv("ASMSPY_PS_TEST_TGID_FAIL", "1", 1);
        int uout = -1;
        pid_t upid = spawn_victim(p_hot2, "hotthreads_victim pid=", &uout);
        if (upid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s\n", p_hot2);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(upid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_hot2);
            kill_victim(upid, uout);
            return 1;
        }
        asmspy_autocand_t cands[8];
        char why[256] = "";
        long before = count_lines_over(uout, 400);
        int n = asmspy_ptrace_sample(upid, &syms, NULL, cands, 8, 400, why,
                                     sizeof why);
        printf("# unknown-kind (tgid read forced to fail): rc=%d why='%s'\n", n,
               why);

        CHECK(strstr(why, "fully seized") != NULL,
              "a task we cannot classify costs COVERAGE: the arm gate must "
              "refuse, because an unknown is not a licence to arm");
        int confirmed = 0, armed = 0;
        for (int i = 0; i < n; i++) {
            if (cands[i].arrivals > 0 || cands[i].first_us > 0)
                confirmed = 1;
            if (has_trap_byte(upid, cands[i].addr))
                armed = 1;
        }
        CHECK(!confirmed, "phase 3 did not run over a thread set we cannot "
                          "prove we hold");
        CHECK(!armed, "and no entry holds a trap byte");
        {
            /* The release path must NOT have run: an unclassifiable task stays
             * traced for the life of the call, which is what stops it meeting a
             * trap untraced. By teardown it is released like any other. */
            pid_t tids[128];
            int nt = task_tids(upid, tids, 128), still = 0;
            for (int i = 0; i < nt; i++)
                if (traced_by_us(tids[i]))
                    still++;
            CHECK(nt > 1 && still == 0,
                  "and every task was still handed back at teardown");
        }
        {
            struct timespec nap = {0, 300 * 1000 * 1000};
            nanosleep(&nap, NULL);
        }
        long after = count_lines_over(uout, 400);
        CHECK(is_alive(upid) && before > 3 && after > 3,
              "the victim survived and kept running — no thread was released "
              "into an address space we had armed");

        asmspy_symtab_free(&syms);
        kill_victim(upid, uout);
        unsetenv("ASMSPY_PS_TEST_TGID_FAIL");
    }

    /* --------------------------------------------------------------------- */
    /* §10  THE DISPOSITION TABLE, WALKED IN FULL.                            */
    /*                                                                       */
    /* Four rounds of fixes to this module each closed the defects they were   */
    /* given and opened exactly one more of the SAME class: a task the         */
    /* conditions did not describe fell out of the bottom of an `if` chain and */
    /* was handed a verb anyway. asmspy_ps_decide replaces that chain with a   */
    /* total function of (kind x reason), and the compiler enforces that every */
    /* cell is written (-Werror=switch/-switch-enum, mk/cli.mk).               */
    /*                                                                       */
    /* What the compiler CANNOT check is what the cells say. These checks are  */
    /* the policy, stated once and asserted over all 3 x 11 x 8 combinations — */
    /* including every combination no victim in this tree can produce, which   */
    /* is exactly where the last four Criticals lived.                        */
    /* --------------------------------------------------------------------- */
    {
        static const asmspy_ps_kind_t KINDS[] = {
            ASMSPY_PS_OWN, ASMSPY_PS_FOREIGN, ASMSPY_PS_UNKNOWN};
        static const asmspy_ps_reason_t WHYS[] = {
            ASMSPY_PS_R_CLONE,       ASMSPY_PS_R_FORK,
            ASMSPY_PS_R_EVENT_OTHER, ASMSPY_PS_R_JOBCTL,
            ASMSPY_PS_R_INTERRUPT,   ASMSPY_PS_R_OUR_STEP,
            ASMSPY_PS_R_OUR_TRAP,    ASMSPY_PS_R_TRAP_UNSURE,
            ASMSPY_PS_R_APP_TRAP,    ASMSPY_PS_R_STRAY_TRAP,
            ASMSPY_PS_R_SIGNAL};
        int cells = 0, bad_range = 0, unknown_acts = 0, foreign_acts = 0,
            unknown_sig = 0, own_sig = 0;
        for (unsigned k = 0; k < sizeof KINDS / sizeof *KINDS; k++)
            for (unsigned r = 0; r < sizeof WHYS / sizeof *WHYS; r++)
                for (int flags = 0; flags < 8; flags++) {
                    asmspy_ps_act_t a =
                        asmspy_ps_decide(KINDS[k], WHYS[r], flags & 1,
                                         (flags >> 1) & 1, (flags >> 2) & 1);
                    cells++;
                    if (a < ASMSPY_PS_ACT_HOLD ||
                        a > ASMSPY_PS_ACT_RELEASE_SIGNAL)
                        bad_range++;
                    if (KINDS[k] == ASMSPY_PS_UNKNOWN) {
                        /* THE RULE, and the currently-open Critical is a single
                         * violation of its last clause: an unknown never
                         * authorises poking, releasing, arming, or
                         * CONTINUING-WITH-A-SIGNAL. Our own int3 reports
                         * si_code == SI_KERNEL exactly as the target's own does,
                         * so "deliver it if it looks like the app's" delivers
                         * OURS to a process with no handler. */
                        if (a == ASMSPY_PS_ACT_RELEASE ||
                            a == ASMSPY_PS_ACT_RELEASE_SIGNAL ||
                            a == ASMSPY_PS_ACT_COLLECT)
                            unknown_acts++;
                        if (a == ASMSPY_PS_ACT_RESUME_SIGNAL)
                            unknown_sig++;
                    }
                    if (KINDS[k] == ASMSPY_PS_FOREIGN &&
                        a != ASMSPY_PS_ACT_RELEASE &&
                        a != ASMSPY_PS_ACT_RELEASE_SIGNAL)
                        foreign_acts++;
                    if (KINDS[k] == ASMSPY_PS_OWN &&
                        WHYS[r] == ASMSPY_PS_R_SIGNAL &&
                        a != ASMSPY_PS_ACT_RESUME_SIGNAL)
                        own_sig++;
                }
        printf("# decision table: %d cells walked\n", cells);
        CHECK(cells == 264 && bad_range == 0,
              "every (kind x reason x flags) combination has a written "
              "disposition -- a total function, not a chain of ifs with a "
              "fall-through tail");
        CHECK(
            unknown_acts == 0,
            "an unknown NEVER authorises releasing or confirming: it stays "
            "traced (so it cannot meet our trap untraced) and it never counts "
            "as an arrival of the target");
        CHECK(
            unknown_sig == 0,
            "an unknown is NEVER continued with a signal -- the open Critical "
            "was exactly this cell, handing a task of unknown kind our own "
            "int3's SIGTRAP because SI_KERNEL cannot tell whose int3 it was");
        CHECK(asmspy_ps_decide(ASMSPY_PS_UNKNOWN, ASMSPY_PS_R_OUR_TRAP, 0, 0,
                               1) == ASMSPY_PS_ACT_CLEAR_AND_RESUME,
              "and the one write it does allow is the read-guarded REMOVAL of "
              "our own byte, which is correct in all three address spaces it "
              "might be in -- anything else leaves it trap-looping forever");
        CHECK(foreign_acts == 0,
              "a fork child is RELEASED at its first stop whatever the stop "
              "was: never sampled, never poked through, never counted");
        CHECK(own_sig == 0,
              "correction 1 holds for every flag combination: a real signal to "
              "a thread of the target is re-injected, never swallowed");
        CHECK(asmspy_ps_decide(ASMSPY_PS_OWN, ASMSPY_PS_R_OUR_TRAP, 0, 0, 1) ==
                      ASMSPY_PS_ACT_COLLECT &&
                  asmspy_ps_decide(ASMSPY_PS_OWN, ASMSPY_PS_R_OUR_TRAP, 0, 0,
                                   0) == ASMSPY_PS_ACT_REWIND_RESUME,
              "our own trap is an ARRIVAL only for the collector; anyone else "
              "rewinds it and lets it run -- never re-injects it");
        CHECK(asmspy_ps_decide(ASMSPY_PS_OWN, ASMSPY_PS_R_JOBCTL, 0, 0, 0) ==
                      ASMSPY_PS_ACT_LISTEN &&
                  asmspy_ps_decide(ASMSPY_PS_OWN, ASMSPY_PS_R_JOBCTL, 1, 0,
                                   0) == ASMSPY_PS_ACT_HOLD,
              "job control is LISTENed while we are working and HELD once the "
              "teardown starts, where DETACH under LISTEN is refused ESRCH");
    }

    /* --------------------------------------------------------------------- */
    /* §11  AN UNKNOWN TASK MEETING OUR OWN ARMED int3, and fork+exec.        */
    /*                                                                       */
    /* §9 forces EVERY /proc read to fail, which costs coverage at SEIZE time */
    /* — so the arm gate refuses and phase 3 never plants a byte. The fatal   */
    /* path therefore sat behind a lever that guaranteed it could not run:    */
    /* a task of unknown kind EXECUTING our armed trap. It failed the         */
    /* `kind == PS_OWN` arrival gate, fell through, and was handed            */
    /* ps_cont(w, sigtrap_is_app(w) ? SIGTRAP : 0) — and an int3 reports      */
    /* si_code == SI_KERNEL whoever planted it, so that predicate said "the   */
    /* target's own" about OURS. A process with no SIGTRAP handler dies of it,*/
    /* un-rewound, so even a swallowed one would resume at base+1.            */
    /*                                                                       */
    /* ASMSPY_PS_TEST_TGID_FAIL=armed fails the read ONLY while the trap byte */
    /* is in the text, which is the scenario the module's own comment cites:  */
    /* one failed open(2) in a long-lived caller at its fd limit, AFTER the   */
    /* arm. Every child forked into that window is then unclassifiable, and   */
    /* forkhot's children run the armed entries immediately.                  */
    /*                                                                       */
    /* The victim also runs in EXEC mode, which is the second finding in one  */
    /* window: an unclassifiable child is never released early, so it is still */
    /* tabled — carrying a restore address into the PARENT's image — when it  */
    /* execve()s. A tracer that reads "PEEKTEXT failed" as "we could not tell" */
    /* then reports fork+exec, the commonest fork shape there is, as a target */
    /* it DAMAGED.                                                            */
    /* --------------------------------------------------------------------- */
    {
        char p_fork2[256];
        snprintf(p_fork2, sizeof p_fork2, "%s/forkhot_victim", bdir);
        setenv("ASMSPY_FORKHOT_EXEC", "1", 1);
        int fout = -1;
        pid_t fpid = spawn_victim(p_fork2, "forkhot_victim pid=", &fout);
        if (fpid < 0) {
            fprintf(stderr, "FAIL: could not spawn %s (exec mode)\n", p_fork2);
            return 1;
        }
        asmspy_symtab_t syms = {0};
        if (asmspy_symtab_load(fpid, &syms) != 0) {
            fprintf(stderr, "FAIL: no symbols for %s\n", p_fork2);
            kill_victim(fpid, fout);
            return 1;
        }
        long seen_b = 0, miss_b = 0, seen_d = 0, miss_d = 0;
        forkhot_gaps_over(fout, 700, &seen_b, &miss_b);

        setenv("ASMSPY_PS_TEST_TGID_FAIL", "armed", 1);
        asmspy_autocand_t cands[8];
        job_t job = {0};
        job.pid = fpid;
        job.syms = &syms;
        job.out = cands;
        job.max = 8;
        /* LONGER than §6's 400 ms, and phase 3 is why. The lever only fires
         * while a byte is armed at an entry a thread has already been seen
         * reaching, and the only such entry here is `slow_entry` — entered every
         * ~20 ms, so its four arrivals cannot be collected inside
         * ASMSPY_PS_CONFIRM_MS and the window stays armed for all of it.
         * MEASURED at window_ms = 400: phase 3's own budget (also window_ms) ran
         * out among the never-entered candidates before it ever reached
         * slow_entry, so nothing was armed-and-live and the lever never fired. */
        job.window_ms = 900;
        pthread_t th;
        if (pthread_create(&th, NULL, run_sampler, &job) != 0) {
            fprintf(stderr, "FAIL: pthread_create\n");
            kill_victim(fpid, fout);
            return 1;
        }
        /* ONE window spanning the whole call AND its teardown. A child that
         * cannot be classified is never released early — it stays traced until
         * the teardown — so its `kid=` line arrives LATE, not never, and a
         * window that stopped at the join would score a survivor as a hole. */
        forkhot_gaps_over(fout, 2600, &seen_d, &miss_d);
        pthread_join(th, NULL);
        unsetenv("ASMSPY_PS_TEST_TGID_FAIL");

        printf("# unknown-at-our-trap (exec forks, tgid fails WHILE ARMED): "
               "before=%ld(gaps %ld) during+teardown=%ld(gaps %ld) rc=%d "
               "why='%s'\n",
               seen_b, miss_b, seen_d, miss_d, job.rc, job.why);
        for (int i = 0; i < job.rc && i < 8; i++)
            printf("#   [%d] %-16s arrivals=%llu first=%lluus\n", i,
                   cands[i].name ? cands[i].name : "?", cands[i].arrivals,
                   cands[i].first_us);

        CHECK(seen_b > 30 && miss_b == 0,
              "fork+exec children were completing before we attached, with an "
              "unbroken sequence (the measurement is sound)");
        /* NON-VACUITY. The lever fails a /proc read only while a byte is
         * actually planted, so a lost-coverage report is direct evidence that a
         * task really was tabled as UNKNOWN inside an armed window — which is
         * the whole scenario. Without this the checks below could pass simply
         * because nothing was ever armed. */
        CHECK(strstr(job.why, "fully seized") != NULL,
              "a task really was tabled as unclassifiable WHILE the entry trap "
              "was armed (otherwise the checks below are vacuous)");
        CHECK(
            seen_d > 100 && miss_d == 0,
            "a task of UNKNOWN kind that executes our armed int3 survives it: "
            "the byte is taken back out of whatever address space it is in, "
            "the task is rewound off base+1, and it is NEVER handed the "
            "SIGTRAP -- si_code says SI_KERNEL for our int3 and for the "
            "target's own alike");
        CHECK(job.rc >= 0 && strstr(job.why, "handed back clean") == NULL,
              "and fork+exec is not reported as damage: after an execve the "
              "armed address is not mapped, which means our byte is provably "
              "NOT there -- not that we could not tell");
        for (int i = 0; i < job.rc && i < 8; i++)
            CHECK(
                !has_trap_byte(fpid, cands[i].addr),
                "no entry is left holding an int3 after the unknown-kind run");
        CHECK(is_alive(fpid), "the forking victim survived");

        asmspy_symtab_free(&syms);
        kill_victim(fpid, fout);
        unsetenv("ASMSPY_FORKHOT_EXEC");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
