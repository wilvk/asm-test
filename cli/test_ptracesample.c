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
    close(epipe[0]);
    buf[n > 0 ? n : 0] = '\0';
    if (n <= 0 || strstr(buf, banner) == NULL) {
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
        CHECK(kill(vpid, 0) == 0, "the victim survived the sampler");

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
        CHECK(kill(vpid, 0) == 0, "the victim survived the filtered run too");

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
        CHECK(kill(spid, 0) == 0, "the signal victim survived the sampler");
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
        CHECK(kill(cpid, 0) == 0, "the cloning victim survived the sampler");

        asmspy_symtab_free(&syms);
        kill_victim(cpid, -1);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
