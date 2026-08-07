/* asmspy_ptracesample.c — the PERF-FREE region picker.
 *
 * The design note, the measurements it was tuned to, and the six corrections
 * this file carries live in cli/asmspy_ptracesample.h. What follows is the
 * machinery, phase by phase; each phase names the measurement that forced its
 * shape rather than restating the header.
 *
 * ONE RULE ABOVE ALL OTHERS, because the failure mode is silent and fatal to a
 * process we do not own: every ptrace-stop this file consumes goes through
 * exactly ONE three-step pipeline, and nothing else in the file may decide what
 * to do with a tracee.
 *
 *   ps_classify   a waitpid status -> asmspy_ps_reason_t. Reads only.
 *   asmspy_ps_decide  (kind, reason) -> asmspy_ps_act_t. PURE, TOTAL, and a
 *                 nested switch with no `default:`, so a kind or a reason that
 *                 nobody has written a disposition for does not compile.
 *   ps_perform    an act -> the syscalls. The ONLY place in this file that
 *                 CONTs, DETACHes, LISTENs or POKEs a byte on behalf of a stop.
 *
 * That shape is the fix for the class of defect this module shipped four times
 * running: a task the conditions did not describe fell out of the bottom of an
 * `if` chain and was handed a verb anyway. See the long note in
 * asmspy_ptracesample.h. Every other resume in the file (ps_resume_all,
 * ps_phase1's post-sample CONT) passes sig 0 and is the completion of an
 * ASMSPY_PS_ACT_HOLD this pipeline already decided on — never a second
 * classification, which is how the prototype ate 89% of a victim's SIGALRMs. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "asmspy_arch.h" /* register seam + ASMSPY_HOST_ARCH (x86-64 | AArch64) */
#include "asmspy_autoregion.h" /* asmspy_autoregion_rank_ip — ONE ranking rule */
#include "asmspy_ptracesample.h"
#include "libasmspy.h"

/* Threads we will follow.
 *
 * This cap used to be justified as "the residency phase is a SHORTLIST
 * generator, and a 4096-thread process does not need every thread polled to
 * nominate a body". That is true of PHASE 1 and FALSE of PHASE 3, and the
 * difference is fatal: phase 3's int3 is one byte in SHARED process text, so
 * thread 513 executes it, takes a SIGTRAP with no tracer, and SIGTRAP's default
 * action takes the WHOLE PROCESS down. That is verbatim
 * cli/asmspy_engine.c:2954-2957, and a JVM, a browser or any thread-pool server
 * clears 512 threads routinely — deterministic, not a race.
 *
 * So the cap survives, but it now feeds `covered` (see ps_ctx_t): a truncated
 * seize is allowed to sample, and is NEVER allowed to arm. */
#define PS_MAX_THREADS 512

/* Slack ABOVE the seize cap, and it is a safety mechanism, not a convenience.
 *
 * The cap is enforced when we ENUMERATE, but a task cloned DURING an armed
 * window arrives through PTRACE_O_TRACECLONE at its attach-stop, before it has
 * run a single instruction. If the table has no room for it the only options are
 * to detach it — releasing an untraced task into a process whose text contains
 * our int3, i.e. the exact death the cap exists to prevent — or to strand it
 * stopped forever. With headroom it is simply TABLED: safe, traced, and the
 * coverage flag is what records that the cap was exceeded.
 *
 * MEASURED: without this, cli/test_ptracesample.c's capped run failed roughly 1
 * time in 5 — tid_victim's second worker is created just after the seize scan,
 * so the gate saw full coverage, phase 3 armed, and the new thread was detached
 * into an armed process and killed it. */
#define PS_ARM_HEADROOM 16
#define PS_TABLE_SLOTS  (PS_MAX_THREADS + PS_ARM_HEADROOM)

/* Passes ps_seize_all will make before it gives up on converging. One pass is
 * not enough: a thread cloned BY a thread we had not yet seized is neither
 * seized nor followed, so the scan repeats until a whole pass adds nothing.
 * Exhausting these means the target is spawning faster than we can enumerate,
 * which is a truthful "not fully covered", not a failure. */
#define PS_SEIZE_PASSES 8

/* PTRACE_DETACH attempts per thread before we call it stuck. */
#define PS_DETACH_TRIES 3

/* Distinct FUNCTIONS the residency histogram can hold. Samples are folded to
 * their symbol at capture time rather than kept as raw PCs, which keeps this
 * table bounded by the number of functions observed (tens) instead of by the
 * number of samples (thousands) — the fold is then LOSSLESS, which the pure
 * rank's own doc comment requires of its out_cap. */
#define PS_MAX_SYMS 128

/* Bytes of a shortlisted function phase 2 will scan for direct calls. Generous
 * for a leaf or a loop body; a function longer than this has its tail skipped,
 * which costs a candidate, never correctness. */
#define PS_SCAN_BYTES 4096

/* Event-pump pacing. The first PS_SPIN_US of any wait is a BUSY poll and the
 * rest naps PS_POLL_US at a time. That split is not micro-optimisation: phase
 * 3 ranks on TIME-TO-FIRST-ARRIVAL (correction 4), and nanosleep's real
 * granularity here is ~60 us, so a nap-only pump would quantise every fast
 * arrival to the same value and re-create the tie it exists to break. The
 * correct pick arrives in TENS of microseconds; the spin is what can see that,
 * and it exits immediately on the common path. */
#define PS_SPIN_US 500
#define PS_POLL_US 200

/* How long to wait for one PTRACE_INTERRUPT to land before giving up on that
 * sample. A thread that does not stop in 2 ms is one whose interrupt was
 * absorbed by a concurrent signal-delivery-stop (documented in ps_phase1);
 * skipping the sample is right — a statistical sampler owes no particular
 * thread a reading. */
#define PS_INTR_US 2000

/* Bringing a thread to a stop for a POKETEXT is a correctness step, not a
 * sample: it has to succeed or the breakpoint bookkeeping is wrong. */
#define PS_STOP_US 200000

/* ps_dispatch outcomes. */
#define PS_EV_NONE    0 /* handled and resumed; nothing for the caller       */
#define PS_EV_STOPPED 1 /* the awaited thread is stopped and stays that way  */
#define PS_EV_ARRIVED 2 /* a thread reached the armed entry (stays stopped)  */
#define PS_EV_GONE    3 /* the awaited thread, or the whole target, is gone  */

/* ps_thr_t::kind is asmspy_ps_kind_t (ASMSPY_PS_OWN / _FOREIGN / _UNKNOWN),
 * declared in the header beside the disposition table that is total over it.
 * It was three #defines on an `int` field, which is exactly what let a fourth
 * case be added without -Wswitch noticing. */

typedef struct {
    pid_t tid;
    int stopped; /* WE consumed its ptrace-stop and have not resumed it yet */
    /* In job-control group-stop under PTRACE_LISTEN. A THIRD state, not a
     * flavour of `stopped`, because a LISTENed tracee is NOT in a ptrace-stop we
     * hold: PEEKTEXT/POKETEXT/CONT all fail ESRCH on it. Marking it `stopped`
     * (which is what this file used to do, before the job-control branch even
     * ran) told ps_unplant it had a thread that could restore the trap byte,
     * told ps_phase3_one it had a planter, and told ps_resume_all to CONT a
     * process the user had deliberately suspended — the exact thing the
     * PTRACE_LISTEN branch exists to prevent. */
    int listening;
    /* WHOSE ADDRESS SPACE is this task in? THREE states, not two, and the third
     * is the whole point: "we could not find out" is not a synonym for either
     * answer. Resolving it as FOREIGN released a real thread of the target into
     * an armed address space — with no coverage cost, so the gate still passed
     * and the int3 still went in. One failed open(2) in the CALLING process (this
     * ships in libasmspy.a, so the caller is long-lived and may be at its fd
     * limit) was enough to kill the target.
     *
     *   ASMSPY_PS_OWN      a thread of the target: pokeable, sampleable, never
     *                      released early
     *   ASMSPY_PS_FOREIGN  a different process (a fork child): never poked,
     *                      never sampled, released as soon as its copy is
     *                      restored
     *   ASMSPY_PS_UNKNOWN  /proc would not say: never poked, never sampled, and
     *                      NEVER RELEASED — it stays traced, so it cannot die of
     *                      our byte — and it costs COVERAGE, so nothing new is
     *                      armed either.
     *
     * What each of those means for a task that is STOPPED is not decided here or
     * anywhere near here: it is the kind axis of asmspy_ps_decide's table. */
    asmspy_ps_kind_t kind;
    /* For a fork child specifically: the entry whose trap BYTE it inherited in
     * its copy-on-write text, and the original word to put back. A child that is
     * released still carrying our int3 dies of SIGTRAP with no tracer — the same
     * fatality as an unseized thread, one address space over. 0 = nothing
     * inherited (a vfork child SHARES the mm, so the parent's disarm covers it
     * and restoring through the child would instead disarm the parent). */
    uint64_t copied_bp;
    long copied_orig;
} ps_thr_t;

typedef struct {
    pid_t pid;
    ps_thr_t t[PS_TABLE_SLOTS];
    int n;
    /* Is the thread set FULLY ours? Set false by anything that leaves a task of
     * the target untraced: the PS_MAX_THREADS cap, a PTRACE_SEIZE refused for
     * anything but ESRCH, a seize scan that never converged, or a table-full
     * detach of a followed child. Phase 3 is skipped outright when this is
     * false — see PS_MAX_THREADS and asmspy_ps_arm_note. */
    int covered;
    int cap; /* PS_MAX_THREADS, or the ASMSPY_PS_TEST_CAP lever */
    /* Teardown suppresses the PTRACE_LISTEN branch. MEASURED (a standalone
     * probe, 2026-08-06): PTRACE_DETACH on a tracee in LISTEN is refused with
     * ESRCH; PTRACE_INTERRUPT then re-reports the group-stop, and DETACH from
     * THAT succeeds and leaves the process in state 'T', still job-control
     * stopped. Without this flag the retry re-LISTENed the thread on every
     * round, `stopped` could never become 1, and a merely ^Z'd target burned
     * 3 x 200 ms and came back as a false "could not be handed back clean". */
    int tearing_down;
    /* A trap byte we could NOT get back out. Nothing may arm after this, and the
     * call must not return a clean result: the target is holding an int3 that
     * will kill it on its next arrival, seconds after we are gone. */
    int leaked;
    /* The entry this candidate is using, and whether the trap BYTE is in the
     * text right now. These are deliberately two facts, not one, and the split
     * is load-bearing: a thread can be queued at base+1 with its SIGTRAP not
     * yet consumed AFTER we have already restored the byte (during the
     * step-and-rearm, and at the disarm). Collapsing them into "bp_base != 0"
     * made that thread's trap look like the TARGET'S OWN int3 — si_code is
     * SI_KERNEL either way — and re-injecting SIGTRAP into a process with no
     * handler kills it. MEASURED: clone_victim died on every run. So bp_base
     * stays set for the whole candidate (it is what tells us a pc of base+1 is
     * ours to rewind) while bp_planted tracks the byte. */
    uint64_t bp_base;
    int bp_planted;
    long bp_orig; /* the word POKETEXT overwrote              */
    /* Has a thread ARRIVED at the entry currently armed?
     *
     * Set by ASMSPY_PS_ACT_COLLECT and cleared per CANDIDATE (ps_phase3_one),
     * NOT per plant: phase 3 re-plants the same address after every arrival, and
     * the fact being tracked is about the entry, not about one planting of it.
     *
     * Read by exactly one thing — ASMSPY_PS_TEST_TGID_FAIL=armed — and it is
     * here rather than in the test because the fact is only knowable from
     * inside. The lever exists to reach "a task of unknown kind EXECUTES our
     * armed int3", and MEASURED, gating it on `bp_planted` alone could not: an
     * unknown costs coverage, coverage stops the arming loop, so the scenario
     * gets exactly ONE armed window — and the windows that stay armed longest
     * are precisely the candidates NOTHING enters (a hot entry collects its four
     * arrivals in microseconds and is disarmed; a never-entered one holds the
     * full ASMSPY_PS_CONFIRM_MS). Spending that one window on an entry no task
     * reaches made the whole check vacuous: 0 of 3 runs reached the path.
     * Gating on "an arrival has already been observed HERE" spends it on an
     * entry the target is demonstrably executing. */
    int bp_arrived;
    /* The LAST address we ever armed, and its original word — never cleared for
     * the life of the call, only overwritten by the next plant.
     *
     * `bp_base` is cleared by ps_disarm, and a task whose mm was snapshotted
     * while that candidate was armed can be TABLED after the clear: ptrace(2)
     * does not order a child's attach-stop against anything. Keying the copy off
     * `bp_base` therefore recorded "nothing to restore" for a child that is
     * holding an int3 — which is the same wait-ordering assumption this module
     * already rejected once, for foreignness. */
    uint64_t armed_bp;
    long armed_orig;
    pid_t stepping; /* the tid whose PTRACE_SINGLESTEP we await */

    /* TEST INSTRUMENTATION for the three shapes the levers manufacture, and the
     * reason it lives here rather than in the test: each one is a fact only the
     * inside knows, and without it every check built on those levers would pass
     * just as happily on a run where the window caught nobody. That is the
     * hollowing this suite has been bitten by four times.
     *
     *   in_drain      set only while ps_drain is consuming;
     *   drain_traps   OUR traps reaped by that drain — the queued arrival the
     *                 disarm would otherwise have carried past bp_base;
     *   late_traps    OUR traps recognised through armed_bp with bp_base
     *                 already cleared — the residual the fallback exists for;
     *   late_detached tasks detached on a SECOND or later teardown pass, i.e.
     *                 tabled after a pass had already taken its snapshot.
     *
     * Reported only when a lever is set (asmspy_ptrace_sample's tail). */
    int in_drain;
    unsigned drain_traps, late_traps, late_detached;
    /* ASMSPY_PS_TEST_STEAL_BYTE: guard_held counts restores that looked at the
     * byte and correctly declined to write; blind_writes counts restores that
     * wrote over something that was not ours. */
    unsigned guard_held, blind_writes;
    /* ASMSPY_PS_TEST_STOP_COST_US: sweep_us is the time spent in stop sweeps
     * that carried a REAL budget (the teardown's PS_STOP_FOREVER sweeps are not
     * counted — they are deliberately unbounded); sweep_clamped is how many of
     * those hit their budget and returned early, and sweep_over how many
     * overran it, which after the fix must be none. */
    unsigned long long sweep_us;
    unsigned sweep_clamped, sweep_over;
} ps_ctx_t;

/* One residency bucket: a FUNCTION START and how many samples landed in its
 * body. Folded at capture time (see PS_MAX_SYMS). */
typedef struct {
    uint64_t start;
    unsigned long long count;
} ps_sym_hit_t;

/* A candidate through phases 2 and 3. `first_us`/`arrivals` are phase 3's
 * measurements; `residency` is kept only for the report — correction 6 is
 * explicit that it does not earn a rank. */
typedef struct {
    uint64_t addr, size;
    const char *name, *module;
    unsigned long long residency;
    unsigned long long arrivals;
    unsigned long long first_us;
    /* Direct CALL instructions phase 2 found naming this entry. 0 means the
     * candidate arrived here by residency alone, which is also the only thing
     * that distinguishes "phase 2 ran" from "phase 2 was deleted" in the
     * output — and it had to be distinguishable, because MEASURED, deleting
     * phase 2 still leaves auto_victim's correct winner standing (phase 1 does
     * land ~5% of its samples inside entered_often, and phase 3 drops
     * grind_forever on its own). Without this field the phase would have been
     * shipped untested. */
    unsigned sites;
} ps_cand_t;

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

/* CLOCK_MONOTONIC throughout: a CLOCK_REALTIME deadline lets settimeofday/NTP
 * jump every bound in either direction, and one of these bounds is the ranking
 * key itself. */
static long long ps_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ */
/* thread table                                                        */
/* ------------------------------------------------------------------ */

/* APPEND a note to `why` rather than overwriting it. More than one thing can be
 * worth saying about a single window — "no Capstone" AND "the thread set was not
 * fully seized" are independent facts, and an assignment would silently drop
 * whichever fired second. */
static void ps_note(char *why, size_t whylen, const char *msg) {
    if (!why || !whylen || !msg)
        return;
    size_t used = strnlen(why, whylen);
    if (used + 1 >= whylen)
        return;
    if (used)
        snprintf(why + used, whylen - used, "; %s", msg);
    else
        snprintf(why, whylen, "%s", msg);
}

/* The THREAD GROUP this task belongs to, or -1. The one question that separates
 * "another thread of our target" from "a process that forked out of it".
 * `live_trap` is the test lever's trigger, not an input to the answer — see
 * ps_tgid_fail_mode. */
static int ps_tgid_of(pid_t tid, int live_trap);
/* Detach ONE thread, insisting — defined below, needed by the release path in
 * ps_perform, which must not drop a task it has not proved it released. `sig`
 * is delivered as the tracee restarts, and is nonzero ONLY when the stop being
 * left is a signal-delivery-stop (asmspy_ps_decide's RELEASE_SIGNAL). */
static int ps_detach_one(ps_ctx_t *c, pid_t tid, int sig);

static int ps_find(ps_ctx_t *c, pid_t tid) {
    for (int i = 0; i < c->n; i++)
        if (c->t[i].tid == tid)
            return i;
    return -1;
}

/* The table cap, with a TEST LEVER. The truncation path is fatal (see
 * PS_MAX_THREADS) and cannot be provoked from outside without a 513-thread
 * victim, so it could only ever be argued rather than demonstrated.
 * ASMSPY_PS_TEST_CAP=<n> caps the table at `n`, exactly as the engine's
 * ASMSPY_TEST_THR_OOM lever does for its own untabled-task path
 * (cli/asmspy_engine.c:1929). Unset = the real cap.
 *
 * Read once PER CALL into ps_ctx_t::cap, deliberately NOT into a function-local
 * static: a static is read on the first sampler call of the process and never
 * again, so a test that sets the lever between calls silently gets the real cap
 * and its assertions go vacuous. That happened on the first run of this very
 * check. */
static int ps_read_cap(void) {
    const char *e = getenv("ASMSPY_PS_TEST_CAP");
    int cap = (e && *e) ? atoi(e) : PS_MAX_THREADS;
    return (cap <= 0 || cap > PS_MAX_THREADS) ? PS_MAX_THREADS : cap;
}

/* A second TEST LEVER, alongside ps_read_cap: ASMSPY_PS_TEST_LOSE_AFTER=<i>
 * forces `covered` false immediately after phase 3 finishes candidate index
 * <i> (0-based) -- simulating a thread cloned mid-window that pushes the
 * table past its cap AFTER some candidates were already confirmed for real,
 * rather than ASMSPY_PS_TEST_CAP's "never covered from the start".
 *
 * That distinction is load-bearing, not a nicety (2026-08-06 T7 review): a run
 * that loses coverage PART-WAY still has cands[0..i] carrying genuine
 * arrivals/first_us from before the loss, so a caller that infers
 * "confirmed" from cands[0]'s fields alone sees real data and never learns
 * the batch is unconfirmed -- exactly the case ASMSPY_PS_TEST_CAP cannot
 * provoke (it never lets the loop start at all) and the one this lever
 * exists to make demonstrable on demand instead of waiting on a race against
 * a real cloning victim. Unset = never triggers (real behaviour). */
static int ps_read_lose_after(void) {
    const char *e = getenv("ASMSPY_PS_TEST_LOSE_AFTER");
    if (!e || !*e)
        return -1;
    long v = strtol(e, NULL, 10);
    return v >= 0 ? (int)v : -1;
}

/* A THIRD TEST LEVER: ASMSPY_PS_TEST_LATE_TRAP_MS=<n> makes ps_disarm resume
 * ONE thread it had just stopped and then idle `n` ms WITHOUT pumping, with the
 * entry trap still armed.
 *
 * It manufactures, on demand, the one shape the whole trap-provenance split
 * exists for and that no victim in this tree produces on cue: a task of the
 * target executing our int3 in the narrow window between the disarm's last pump
 * and the byte coming back out, so its SIGTRAP is QUEUED but not yet reaped
 * while the candidate is being torn down. In the wild that task is a thread
 * cloned after ps_stop_all's round-2 snapshot (2026-08-06 final review,
 * finding 1) — tabled by the pump, resumed RESUME_QUIET into an address space
 * whose int3 is still planted, and no test could reach it because it depends on
 * a clone landing inside a window microseconds wide. Resuming a thread we
 * already hold reaches the identical state deterministically: from the module's
 * point of view a thread it did not stop is a thread it did not stop.
 *
 * Unset/0 = never fires (real behaviour); the resume is the ONLY thing this
 * changes, and every disposition downstream of it is production code. */
static long ps_read_late_trap_ms(void) {
    const char *e = getenv("ASMSPY_PS_TEST_LATE_TRAP_MS");
    if (!e || !*e)
        return 0;
    long v = strtol(e, NULL, 10);
    return v > 0 ? v : 0;
}

/* A FOURTH TEST LEVER: ASMSPY_PS_TEST_SKIP_DRAIN=1 skips ps_disarm's drain.
 *
 * The drain closes the window above by reaping what is already queued while
 * `bp_base` still names the trap. It cannot close it completely: a task that
 * executed the int3 microseconds before the byte came out may not have finished
 * entering its stop by the time waitpid(WNOHANG) asks, so its status surfaces
 * only AFTER the disarm has cleared bp_base. That residual is exactly what
 * ps_trap_addr's armed_bp fallback is for, and with the drain in place nothing
 * in the suite would ever exercise it — this lever makes "the status was not
 * visible yet" reachable so the fallback is tested rather than assumed. */
static int ps_read_skip_drain(void) {
    const char *e = getenv("ASMSPY_PS_TEST_SKIP_DRAIN");
    return (e && *e && *e != '0') ? 1 : 0;
}

/* A FIFTH TEST LEVER: ASMSPY_PS_TEST_STEAL_BYTE=1 overwrites the armed byte
 * with something that is neither our trap nor the original, through a thread we
 * hold, immediately before ps_unplant — and checks afterwards whether the
 * restore wrote anyway.
 *
 * That is the A29 question in one measurement (2026-08-06 final review, finding
 * 5). Between the plant and the disarm the target has been RUNNING for up to
 * ASMSPY_PS_CONFIRM_MS, so a dlclose or an mmap can leave a different mapping at
 * that address; PTRACE_POKETEXT succeeds anyway (FOLL_FORCE), splicing a stale
 * word into live text. Waiting for a real remap inside a 50 ms window is not a
 * test, so the byte is changed by hand, with every thread stopped, and put back
 * before anything runs — the target's text is identical either side of the
 * probe, and the only thing observed is whether the restore looked first. */
static int ps_read_steal_byte(void) {
    const char *e = getenv("ASMSPY_PS_TEST_STEAL_BYTE");
    return (e && *e && *e != '0') ? 1 : 0;
}

/* A SIXTH TEST LEVER: ASMSPY_PS_TEST_STOP_COST_US=<n> charges `n` microseconds
 * to every ps_ensure_stopped that actually has to stop something.
 *
 * The cost being modelled is real and this repo has already measured its cause:
 * a thread in uninterruptible sleep pays PS_STOP_US (200 ms), stays in the
 * table, and is paid for again by every later sweep — and amdgpu putting
 * threads in D state is in this repo's own host notes. A victim cannot be asked
 * to enter D state on demand, so the cost is injected instead. Nothing else
 * changes: the sweep still stops what it can, in the same order. */
static long ps_read_stop_cost_us(void) {
    const char *e = getenv("ASMSPY_PS_TEST_STOP_COST_US");
    if (!e || !*e)
        return 0;
    long v = strtol(e, NULL, 10);
    return v > 0 ? v : 0;
}

/* A SEVENTH TEST LEVER: ASMSPY_PS_TEST_WALK_PUMP_MS=<n> makes ps_detach_all's
 * FIRST teardown pass pump for `n` ms after taking its snapshot of tids.
 *
 * That is not an artificial insertion — it is what the walk already does. Every
 * ps_detach_one that has to retry calls ps_ensure_stopped, which pumps, and the
 * pump TABLES whatever reports, including a clone the target made a microsecond
 * ago. A task tabled after the snapshot is a task the pass will never detach
 * (2026-08-06 final review, finding 4), and the old walk then zeroed the table
 * and reported the target handed back clean while that task stayed seized with
 * the pump about to disappear. Reproducing it on a real target means waiting for
 * a clone to land inside a window microseconds wide; this widens the window and
 * changes nothing else. Unset/0 = never fires. */
static long ps_read_walk_pump_ms(void) {
    const char *e = getenv("ASMSPY_PS_TEST_WALK_PUMP_MS");
    if (!e || !*e)
        return 0;
    long v = strtol(e, NULL, 10);
    return v > 0 ? v : 0;
}

/* Add `tid`, or find it.
 *
 * Two different limits, and conflating them is what made a followed clone get
 * DETACHED into an armed process. Exceeding the seize CAP means "we no longer
 * hold the whole thread set", which is a coverage fact — the task is still
 * tabled, out of the headroom above, because a task we trace can never die of
 * our trap. Only exhausting the physical SLOTS is a real refusal, and every
 * caller must then treat the task as not ours. */
static int ps_add(ps_ctx_t *c, pid_t tid) {
    int i = ps_find(c, tid);
    if (i >= 0)
        return i;
    if (c->n >= c->cap)
        c->covered = 0;
    if (c->n >= PS_TABLE_SLOTS)
        return -1;
    memset(&c->t[c->n], 0, sizeof c->t[c->n]);
    c->t[c->n].tid = tid;
    /* CLASSIFY FOREIGNNESS HERE, from /proc, not from the fork event.
     *
     * ptrace(2) does not order the parent's PTRACE_EVENT_FORK stop against the
     * CHILD's own attach-stop, so a child could arrive first and be tabled as an
     * ordinary thread of the target. It would then be treated as an ARRIVAL at
     * our entry, re-planted through — into a DIFFERENT ADDRESS SPACE — and
     * eventually released still carrying the int3, or handed the SIGTRAP back
     * (si_code is SI_KERNEL whoever planted it) into a process with no handler.
     * The task's own Tgid settles it with no ordering to reason about.
     *
     * AND UNREADABLE /proc IS ITS OWN ANSWER. It used to collapse into FOREIGN,
     * on the reasoning that "misreading a thread as foreign costs a sample and an
     * early disarm" — which is true of ps_phase1 and ps_can_poke and FALSE of the
     * release path, and the release path is the one that runs: it detached the
     * task and dropped it from the table without costing coverage, so the arm
     * gate still passed and an int3 went into text a now-untraced thread was
     * about to execute. An unknown is not a licence to act. */
    if (tid != c->pid) {
        int tgid = ps_tgid_of(tid, c->bp_planted && c->bp_arrived);
        if (tgid < 0) {
            c->t[c->n].kind = ASMSPY_PS_UNKNOWN;
            c->covered = 0; /* we cannot prove we hold the thread set */
        } else if (tgid != (int)c->pid) {
            c->t[c->n].kind = ASMSPY_PS_FOREIGN;
        }
    }
    /* A trap has been armed at some point in this call, so any task not in OUR
     * address space may be holding a copy-on-write copy of it. `armed_bp`, not
     * `bp_base`: bp_base is cleared at each disarm, and this task's stop may be
     * dispatched after that even though its mm snapshot predates it. The restore
     * itself is guarded by ps_restore_copy, which writes only if our byte is
     * actually there — so a stale address costs nothing.
     *
     * RESIDUAL, recorded rather than closed (alongside A26): `armed_bp` is read
     * at INSERT time, so it can be NEWER than the byte this task actually
     * inherited if a re-plant lands between the fork and the child's first stop.
     * The restore would then look for a trap at an address the child never
     * copied, find none (ps_restore_copy reads first), and leave the inherited
     * one in place. What makes it not bite is that every plant is preceded by
     * ps_stop_all, whose pump drains the pending attach-stops first — so the
     * child is tabled before the next address exists. Closing it properly needs
     * a per-task snapshot taken at fork time, which is the ordering ptrace(2)
     * does not define and which this module has already been burned by twice. */
    if (c->t[c->n].kind != ASMSPY_PS_OWN && c->armed_bp) {
        c->t[c->n].copied_bp = c->armed_bp;
        c->t[c->n].copied_orig = c->armed_orig;
        /* A VFORK child SHARES the mm, so this restore disarms the PARENT's
         * trap rather than a private copy — the candidate loses its remaining
         * arrivals. That is deliberate: the alternative is to distinguish fork
         * from vfork, which is only knowable from an event whose ordering
         * against this stop ptrace(2) does not define. Losing a measurement is
         * survivable; releasing a task that dies of our byte is not. */
    }
    return c->n++;
}

static void ps_del(ps_ctx_t *c, pid_t tid) {
    int i = ps_find(c, tid);
    if (i < 0)
        return;
    c->t[i] = c->t[--c->n]; /* swap-with-last: order is not meaningful here */
}

/* Resume a stop the pipeline has already classified. `sig` is what
 * asmspy_ps_decide's cell chose to deliver — 0 ONLY for a stop that was ours to
 * begin with, or one whose signal an unknown kind forbids us to carry. */
static void ps_cont(ps_ctx_t *c, pid_t tid, int sig) {
    if (ptrace(PTRACE_CONT, tid, NULL, (void *)(uintptr_t)sig) == 0) {
        int i = ps_find(c, tid);
        if (i >= 0)
            c->t[i].stopped = 0;
    }
}

/* ------------------------------------------------------------------ */
/* /proc predicates                                                    */
/* ------------------------------------------------------------------ */

static int ps_read_file(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

/* Force ps_tgid_of to fail, so the ASMSPY_PS_UNKNOWN path can be reached at all.
 *
 * It is otherwise unreachable from outside: /proc/<tid>/status is readable for
 * any task we have seized, and the real trigger is an fd exhaustion or a
 * /proc-less namespace in the CALLING process. Same lever pattern as
 * ASMSPY_PS_TEST_CAP above and the engine's ASMSPY_TEST_THR_OOM — and the same
 * justification: the branch is FATAL when it resolves the wrong way (it released
 * a real thread into an armed address space), so "we could only argue about it"
 * is not good enough.
 *
 * TWO MODES, and the second one exists because the first could not reach the
 * defect it was written for. "1" fails every read, INCLUDING the ones during
 * ps_seize_all — so coverage is gone before the arm gate and phase 3 never
 * plants a byte. The whole fatal path (an unknown task EXECUTING our armed
 * int3) therefore sat behind a lever that guaranteed nothing was ever armed.
 * "armed" fails a read only while a trap byte is in the target's text AND a
 * thread has already been seen arriving at it (ps_ctx_t::bp_arrived) — which is
 * precisely the scenario the module's own comment cites: one failed open(2) in a
 * long-lived caller at its fd limit, AFTER the arm, at an entry the target is
 * demonstrably executing. The second half of that condition is not fussiness;
 * without it the lever fired inside windows nothing ever reaches, and the path
 * it exists to reach was executed 0 times in 3 runs. See ps_ctx_t::bp_arrived. */
#define PS_TGID_FAIL_OFF   0
#define PS_TGID_FAIL_ALL   1
#define PS_TGID_FAIL_ARMED 2
static int ps_tgid_fail_mode(void) {
    const char *e = getenv("ASMSPY_PS_TEST_TGID_FAIL");
    if (!e || !*e || *e == '0')
        return PS_TGID_FAIL_OFF;
    return *e == 'a' ? PS_TGID_FAIL_ARMED : PS_TGID_FAIL_ALL;
}

/* The thread group `tid` belongs to, or -1 when /proc cannot answer. */
static int ps_tgid_of(pid_t tid, int live_trap) {
    char path[64], buf[1024];
    int mode = ps_tgid_fail_mode();
    if (mode == PS_TGID_FAIL_ALL || (mode == PS_TGID_FAIL_ARMED && live_trap))
        return -1;
    snprintf(path, sizeof path, "/proc/%d/status", (int)tid);
    if (ps_read_file(path, buf, sizeof buf) <= 0)
        return -1;
    const char *p = strstr(buf, "\nTgid:");
    if (!p)
        return -1;
    return atoi(p + 6);
}

/* Is `tid` EXECUTING right now — i.e. worth interrupting for a PC sample?
 *
 * CORRECTION 3, and it cost a day to find. The obvious discriminator is
 * /proc/<tid>/stat's utime+stime delta, and at this window it is not a weak
 * signal, it is NO signal: CLK_TCK is 100 (10 ms ticks) against a 400 ms
 * window, and MEASURED, every single thread of threads_victim reports a delta
 * of 0. /proc/<tid>/schedstat, the natural fallback, reads "0 0 1" — nanosecond
 * accounting is off unless kernel.sched_schedstats=1, which needs root.
 *
 * What DOES discriminate is /proc/<tid>/syscall, readable here precisely
 * because we are the tracee's tracer (it needs PTRACE_MODE_ATTACH): it prints
 * the literal "running" for a thread executing user code, and a syscall number
 * plus its arguments for one blocked in the kernel. That is the exact
 * distinction a residency sample needs, at no time resolution at all.
 *
 * CORRECTION 6, on the 'R' prefilter below: it is here for COST and is credited
 * with nothing else. 100 threads cost the same as 1 (0.27% of the window), and
 * with the filter ON the residency ranking still put clock_nanosleep above
 * `worker` 11:3 — a thread caught 'R' on its way in or out of a syscall is
 * genuinely runnable and genuinely the wrong answer. Accuracy is bought in
 * phase 3, not here. */
static int ps_thread_executing(pid_t pid, pid_t tid) {
    char path[96], buf[512];

    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", (int)pid, (int)tid);
    if (ps_read_file(path, buf, sizeof buf) > 0) {
        /* The comm field is parenthesised and may itself contain spaces and
         * parens ("(sh (weird))"), so the state is found from the LAST ')'. */
        char *p = strrchr(buf, ')');
        if (p && p[1] == ' ' && p[2] != 'R')
            return 0;
    }

    snprintf(path, sizeof path, "/proc/%d/task/%d/syscall", (int)pid, (int)tid);
    if (ps_read_file(path, buf, sizeof buf) <= 0)
        return 0; /* unreadable: not attached yet, or the thread is gone */
    return strncmp(buf, "running", 7) == 0;
}

/* ------------------------------------------------------------------ */
/* the entry breakpoint (phase 3)                                      */
/* ------------------------------------------------------------------ */

/* Plant the shared entry trap through a STOPPED thread. Same encoding split as
 * the region engine's rgn_plant_bp (cli/asmspy_engine.c): a one-byte int3 on
 * x86-64, a four-byte `brk #0` on AArch64, spliced into the peeked word. */
static int ps_plant(ps_ctx_t *c, pid_t tid, uint64_t base) {
    errno = 0;
    long o = ptrace(PTRACE_PEEKTEXT, tid, (void *)(uintptr_t)base, NULL);
    if (o == -1 && errno != 0)
        return -1;
#if defined(__aarch64__)
    long trap = (o & ~0xffffffffL) | (long)0xd4200000L;
#else
    long trap = (o & ~0xffL) | 0xccL;
#endif
    if (ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
               (void *)(uintptr_t)trap) != 0)
        return -1;
    c->bp_base = base;
    c->bp_orig = o;
    c->bp_planted = 1;
    c->armed_bp = base; /* outlives the disarm on purpose — see ps_ctx_t */
    c->armed_orig = o;
    return 0;
}

/* THE ADDRESS A SIGTRAP OF OURS WOULD HAVE COME FROM, and the word that was
 * there before we wrote to it. `bp_base` while a candidate owns the trap, and
 * `armed_bp` — which ps_plant sets and nothing ever clears — once ps_disarm has
 * given the address back.
 *
 * This IS the trap-provenance question, and asking only `bp_base` was a Critical
 * (2026-08-06 final review, finding 1). A task can execute the int3 and have its
 * SIGTRAP still QUEUED when the byte comes out; ps_disarm then clears bp_base,
 * and the next pump reaps that status with nothing left to recognise it by. It
 * falls through to ps_sigtrap_is_app — which reads SI_KERNEL, which is what OUR
 * int3 reports — and asmspy_ps_decide's OWN x R_APP_TRAP cell re-injects SIGTRAP
 * into a process that has no handler for it. That is the user's process, killed
 * by a byte we planted, after we reported success. `armed_bp` was added for
 * precisely this (see ps_ctx_t) and nothing consulted it.
 *
 * Every site that acts on "the trap" reads it through here rather than through
 * bp_base, because a rewind or a restore aimed at address 0 is not a no-op —
 * it is a thread left parked mid-instruction at base+1. */
static uint64_t ps_trap_addr(const ps_ctx_t *c) {
    return c->bp_base ? c->bp_base : c->armed_bp;
}

static long ps_trap_orig(const ps_ctx_t *c) {
    return c->bp_base ? c->bp_orig : c->armed_orig;
}

/* MAY WE WRITE THE TARGET'S TEXT THROUGH A TASK OF THIS KIND?
 *
 * A switch, not a comparison, and with no `default:` — this and ps_kind_may_
 * sample below are the two places outside ps_perform where a kind reaches a
 * verb, so they are the two places a fourth kind must also stop the build. */
static int ps_kind_may_poke(asmspy_ps_kind_t k) {
    switch (k) {
    case ASMSPY_PS_OWN:
        return 1;
    case ASMSPY_PS_FOREIGN:
        return 0; /* its own mm, or a vfork'd one whose clear disarms the parent */
    case ASMSPY_PS_UNKNOWN:
        return 0; /* an unknown is not a licence to act */
    }
    return 0; /* unreachable while `k` holds an enumerator: -Wswitch is what
               * keeps it that way, and refusing is the safe value regardless */
}

/* MAY WE FOLD THIS TASK'S PC INTO THE TARGET'S RESIDENCY HISTOGRAM? */
static int ps_kind_may_sample(asmspy_ps_kind_t k) {
    switch (k) {
    case ASMSPY_PS_OWN:
        return 1;
    case ASMSPY_PS_FOREIGN:
        return 0; /* different process, different code: folding its pc in is a
                   * lie about the target, and its /proc path is not even ours */
    case ASMSPY_PS_UNKNOWN:
        return 0; /* costs one sample; the other direction costs correctness */
    }
    return 0; /* unreachable — see ps_kind_may_poke */
}

/* Can this table entry POKE the target's text? A thread we actually hold in a
 * ptrace-stop, in the target's own address space. Excludes:
 *   - a running thread          (PEEK/POKETEXT are refused, silently)
 *   - a LISTENed thread         (group-stop, not a stop we hold: ESRCH)
 *   - anything ps_kind_may_poke refuses
 */
static int ps_can_poke(const ps_thr_t *t) {
    return t->stopped && !t->listening && ps_kind_may_poke(t->kind);
}

/* Put the ORIGINAL word back at `base` in `tid`'s address space — but ONLY if
 * our trap byte is actually there.
 *
 * The check is the point, and the audit is what found it. A fork child may have
 * execve'd between the fork and this stop, in which case `base` names a
 * completely different image and the "restore" would splice eight bytes of the
 * parent's old text into it. Writing on the assumption that a byte we have not
 * looked at is ours is the same class of error as every other one in this file:
 * acting on an unknown. Reading first also makes the restore idempotent, which
 * is what lets ps_add key the copy off an address that outlives the disarm.
 *
 * 0 = there is nothing of ours there any more (restored, never was, or that
 *     address does not exist in this task's address space at all);
 * -1 = we could not tell, or could not write. */
static int ps_restore_copy(pid_t tid, uint64_t base, long orig) {
    if (!base)
        return 0;
    errno = 0;
    long cur = ptrace(PTRACE_PEEKTEXT, tid, (void *)(uintptr_t)base, NULL);
    if (cur == -1 && errno != 0) {
        /* NOT MAPPED IS AN ANSWER, and treating it as "could not tell" was a
         * defect of its own: a fork child that execve'd has a brand new image,
         * `base` names nothing in it, and PTRACE_PEEKTEXT fails EIO. The old
         * code returned -1, so the child was kept traced, the teardown counted
         * it as damage, and the whole call came back -1 "do not trace this
         * process further" — for fork+exec, the commonest fork shape there is.
         * A byte in a page that is not mapped is not our int3 by any
         * definition; there is nothing to undo and nothing to report.
         * ESRCH (gone, or not stopped) really is "could not tell". */
        return (errno == EIO || errno == EFAULT) ? 0 : -1;
    }
#if defined(__aarch64__)
    if ((cur & 0xffffffffL) != (long)0xd4200000L)
        return 0; /* not our brk: nothing of ours to undo */
#else
    if ((cur & 0xffL) != 0xccL)
        return 0; /* not our int3: nothing of ours to undo */
#endif
    return ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
                  (void *)(uintptr_t)orig) == 0
               ? 0
               : -1;
}

/* Take the trap byte back out, through any thread that can poke.
 *
 * RETURNS A STATUS, and the caller must not flatten it. This used to be void:
 * if no tabled thread was stopped, or every POKETEXT returned ESRCH, it returned
 * silently with bp_planted still 1 — and the disarm then cleared bp_base
 * unconditionally, so the ADDRESS of the live int3 was gone. The later
 * ps_detach_all saw bp_planted == 1 and poked address ZERO. The sampler returned
 * success and the target died seconds later on its next arrival: precisely the
 * "does not fail loudly" outcome the disarm comment forbids.
 *
 * `bp_base` is deliberately LEFT set on success too — see the ps_ctx_t note:
 * threads may still be queued at base+1 and only bp_base identifies them as
 * ours. 0 = the byte is out (or was never in), -1 = it is still in the target.
 *
 * THROUGH ps_restore_copy, which PEEKs first — A29, the rule the audit added
 * after a blind restore spliced eight bytes of a parent's old text into an
 * execve'd child's new image. This site was the last raw POKETEXT left in the
 * restore path (2026-08-06 final review, finding 5) and it is not immune to the
 * same fact: between ps_plant and here the target has been RUNNING for up to
 * ASMSPY_PS_CONFIRM_MS, and a dlclose/mmap in that window can leave something
 * else mapped at `bp_base`. FOLL_FORCE means the write would SUCCEED, splicing
 * a stale word into live text, and the target then executes garbage and dies
 * after we reported success. Reading first also makes "the byte is out" the
 * honest answer when someone else already took it out. */
static int ps_unplant(ps_ctx_t *c) {
    if (!c->bp_planted)
        return 0;
    for (int i = 0; i < c->n; i++)
        if (ps_can_poke(&c->t[i]) &&
            ps_restore_copy(c->t[i].tid, c->bp_base, c->bp_orig) == 0) {
            c->bp_planted = 0;
            return 0;
        }
    return -1;
}

/* IS `tid` STOPPED AT THE TRAP WE ARMED AT `base`? A pure READ — it is the
 * classification half of what used to be one read-compare-write function, split
 * so that classification cannot mutate a tracee and so the write happens inside
 * a written case of ps_perform.
 *
 * Returns 1 yes, 0 demonstrably not, and -1 if we COULD NOT TELL — a third
 * answer, not a synonym for 0 (A11). A caller that flattens -1 into "not at our
 * trap" hands the thread its SIGTRAP back, and an int3 reports si_code ==
 * SI_KERNEL whoever planted it, so ours becomes indistinguishable from the
 * target's own and a process with no handler dies of it. The table keeps the
 * two apart as ASMSPY_PS_R_OUR_TRAP and ASMSPY_PS_R_TRAP_UNSURE. */
static int ps_trap_where(pid_t tid, uint64_t base) {
    asmspy_regs_t regs;
    if (!base)
        return 0;
    if (asmspy_regs_read(tid, &regs) != 0)
        return -1;
#if defined(__aarch64__)
    /* `brk` faults AT base, so there is nothing to rewind and this is the whole
     * of the question. */
    return asmspy_reg_pc(&regs) == base;
#else
    return asmspy_reg_pc(&regs) == base + 1;
#endif
}

/* THE SAFETY NET, lifted verbatim in spirit from rgn_rewind_from_bp. On x86 a
 * thread trap-stopped just past the int3 has pc == base+1, which is the MIDDLE
 * of the region's first real instruction once the original byte is back:
 * resuming it there executes garbage in a process we do not own.
 *
 * Idempotent and self-checking: it re-reads the pc and moves it ONLY from
 * base+1, so calling it on a thread that is not there does nothing. 0 = the
 * thread is safe to resume, -1 = we could not read or write its registers, in
 * which case NOTHING may resume it (ps_perform holds it instead). */
static int ps_rewind(pid_t tid, uint64_t base) {
    asmspy_regs_t regs;
    if (!base)
        return 0;
#if defined(__aarch64__)
    (void)regs;
    return 0; /* faulted AT base: already where it must resume */
#else
    if (asmspy_regs_read(tid, &regs) != 0)
        return -1;
    if (asmspy_reg_pc(&regs) != base + 1)
        return 0;
    asmspy_set_pc(&regs, base);
    return asmspy_regs_write(tid, &regs) == 0 ? 0 : -1;
#endif
}

/* Is this SIGTRAP the TARGET'S OWN (an int3 it executed, a hardware breakpoint
 * it armed) rather than something ptrace synthesised? The same si_code evidence
 * every other engine here uses (cli/asmspy_engine.c's sigtrap_is_app). A target
 * that drives its own breakpoints — a JIT, a debugger — must run its handler,
 * not silently skip it; a spurious SI_USER/SI_TKILL trap is still swallowed. */
static int ps_sigtrap_is_app(pid_t tid) {
    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, tid, NULL, &si) != 0)
        return 0;
    return si.si_code == SI_KERNEL || si.si_code == TRAP_HWBKPT;
}

/* ------------------------------------------------------------------ */
/* THE DISPOSITION PIPELINE — classify, decide, perform                */
/* ------------------------------------------------------------------ */

/* WHY is this task stopped? A pure READ of the wait status plus, for SIGTRAP
 * only, two questions the status cannot answer: where the pc is, and what
 * si_code says. Nothing here writes to a tracee, so a misclassification can
 * cost a decision but can never itself be the damage.
 *
 * CORRECTION 1 starts here. The prototype had no classifier at all: it
 * waitpid()ed and unconditionally PTRACE_CONTed with sig=0. man 2 ptrace is
 * unambiguous about the cost — "If sig is 0, then a signal is not delivered" —
 * and against a 100 Hz ITIMER_REAL victim that destroyed 89% of the target's
 * SIGALRMs and collapsed its forward progress ~99% (2 utime ticks and 0
 * progress lines per 2 s, against 200/93 both at baseline and after detach).
 * The reason it shipped is that the identical code costs a SIGNAL-FREE spinner
 * about 1%, so the "~1% overhead, no perturbation" figure was true and
 * meaningless.
 *
 * Under PTRACE_SEIZE the cases are distinguishable without guessing:
 *   - PTRACE_EVENT_STOP  => a group-stop, our own PTRACE_INTERRUPT, or a new
 *                           task's attach-stop. Nothing is being delivered.
 *                           (The kernel reports the last two with SIGTRAP and
 *                           a real group-stop with the stopping signal —
 *                           kernel/signal.c:do_jobctl_trap — which is what
 *                           separates JOBCTL from INTERRUPT here.)
 *   - event == 0         => a SIGNAL-DELIVERY-stop. WSTOPSIG is a signal the
 *                           target was about to take, and PTRACE_CONT is what
 *                           delivers it.
 * SIGTRAP needs the extra questions because it is also how our own trap and our
 * own single-step report. */
static asmspy_ps_reason_t ps_classify(ps_ctx_t *c, pid_t w, int st) {
    int sig = WSTOPSIG(st);
    int event = (st >> 16) & 0xff;

    if (event == PTRACE_EVENT_CLONE)
        return ASMSPY_PS_R_CLONE;
    if (event == PTRACE_EVENT_FORK || event == PTRACE_EVENT_VFORK)
        return ASMSPY_PS_R_FORK;
    if (event == PTRACE_EVENT_STOP)
        return (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
                sig == SIGTTOU)
                   ? ASMSPY_PS_R_JOBCTL
                   : ASMSPY_PS_R_INTERRUPT;
    if (event != 0)
        /* An event-stop for an option we never set (EXEC, EXIT, SECCOMP). It
         * cannot be a pending signal, so it has a disposition of its own rather
         * than being swept into the SIGTRAP arm by accident. */
        return ASMSPY_PS_R_EVENT_OTHER;
    if (sig != SIGTRAP)
        return ASMSPY_PS_R_SIGNAL;
    if (c->stepping == w)
        return ASMSPY_PS_R_OUR_STEP;
    uint64_t tb = ps_trap_addr(c);
    if (tb) {
        /* ps_trap_addr, not `bp_planted` and not `bp_base` either. Not
         * bp_planted: a thread can be queued at base+1 with its SIGTRAP not yet
         * consumed AFTER we have already restored the byte (during phase 3's
         * step-and-rearm, and at the disarm). Collapsing the two made that
         * thread's trap look like the TARGET'S OWN — si_code is SI_KERNEL either
         * way — and re-injecting SIGTRAP into a process with no handler kills it.
         * MEASURED: clone_victim died on every run. And not bp_base, which
         * ps_disarm clears per candidate: the same queued-status thread reaped
         * one microsecond later then had no provenance at all. See
         * ps_trap_addr. */
        int at = ps_trap_where(w, tb);
        if (at > 0) {
            if (!c->bp_base)
                c->late_traps++; /* recognised only because armed_bp outlived
                                  * the disarm — see ps_trap_addr */
            else if (c->in_drain)
                c->drain_traps++;
            return ASMSPY_PS_R_OUR_TRAP;
        }
        if (at < 0)
            return ASMSPY_PS_R_TRAP_UNSURE;
    }
    return ps_sigtrap_is_app(w) ? ASMSPY_PS_R_APP_TRAP : ASMSPY_PS_R_STRAY_TRAP;
}

/* THE TABLE. 3 kinds x 11 reasons, every cell written, no `default:` anywhere —
 * see the long note in asmspy_ptracesample.h for why this is a switch and not a
 * chain of `if`s, and mk/cli.mk for the -Werror=switch/-Werror=switch-enum that
 * turns a missing cell into a build failure.
 *
 * Read it as three policies:
 *
 *   OWN      the target's own thread. It is sampled, poked, stepped, and its
 *            signals are re-injected — everything this module does, it does
 *            here.
 *   FOREIGN  a different process holding a copy-on-write copy of our byte.
 *            RELEASE, at the first stop of any kind: restore its copy in its
 *            own address space, rewind it off base+1, detach. Left as it is it
 *            dies of a SIGTRAP with no tracer, the same fatality as an unseized
 *            thread one address space over.
 *   UNKNOWN  /proc would not say. It never poks, never releases, never arms and
 *            never continues-with-a-signal. That last cell is the Critical this
 *            redesign exists for: an unknown at OUR OWN int3 used to fail an
 *            arrival gate that tested `kind == PS_OWN`, fall out of the bottom
 *            of the chain, and be handed ps_cont(SIGTRAP) by a predicate that
 *            reads SI_KERNEL — which is what our int3 reports.
 */
asmspy_ps_act_t asmspy_ps_decide(asmspy_ps_kind_t kind, asmspy_ps_reason_t why,
                                 int tearing_down, int is_hold,
                                 int collecting) {
    switch (kind) {
    /* ---------------------------------------------------------------- */
    case ASMSPY_PS_OWN:
        switch (why) {
        case ASMSPY_PS_R_CLONE:
            /* CORRECTION 2, the half that makes the option useful: table the
             * child so the phase-3 int3 in the SHARED text has a tracer when the
             * new task reaches it. */
            return ASMSPY_PS_ACT_FOLLOW_CHILD;
        case ASMSPY_PS_R_FORK:
            /* A fork/vfork child is deliberately NOT tabled from the parent's
             * event. It is a different PROCESS; it is classified from /proc and
             * released at its OWN attach-stop, which it cannot run past. Tabling
             * it here re-adds a child we may have ALREADY released — ptrace(2)
             * does not order the two stops — and such an entry never leaves,
             * because a detached task never reports again. MEASURED against
             * forkhot_victim: 152 stale tids in a 900 ms window, after which
             * every teardown walk paid PS_STOP_US per corpse. */
            return ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_EVENT_OTHER:
            return ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_JOBCTL:
            /* PTRACE_LISTEN leaves the thread stopped — honouring the stop the
             * user asked for — yet traced. Except during teardown: MEASURED,
             * PTRACE_DETACH on a LISTENed tracee is refused ESRCH, so the retry
             * re-LISTENed on every round and never converged. Then HOLD, and
             * detach out of the group-stop, which leaves the stop in force. */
            return tearing_down ? ASMSPY_PS_ACT_HOLD : ASMSPY_PS_ACT_LISTEN;
        case ASMSPY_PS_R_INTERRUPT:
            return (is_hold || tearing_down) ? ASMSPY_PS_ACT_HOLD
                                             : ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_OUR_STEP:
            return ASMSPY_PS_ACT_HOLD; /* phase 3 re-arms and resumes it */
        case ASMSPY_PS_R_OUR_TRAP:
            /* The arrival. Whether the byte is still planted decides nothing
             * here — only whether someone is collecting. */
            return collecting ? ASMSPY_PS_ACT_COLLECT
                              : ASMSPY_PS_ACT_REWIND_RESUME;
        case ASMSPY_PS_R_TRAP_UNSURE:
            /* A11. We could not read its registers, so we cannot say whose trap
             * this is. Swallowing costs a target that drives its own
             * breakpoints one handler run; delivering ours costs it its life. */
            return ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_APP_TRAP:
            /* The TARGET'S own int3 / hardware breakpoint: delivered, so its
             * signal machinery runs as it would untraced. */
            return ASMSPY_PS_ACT_RESUME_SIGNAL;
        case ASMSPY_PS_R_STRAY_TRAP:
            return ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_SIGNAL:
            return ASMSPY_PS_ACT_RESUME_SIGNAL; /* CORRECTION 1 */
        }
        break;
    /* ---------------------------------------------------------------- */
    case ASMSPY_PS_FOREIGN:
        switch (why) {
        case ASMSPY_PS_R_CLONE:
        case ASMSPY_PS_R_FORK:
        case ASMSPY_PS_R_EVENT_OTHER:
        case ASMSPY_PS_R_JOBCTL:
        case ASMSPY_PS_R_INTERRUPT:
        case ASMSPY_PS_R_OUR_STEP:
        case ASMSPY_PS_R_OUR_TRAP:
        case ASMSPY_PS_R_TRAP_UNSURE:
        case ASMSPY_PS_R_APP_TRAP:
        case ASMSPY_PS_R_STRAY_TRAP:
            /* Release at the FIRST stop of any kind, and before the job-control
             * question in particular: a child's own attach-stop must never be
             * mistaken for a stop the user asked for. A cloned grandchild and a
             * grandchild fork are handled the same way, at their own stops.
             * OUR_STEP and OUR_TRAP are unreachable for a foreign task by
             * construction — we only step tasks we may poke — but "unreachable"
             * is what the last four rounds each said about the cell that then
             * killed something. */
            return ASMSPY_PS_ACT_RELEASE;
        case ASMSPY_PS_R_SIGNAL:
            /* Its OWN signal, and PTRACE_DETACH takes one: delivering it as we
             * let go is what would have happened had we never been here.
             * Restarting from a signal-delivery-stop is the only ptrace-stop
             * where injecting a signal is defined, which is exactly this cell. */
            return ASMSPY_PS_ACT_RELEASE_SIGNAL;
        }
        break;
    /* ---------------------------------------------------------------- */
    case ASMSPY_PS_UNKNOWN:
        switch (why) {
        case ASMSPY_PS_R_CLONE:
            /* Tabling the child is not a verb against a tracee, and it is what
             * keeps the child traced. Its own kind is settled by its own /proc. */
            return ASMSPY_PS_ACT_FOLLOW_CHILD;
        case ASMSPY_PS_R_FORK:
        case ASMSPY_PS_R_EVENT_OTHER:
            return ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_JOBCTL:
            return tearing_down ? ASMSPY_PS_ACT_HOLD : ASMSPY_PS_ACT_LISTEN;
        case ASMSPY_PS_R_INTERRUPT:
            return (is_hold || tearing_down) ? ASMSPY_PS_ACT_HOLD
                                             : ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_OUR_STEP:
            return ASMSPY_PS_ACT_HOLD; /* we never step one; hold if we did */
        case ASMSPY_PS_R_OUR_TRAP:
            /* THE CRITICAL THIS REDESIGN CLOSES. This task is stopped at a byte
             * WE put there, and we do not know whose address space it is in.
             *
             * Not RESUME_SIGNAL: our int3 reports si_code == SI_KERNEL, so the
             * old fall-through's "is it the app's own trap?" test said yes about
             * OURS and delivered SIGTRAP to a process with no handler — and did
             * not rewind, so even a swallowed one resumed at base+1, in the
             * middle of an instruction.
             *
             * Not COLLECT: an unknown never confirms a candidate.
             * Not RELEASE: an unknown is never handed back early; it stays
             * traced, so it cannot meet a trap untraced.
             * Not plain REWIND_RESUME: the byte is still in whatever text this
             * task is executing, so it would trap again immediately, forever.
             *
             * CLEAR_AND_RESUME is the one disposition that is correct in all
             * three address spaces at once, and it is only correct because
             * ps_restore_copy READS BEFORE IT WRITES (A29): same mm — the
             * candidate disarms early, arrivals lost, nobody dies; a private COW
             * copy — exactly the restore that copy needed; a re-exec'd image
             * where our byte is not there — nothing is written at all. */
            return ASMSPY_PS_ACT_CLEAR_AND_RESUME;
        case ASMSPY_PS_R_TRAP_UNSURE:
        case ASMSPY_PS_R_APP_TRAP:
        case ASMSPY_PS_R_STRAY_TRAP:
            /* SWALLOWED. These are the cells the "an unknown never continues
             * with a signal" rule is actually about, and the signal in question
             * is always SIGTRAP — the one signal whose PROVENANCE is genuinely
             * unknowable here, because our own int3 and the target's own report
             * the identical si_code (SI_KERNEL). Delivering costs the task its
             * life if the trap was ours; swallowing costs a target that drives
             * its own breakpoints one handler run. */
            return ASMSPY_PS_ACT_RESUME_QUIET;
        case ASMSPY_PS_R_SIGNAL:
            /* RE-INJECTED, and this cell is deliberately NOT grouped with its
             * neighbours above.
             *
             * ps_classify returns ASMSPY_PS_R_SIGNAL only for `event == 0 &&
             * sig != SIGTRAP` — an ordinary signal-delivery-stop — and THIS
             * MODULE NEVER SENDS A SIGNAL TO ANY TRACEE. So a signal arriving
             * here is provably not ours in any of the three address spaces this
             * task might be in, and whose address space it is in does not enter
             * into it. Re-injecting is the NO-OP: it is what would have happened
             * had we never attached. Swallowing is the intervention.
             *
             * This cell first shipped as a swallow, on a blanket reading of the
             * rule, and that was a regression: the module's own named triggers
             * for an unclassifiable task (fd pressure in a long-lived caller, a
             * transient /proc read failure) make EVERY non-leader thread
             * UNKNOWN, `kind` is sticky for the whole call, and phase 1 still
             * runs its full window over the target. That is correction 1's
             * 89%-SIGALRM-loss scenario at full scale, inflicted on a target
             * whose only offence was that we could not name it. Losing coverage
             * degrades OUR measurement; it is not a licence to degrade the
             * target's signal stream. */
            return ASMSPY_PS_ACT_RESUME_SIGNAL;
        }
        break;
    }
    /* Unreachable while `kind` and `why` hold enumerators, which is what
     * -Werror=switch/-switch-enum enforces at every call site. HOLD is the only
     * act that touches nothing: even the impossible tail may not act. */
    return ASMSPY_PS_ACT_HOLD;
}

/* PERFORM one act. The only place in this file a stop turns into a syscall.
 *
 * Every index is re-resolved from the tid after anything that can pump, because
 * ps_del swaps-with-last: an index captured before a pump can name a DIFFERENT
 * task afterwards, or none. That is not hypothetical — the release path used to
 * hold one across ps_detach_one. */
static int ps_perform(ps_ctx_t *c, pid_t w, int sig, asmspy_ps_act_t act,
                      pid_t hold, pid_t *arrived) {
    switch (act) {
    case ASMSPY_PS_ACT_HOLD:
        /* No verb at all: the task stays in the stop we hold, and whoever asked
         * for it (ps_ensure_stopped, ps_pump, the teardown) resumes or detaches
         * it later. */
        return w == hold ? PS_EV_STOPPED : PS_EV_NONE;

    case ASMSPY_PS_ACT_RESUME_QUIET:
        ps_cont(c, w, 0);
        return PS_EV_NONE;

    case ASMSPY_PS_ACT_RESUME_SIGNAL:
        ps_cont(c, w, sig);
        return PS_EV_NONE;

    case ASMSPY_PS_ACT_REWIND_RESUME:
        /* ps_trap_addr, for the reason ps_classify uses it: this act is reached
         * for a trap the classifier proved was OURS, and after a disarm that
         * proof came from armed_bp. Rewinding against a cleared bp_base is
         * ps_rewind(w, 0) — a no-op that returns 0, so the thread is CONTed
         * from base+1, i.e. resumed one byte into an instruction. */
        if (ps_rewind(w, ps_trap_addr(c)) != 0)
            return PS_EV_NONE; /* could not make it safe to run: HOLD it, and
                                * let the teardown try again */
        ps_cont(c, w, 0);
        return PS_EV_NONE;

    case ASMSPY_PS_ACT_COLLECT:
        if (ps_rewind(w, ps_trap_addr(c)) != 0)
            return PS_EV_NONE; /* as above: never report an arrival we could not
                                * make resumable — phase 3 would step it from
                                * base+1, mid-instruction */
        c->bp_arrived = 1; /* this entry is live — see ps_ctx_t::bp_arrived */
        if (arrived)
            *arrived = w;
        return PS_EV_ARRIVED;

    case ASMSPY_PS_ACT_CLEAR_AND_RESUME:
        /* Read-guarded: writes only where OUR byte actually is. See the
         * ASMSPY_PS_UNKNOWN/OUR_TRAP cell. bp_planted is deliberately not
         * cleared — we do not know whether this cleared the target's own text or
         * a private copy of it, and a redundant restore later is free while a
         * skipped one leaks. */
        ps_restore_copy(w, ps_trap_addr(c), ps_trap_orig(c));
        if (ps_rewind(w, ps_trap_addr(c)) != 0)
            return PS_EV_NONE;
        ps_cont(c, w, 0);
        return PS_EV_NONE;

    case ASMSPY_PS_ACT_FOLLOW_CHILD: {
        unsigned long child = 0;
        if (ptrace(PTRACE_GETEVENTMSG, w, NULL, &child) == 0 && child) {
            /* Tabled HERE so it is followed even if its own attach-stop is
             * delayed. Do NOT resume the child from here — its stop surfaces in
             * this same loop and is handled there. */
            if (ps_add(c, (pid_t)child) < 0)
                c->covered = 0; /* a task of the target we cannot follow */
        }
        ps_cont(c, w, 0);
        return PS_EV_NONE;
    }

    case ASMSPY_PS_ACT_LISTEN: {
        int i = ps_find(c, w);
        if (i < 0)
            return PS_EV_NONE;
        if (ptrace(PTRACE_LISTEN, w, NULL, NULL) == 0) {
            /* `listening`, NOT `stopped`: a LISTENed tracee is in group-stop,
             * which is not a ptrace-stop we hold, so PEEK/POKE/CONT on it all
             * fail ESRCH. See ps_thr_t. */
            c->t[i].listening = 1;
            c->t[i].stopped = 0;
            return PS_EV_NONE;
        }
        /* LISTEN refused: an ordinary resume is wrong for job control but
         * INFINITELY better than a thread nothing ever resumes, which is the
         * only other option here. */
        ps_cont(c, w, 0);
        return PS_EV_NONE;
    }

    case ASMSPY_PS_ACT_RELEASE:
    case ASMSPY_PS_ACT_RELEASE_SIGNAL: {
        int i = ps_find(c, w);
        if (i < 0)
            return PS_EV_NONE;
        uint64_t cb = c->t[i].copied_bp;
        long co = c->t[i].copied_orig;
        /* Restore whatever copy of ours it holds, rewind it off base+1, and hand
         * it back. Both steps are CHECKED, and the task leaves the table only
         * once it is provably no longer ours: an earlier version discarded
         * PTRACE_DETACH's result and ps_del'd regardless, which drops a
         * possibly-still-seized task out of the table — after which nothing
         * pumps it and teardown never retries it. A permanently frozen child,
         * from a return value nobody read. */
        if (ps_restore_copy(w, cb, co) != 0)
            return PS_EV_NONE; /* keep it TRACED (so its trap is ours to absorb)
                                * and stopped; the teardown retries */
        ps_rewind(w, cb ? cb : ps_trap_addr(c));
        if (ps_detach_one(c, w,
                          act == ASMSPY_PS_ACT_RELEASE_SIGNAL ? sig : 0) == 0)
            ps_del(c, w);
        else {
            /* still ours: teardown retries. Re-find, because ps_detach_one
             * pumps and the pump can ps_del another task into this slot. */
            i = ps_find(c, w);
            if (i >= 0)
                c->t[i].stopped = 1;
        }
        return PS_EV_NONE;
    }
    }
    /* Unreachable while `act` holds an enumerator. No verb here either. */
    return PS_EV_NONE;
}

/* A stop from a task we could not TABLE, because the physical slots are
 * exhausted (PS_TABLE_SLOTS: the cap plus its headroom, i.e. a >528-task target
 * that grew past both inside one window).
 *
 * This is the ONE disposition that is not in asmspy_ps_decide's table, and it is
 * outside it for a reason that is itself the rule: with no table entry there is
 * nowhere to record which address space the task is in, so it HAS no kind. What
 * is left is the only action that is safe without one. `covered` is already
 * false, so no FURTHER candidate will arm — but the one armed RIGHT NOW still
 * is, and releasing an untraced task into an armed address space is the death
 * described at PS_MAX_THREADS. */
static int ps_release_untabled(ps_ctx_t *c, pid_t w) {
    if (ps_trap_addr(c)) {
        /* READ BEFORE WRITING (A29), which this path used to be the last place
         * not to do. It POKETEXTed bp_orig into a task whose address space it
         * had just finished saying it could not identify — so if that task had
         * execve'd, or was simply a different image, those were eight bytes of
         * someone else's text, written on the assumption that a byte we had not
         * looked at was ours. ps_restore_copy needs no table entry, so nothing
         * ever justified the raw poke here. Same mm (a thread): the candidate is
         * disarmed early — arrivals lost, nobody dies. Different mm (a fork
         * child): exactly the restore it needed. bp_planted is deliberately NOT
         * cleared, because we do not know which case this was, and a redundant
         * restore later is free while a skipped one leaks. */
        ps_restore_copy(w, ps_trap_addr(c), ps_trap_orig(c));
        ps_rewind(w, ps_trap_addr(c));
    }
    /* AUDIT NOTE (A26): the ONLY PTRACE_DETACH in this file whose result is not
     * checked and retried, and it is not checked because there is nowhere to
     * record the task — ps_detach_one works through the table. A failure here
     * leaves a task seized with nothing pumping it. Left as a documented
     * residual rather than closed with an untestable second overflow list;
     * every other release path in the file is checked. */
    ptrace(PTRACE_DETACH, w, NULL, NULL);
    return PS_EV_NONE;
}

/* Consume ONE waitpid result: table it, classify it, decide, act. `hold` is the
 * tid whose stop the caller wants kept (0 = none).
 *
 * There is deliberately no logic here beyond the three calls and the two facts
 * that precede them. Everything that used to be a chain of `if`s with a
 * fall-through tail — the tail every one of this module's four Criticals landed
 * in — is now a cell in a total table. */
static int ps_dispatch(ps_ctx_t *c, pid_t w, int st, pid_t hold,
                       pid_t *arrived) {
    if (WIFEXITED(st) || WIFSIGNALED(st)) {
        /* Not a stop: no address space, no kind, no verb owed. */
        ps_del(c, w);
        return PS_EV_GONE;
    }
    if (!WIFSTOPPED(st))
        return PS_EV_NONE;

    /* A followed clone's very first stop is where we learn its tid. */
    int idx = ps_add(c, w);
    if (idx < 0)
        return ps_release_untabled(c, w);

    /* We hold a ptrace-stop for this task from here until an act ends it. Even a
     * group-stop reported as an event-stop is one we hold — that is why
     * PTRACE_LISTEN works on it — and ASMSPY_PS_ACT_LISTEN is what hands it
     * back. */
    c->t[idx].stopped = 1;
    c->t[idx].listening = 0;

    asmspy_ps_kind_t kind = c->t[idx].kind;
    asmspy_ps_reason_t why = ps_classify(c, w, st);
    asmspy_ps_act_t act = asmspy_ps_decide(kind, why, c->tearing_down,
                                           w == hold, arrived != NULL);
    return ps_perform(c, w, WSTOPSIG(st), act, hold, arrived);
}

/* Pump events until `hold` stops, a thread arrives at the armed entry, `hold`
 * (or the whole target) is gone, or `budget_us` elapses. `spin_us` is how long
 * the wait busy-polls before it starts napping (see PS_SPIN_US).
 *
 * THIS IS ALSO THE ONLY THING KEEPING THE TARGET ALIVE, and that is not
 * rhetoric — it is the second half of correction 1 and it cost a measurement to
 * find. PTRACE_SEIZE makes us the owner of the tracee's SIGNAL STREAM: from the
 * seize onward, every signal it takes becomes a signal-delivery-stop that only
 * the tracer can end. A tracer that pumps only around its own PTRACE_INTERRUPTs
 * leaves those stops unconsumed, and the target simply STOPS — MEASURED against
 * sigload_victim, whose 100 Hz counter froze at ticks=99 for the entire window
 * while /proc reported it in state 't' and the sampler, seeing no RUNNING
 * thread to interrupt, quietly took ONE sample in 1.5 s (intr=1, notrun=1424).
 * A "1% overhead" sampler had in fact suspended the process outright.
 *
 * So every idle stretch anywhere in this file is spent HERE rather than in a
 * nanosleep — the pump is the target's heartbeat, not a poll for our
 * convenience. */
static int ps_pump(ps_ctx_t *c, pid_t hold, long budget_us, long spin_us,
                   pid_t *arrived) {
    long long t0 = ps_now_us();
    for (;;) {
        /* THE DEADLINE, checked on EVERY iteration including the drain path.
         * It used to sit below the `continue` that drains queued events, so a
         * target generating stops faster than we consumed them kept this loop
         * fed forever and every budget in the file became advisory. MEASURED:
         * one sampler run stopped making progress and had to be killed. */
        long long el = ps_now_us() - t0;
        if (el >= budget_us)
            return PS_EV_NONE;
        int st = 0;
        pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
        if (w > 0) {
            int ev = ps_dispatch(c, w, st, hold, arrived);
            if (ev == PS_EV_ARRIVED)
                return ev;
            if (ev == PS_EV_STOPPED && w == hold)
                return ev;
            if (ev == PS_EV_GONE && (c->n == 0 || (hold && w == hold)))
                return PS_EV_GONE;
            continue; /* drain what is already queued before napping */
        }
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return PS_EV_GONE; /* ECHILD — nothing left to wait for */
        }
        if (el >= spin_us) {
            struct timespec nap = {0, PS_POLL_US * 1000};
            nanosleep(&nap, NULL);
        }
    }
}

/* Bring `tid` to a ptrace-stop. PTRACE_POKETEXT and PTRACE_PEEKTEXT are both
 * refused on a RUNNING tracee — silently, leaving a trap armed — so every
 * plant/restore goes through here first. 0 if stopped, -1 if it vanished.
 *
 * `budget_us` CLAMPS the wait, and it is what makes phase 3's advertised bound
 * true (2026-08-06 final review, finding 7). PS_STOP_US is 200 ms PER THREAD and
 * a thread in uninterruptible sleep pays all of it and stays in the table, so
 * every later sweep pays again: against a fat or D-state target the confirm
 * phase cost O(threads x 200 ms) per candidate while its own comment advertised
 * 800 ms for the whole phase — wrong by ~60x, with the target held stopped for
 * it. Callers that own a deadline pass what is left of it; the teardown passes
 * PS_STOP_FOREVER, because getting our byte back out is not something to give up
 * on early. */
#define PS_STOP_FOREVER ((long)0x7fffffff)
static int ps_ensure_stopped(ps_ctx_t *c, pid_t tid, long budget_us) {
    int i = ps_find(c, tid);
    if (i < 0)
        return -1;
    if (c->t[i].stopped)
        return 0;
    if (budget_us <= 0)
        return -1; /* nothing left to spend: not stopped, and say so */
    if (budget_us > PS_STOP_US)
        budget_us = PS_STOP_US;
    /* TEST LEVER (inert unless ASMSPY_PS_TEST_STOP_COST_US is set), charged
     * HERE so it lands inside the sweep's own accounting, exactly where a
     * D-state thread's cost lands. See ps_read_stop_cost_us. */
    long cost = ps_read_stop_cost_us();
    if (cost > 0) {
        struct timespec ts = {cost / 1000000L, (cost % 1000000L) * 1000L};
        nanosleep(&ts, NULL);
    }
    ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
    long long t0 = ps_now_us();
    while (ps_now_us() - t0 < budget_us) {
        /* `arrived` NULL: this is not an arrival collector. A thread that trips
         * the entry while we are getting `tid` stopped is rewound and RELEASED
         * inside ps_perform rather than parked here. */
        ps_pump(c, tid, PS_INTR_US, PS_SPIN_US, NULL);
        i = ps_find(c, tid);
        if (i < 0)
            return -1;
        if (c->t[i].stopped)
            return 0;
        /* The interrupt can be absorbed by a concurrent signal-delivery-stop
         * (which the pipeline consumed and re-injected); re-issue it. */
        ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
    }
    return -1;
}

/* Stop every thread we hold, over a SNAPSHOT of tids rather than live indices.
 *
 * The live-index walk this replaces was unsafe against ps_del's swap-with-last:
 * a thread swapped from the tail into an already-visited slot was never stopped,
 * and was then detached while RUNNING — which fails ESRCH, leaves it seized, and
 * with the pump gone its next signal becomes a stop nobody will ever consume.
 * That is a permanent version of the freeze correction 1's second half
 * measured. The snapshot is taken without issuing any ptrace call, so nothing
 * can mutate the table underneath it.
 *
 * Two rounds, because a thread cloned DURING the first round is tabled by the
 * pump and is not in that round's snapshot.
 *
 * `budget_us` is the bound for the WHOLE sweep, not per thread, and it is
 * spent down as it goes (finding 7). A sweep that runs out stops asking: the
 * threads it did get are still stopped, and a plant needs only ONE pokeable
 * thread, so an exhausted budget costs candidate coverage, never safety —
 * every task here is SEIZED either way, which is the property that keeps an
 * armed int3 survivable. */
static void ps_stop_all(ps_ctx_t *c, long budget_us) {
    long long t0 = ps_now_us();
    const int bounded = budget_us != PS_STOP_FOREVER;
    for (int round = 0; round < 2; round++) {
        pid_t snap[PS_TABLE_SLOTS];
        int n = 0;
        for (int i = 0; i < c->n; i++)
            if (!c->t[i].stopped && (!c->t[i].listening || c->tearing_down))
                snap[n++] = c->t[i].tid;
        if (n == 0)
            break;
        for (int i = 0; i < n; i++) {
            long left = bounded ? budget_us - (long)(ps_now_us() - t0)
                                : PS_STOP_FOREVER;
            if (left <= 0) {
                if (bounded) {
                    c->sweep_clamped++;
                    c->sweep_us += (unsigned long long)(ps_now_us() - t0);
                }
                return;
            }
            ps_ensure_stopped(c, snap[i], left);
        }
    }
    if (bounded) {
        long long spent = ps_now_us() - t0;
        c->sweep_us += (unsigned long long)spent;
        if (spent > budget_us)
            c->sweep_over++; /* the bound was advertised and then exceeded */
    }
}

/* Consume every wait status that is ALREADY QUEUED, and not one more.
 *
 * ps_pump with a budget would do this too, but it would also NAP for the rest of
 * that budget when the queue is empty — and the two callers here run on a
 * deadline they have already advertised. This costs one waitpid when there is
 * nothing to reap. The guard is the pump's own lesson: a target generating stops
 * faster than we consume them must not be able to keep a "drain" fed forever. */
#define PS_DRAIN_MAX 512
static void ps_drain(ps_ctx_t *c) {
    c->in_drain = 1;
    for (int guard = 0; guard < PS_DRAIN_MAX; guard++) {
        int st = 0;
        pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
        if (w <= 0)
            break;
        /* hold 0, arrived NULL: nobody is waiting on a particular tid here, and
         * an arrival at this point is not a measurement — it is a task to make
         * safe, which is exactly what ASMSPY_PS_ACT_REWIND_RESUME does. */
        ps_dispatch(c, w, st, 0, NULL);
    }
    c->in_drain = 0;
}

/* Undo ps_stop_all: end the stops WE caused, and only those.
 *
 * THROUGH THE DECISION, not around it. Every task this walks was stopped by our
 * own PTRACE_INTERRUPT, which ps_classify calls ASMSPY_PS_R_INTERRUPT, so asking
 * the table what that stop is owed is not a re-classification — it is the same
 * question the pump already answered for the same stop, and it keeps this the
 * completion of an ASMSPY_PS_ACT_HOLD rather than a second opinion.
 *
 * It also fixes a real hole. The old form resumed any `stopped && !listening`
 * entry regardless of kind, which contradicted the one case that deliberately
 * leaves a task stopped: ASMSPY_PS_ACT_RELEASE, when the restore could not be
 * completed, marks a FOREIGN task "still ours, the teardown retries" — and this
 * walk then CONTed it anyway, with sig 0, ending a signal-delivery-stop and
 * destroying another process's signal. The table says RELEASE for that task, and
 * RELEASE is not something to do from here (it detaches, and it can pump), so
 * the honest reading of a non-RESUME_QUIET answer is: not mine to resume.
 *
 * An UNKNOWN task IS resumed here, because the table says RESUME_QUIET for it —
 * leaving one stopped for the rest of phase 3 would freeze a task that may well
 * be a thread of the target, which is the freeze correction 1's second half
 * measured, in slow motion. */
static void ps_resume_all(ps_ctx_t *c) {
    for (int i = 0; i < c->n; i++) {
        /* NOT the listening ones: PTRACE_CONT on a group-stopped tracee either
         * fails or, worse, resumes a process the user deliberately suspended. */
        if (!c->t[i].stopped || c->t[i].listening)
            continue;
        if (asmspy_ps_decide(c->t[i].kind, ASMSPY_PS_R_INTERRUPT,
                             c->tearing_down, 0,
                             0) != ASMSPY_PS_ACT_RESUME_QUIET)
            continue;
        ps_cont(c, c->t[i].tid, 0); /* sig 0: completing a HOLD, never a
                                     * delivery — see the file-header rule */
    }
}

/* Stop everything, take the trap byte out, rewind anyone parked at base+1, and
 * only THEN forget the address.
 *
 * Two attempts, because the usual reason the first fails is that nothing was
 * stopped yet. If the byte is still in after that, `leaked` is set and bp_base
 * is KEPT: clearing it is how the address of a live int3 was lost, after which
 * teardown poked address zero and the target died seconds later with the
 * sampler reporting success. Returns 0 clean, -1 leaked. */
static int ps_disarm(ps_ctx_t *c, long budget_us) {
    ps_stop_all(c, budget_us);

    /* TEST LEVER (inert unless ASMSPY_PS_TEST_LATE_TRAP_MS is set): put the
     * candidate back into the state it held for its whole collection window —
     * byte IN, a thread RUNNING — and idle, so a task of the target executes our
     * int3 in the one window the two steps below exist for. See
     * ps_read_late_trap_ms.
     *
     * Gated on bp_arrived for the reason ASMSPY_PS_TEST_TGID_FAIL=armed is:
     * spending the window on an entry nothing enters makes the whole thing
     * vacuous. The re-plant is needed because a saturating entry has its byte
     * taken out by the arrival loop's last iteration — which exit phase 3 took
     * says nothing about the window this reconstructs, and a target with two
     * runnable threads is in exactly this state for 50 ms per candidate. The
     * thread resumed is never the one the unplant will poke through. */
    long late = ps_read_late_trap_ms();
    if (late > 0 && c->bp_arrived) {
        pid_t planter = 0;
        for (int i = 0; i < c->n && !planter; i++)
            if (ps_can_poke(&c->t[i]))
                planter = c->t[i].tid;
        if (planter && !c->bp_planted)
            ps_plant(c, planter, c->bp_base);
        if (planter && c->bp_planted) {
            for (int i = 0; i < c->n; i++)
                if (c->t[i].tid != planter && c->t[i].stopped &&
                    !c->t[i].listening && c->t[i].kind == ASMSPY_PS_OWN) {
                    ps_cont(c, c->t[i].tid, 0);
                    break;
                }
            struct timespec ts = {late / 1000, (late % 1000) * 1000000L};
            nanosleep(&ts, NULL);
        }
    }

    /* TEST LEVER (inert unless ASMSPY_PS_TEST_STEAL_BYTE is set): make the word
     * at the armed address neither our trap nor the original, ask ps_unplant to
     * restore, and see whether it wrote. Every thread is stopped here, and the
     * true original goes back before any of them runs, so the target's text is
     * identical either side. See ps_read_steal_byte. */
    pid_t stealer = 0;
    long stolen = 0;
    if (ps_read_steal_byte() && c->bp_planted && c->bp_base) {
        for (int i = 0; i < c->n && !stealer; i++)
            if (ps_can_poke(&c->t[i]))
                stealer = c->t[i].tid;
        if (stealer) {
            /* Distinct from BOTH the original and the trap encoding, so the
             * PEEK afterwards can tell "declined to write" from "wrote". */
#if defined(__aarch64__)
            stolen = (c->bp_orig & ~0xffffffffL) | (long)0xd503201fL;
#else
            stolen = (c->bp_orig & ~0xffL) | 0x90L;
            if ((c->bp_orig & 0xffL) == 0x90L)
                stolen = (c->bp_orig & ~0xffL) | 0xf4L;
#endif
            if (ptrace(PTRACE_POKETEXT, stealer, (void *)(uintptr_t)c->bp_base,
                       (void *)(uintptr_t)stolen) != 0)
                stealer = 0;
        }
    }

    if (ps_unplant(c) != 0) {
        ps_stop_all(c, budget_us);
        if (ps_unplant(c) != 0) {
            c->leaked = 1;
            return -1;
        }
    }

    if (stealer) {
        errno = 0;
        long now = ptrace(PTRACE_PEEKTEXT, stealer,
                          (void *)(uintptr_t)c->bp_base, NULL);
        if (!(now == -1 && errno)) {
            if (now == stolen)
                c->guard_held++; /* looked, saw it was not ours, left it */
            else
                c->blind_writes++; /* wrote over a word it never read */
        }
        /* Put the target's real text back, unconditionally: this probe must
         * not be the thing that damages the process. */
        ptrace(PTRACE_POKETEXT, stealer, (void *)(uintptr_t)c->bp_base,
               (void *)(uintptr_t)c->bp_orig);
        c->bp_planted = 0;
    }

    /* REAP WHAT IS ALREADY QUEUED, HERE, WHILE bp_base STILL NAMES THE TRAP.
     *
     * The byte is out, so no task can arrive from now on — but one that arrived
     * a moment ago may be sitting in a ptrace-stop whose status we have not
     * consumed. Two things then go wrong at once, and MEASURED, both of them do
     * (2026-08-06 final review, finding 1): the rewind sweep below silently
     * moves its pc off base+1 — PTRACE_GETREGS/SETREGS on an unreaped
     * ptrace-stop SUCCEED, measured on this host, so "we have not waited on it"
     * is no protection at all — and then bp_base is cleared, so when the status
     * finally is reaped there is neither an address nor a pc left to recognise
     * it by, and asmspy_ps_decide is handed R_APP_TRAP and re-injects SIGTRAP
     * into a process with no handler. Draining first means those stops go
     * through the pipeline as the ARRIVALS they are, and are rewound and
     * resumed by the one cell that owns that decision. */
    if (!ps_read_skip_drain())
        ps_drain(c);

    for (int i = 0; i < c->n; i++)
        /* Only a stop we HOLD. Rewinding a task whose status is still queued is
         * how the evidence above got erased; and on a running tracee ps_rewind's
         * register read fails anyway, so this loses nothing real. */
        if (c->t[i].stopped)
            ps_rewind(c->t[i].tid, c->bp_base);
    c->bp_base = 0; /* ONLY now, and only because the byte is provably out */
    return 0;
}

/* Detach ONE thread, insisting. PTRACE_DETACH requires a ptrace-stop; on a
 * running tracee it fails ESRCH — and the old code discarded that return, so the
 * thread stayed SEIZED after we returned, with the pump gone. Its next signal
 * then becomes a signal-delivery-stop nobody will ever consume and it hangs
 * PERMANENTLY. Returns 0 (detached, or it exited), -1 (still ours, and stuck). */
static int ps_detach_one(ps_ctx_t *c, pid_t tid, int sig) {
    for (int attempt = 0; attempt < PS_DETACH_TRIES; attempt++) {
        /* `sig` on the FIRST attempt only, and it is nonzero only for
         * ASMSPY_PS_ACT_RELEASE_SIGNAL — i.e. only when the stop we are leaving
         * is a signal-delivery-stop, the one ptrace-stop where restarting with a
         * signal is defined. A retry has already interrupted the tracee out of
         * that stop, so injecting there would be fabricating a delivery. */
        if (ptrace(PTRACE_DETACH, tid, NULL,
                   (void *)(uintptr_t)(attempt == 0 ? sig : 0)) == 0)
            return 0;
        /* PS_STOP_FOREVER: the alternative to stopping it is leaving it SEIZED
         * with no pump, which is a permanent hang — see this function's own
         * doc. Nothing here is on the confirm phase's clock. */
        if (ps_ensure_stopped(c, tid, PS_STOP_FOREVER) != 0)
            /* ps_ensure_stopped removes a vanished tid; anything else means we
             * could not bring it to a stop and the next attempt will not fare
             * better. */
            return ps_find(c, tid) < 0 ? 0 : -1;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* attach / detach                                                     */
/* ------------------------------------------------------------------ */

/* SEIZE every thread of `pid`.
 *
 * CORRECTION 2, the half that keeps the user's process alive. The entry trap
 * phase 3 plants is a byte in SHARED process text, and
 * cli/asmspy_engine.c:2954-2957 spells out the consequence for a thread that
 * was never seized and reaches it: it "would take a SIGTRAP with no tracer and
 * DIE" — and SIGTRAP's default action takes the whole process with it, seconds
 * after we detached, so the damage does not even look like ours. The prototype
 * passed options 0. PTRACE_O_TRACECLONE is what makes a thread created after
 * this scan our tracee too.
 *
 * PTRACE_O_TRACEFORK/VFORK are set for the SAME reason one address space over:
 * a child forked inside an armed window takes a copy-on-write copy of the trap
 * byte and dies of it. They were previously unset while the classifier carried
 * arms for both events, so the code READ as though forks were handled while the
 * arms were unreachable — a fork-per-request server is a realistic target and
 * the armed windows total ~800 ms.
 *
 * THE SCAN REPEATS until a whole pass adds nothing. A single pass is not enough:
 * a thread cloned BY a thread we had not yet seized is neither in our scan nor
 * covered by TRACECLONE, and it is exactly such a thread that later executes the
 * shared int3 with no tracer.
 *
 * Sets c->covered = 0 for anything that leaves a task of the target untraced —
 * the table cap, a refusal that is not ESRCH, or a scan that never converged.
 * Returns the number of tasks seized, or -1 if none could be. */
static int ps_seize_all(ps_ctx_t *c) {
    char dir[64];
    snprintf(dir, sizeof dir, "/proc/%d/task", (int)c->pid);
    long opts = PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK;
    c->covered = 1;

    for (int pass = 0; pass < PS_SEIZE_PASSES; pass++) {
        DIR *d = opendir(dir);
        if (!d) {
            /* The target vanished mid-scan. Whatever we hold is not the whole
             * thread set by definition. */
            c->covered = 0;
            break;
        }
        int added = 0;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] < '0' || e->d_name[0] > '9')
                continue;
            pid_t tid = (pid_t)atoi(e->d_name);
            if (ps_find(c, tid) >= 0)
                continue;
            if (c->n >= c->cap) {
                /* Test capacity BEFORE seizing, never after. PTRACE_SEIZE does
                 * not stop the tracee, so the PTRACE_DETACH that "undid" an
                 * untabled seize was refused ESRCH on a running task — leaving
                 * it seized, untabled and unpumped, i.e. a task whose next
                 * signal-delivery-stop nobody would ever consume. */
                c->covered = 0;
                continue;
            }
            errno = 0;
            if (ptrace(PTRACE_SEIZE, tid, NULL, (void *)opts) != 0) {
                /* ESRCH is a task that exited between readdir and here: it can
                 * never execute the trap, so coverage is intact. Anything else
                 * (EPERM, a locked-down container) is a task that is ALIVE and
                 * NOT OURS, which is exactly the fatal case. */
                if (errno != ESRCH)
                    c->covered = 0;
                continue;
            }
            if (ps_add(c, tid) < 0) { /* physical slots exhausted */
                c->covered = 0;
                continue;
            }
            added++;
        }
        closedir(d);
        if (added == 0)
            return c->n == 0 ? -1 : c->n; /* converged */
    }
    /* Fell out of the pass budget with tasks still appearing: the target is
     * spawning faster than we can enumerate. Truthfully not covered. */
    c->covered = 0;
    return c->n == 0 ? -1 : c->n;
}

/* Detach every task, leaving the target alive and running.
 *
 * The two hazards this owns are the ones rgn_detach_all documents, and they are
 * the same failure in different clothes: leaving a live process holding a trap
 * we planted. The entry byte must be restored, and any thread queued at base+1
 * must be rewound before it is released — detaching it there resumes it in the
 * middle of an instruction. Both primitives are refused on a running thread, so
 * everything is stopped first.
 *
 * A THIRD hazard, which detaching a thread we merely HOPED was stopped used to
 * create: PTRACE_DETACH on a running tracee fails ESRCH and leaves it seized
 * with our pump gone, so its next signal is a stop nobody will ever consume and
 * it hangs forever. ps_detach_one insists; what it cannot free is reported.
 *
 * A FOURTH: a task TABLED DURING THIS WALK (2026-08-06 final review, finding 4).
 * ps_detach_one pumps on a failed DETACH and the pump tables whatever reports,
 * so a clone that lands mid-walk is not in the snapshot the walk is working
 * through. The old form then did an unconditional `c->n = 0` with `bad`
 * untouched: the task was never detached, the evidence that it existed was
 * erased, and the call reported a clean target. Its own attach-stop decides
 * ASMSPY_PS_ACT_HOLD under `tearing_down`, so it is stopped FOREVER with the
 * pump gone — and the tracer here is libasmspy.a inside a long-lived `--serve`
 * process, so there is no exit-time auto-detach to rescue it. Hence the
 * snapshot-and-detach REPEATS until a pass adds nothing (the shape ps_seize_all
 * already uses), and what is left in the table at the end is reported, not
 * zeroed.
 *
 * Returns 0 if the target was handed back clean, -1 if anything was left behind
 * (a trap byte still in, or a task still seized) — which the caller must not
 * report as success. */
#define PS_DETACH_PASSES 8
static int ps_detach_all(ps_ctx_t *c) {
    int bad = 0;
    /* From here on a job-control stop must NOT be re-LISTENed: PTRACE_DETACH is
     * refused ESRCH on a LISTENed tracee (MEASURED), and ps_detach_one's retry
     * would re-LISTEN it on every round and never converge. Interrupting it
     * out of LISTEN and detaching from the resulting group-stop leaves the
     * process in state 'T' — still suspended, exactly as the user left it. */
    c->tearing_down = 1;

    /* THE TARGET IS GONE is not THE TARGET WAS DAMAGED. A process that exits on
     * its own during the window leaves an empty table with bp_planted still
     * set; ps_unplant then has nobody to poke through, reports a leak, and the
     * whole call comes back "could not be handed back clean" — about a process
     * that simply ended, and whose text died with it. Two different facts, and
     * an operator sent to look for a corrupted process finds nothing.
     *
     * `covered &&`, because an empty table only means "the target exited" if we
     * held the WHOLE thread set. Without coverage it can equally mean we dropped
     * the tasks we knew about while others we never seized are still running —
     * with our byte possibly still in their text. That is the one case that must
     * NOT come back clean, and it is the reading the plain `n == 0` gave it. */
    if (c->covered && c->n == 0) {
        c->bp_base = 0;
        c->bp_planted = 0;
        return 0;
    }

    /* PS_STOP_FOREVER: the confirm phase's clamp (finding 7) is about not
     * charging a caller for candidates; the teardown is about not leaving a
     * trap byte in a live process, and there is no budget at which giving up on
     * that is the better answer. */
    if (c->bp_base && ps_disarm(c, PS_STOP_FOREVER) != 0)
        bad = 1;
    else
        ps_stop_all(c, PS_STOP_FOREVER);

    /* Restore-and-rewind, then detach, REPEATED until a pass frees nobody new.
     * ps_detach_one pumps, the pump tables clones, and a task tabled after the
     * snapshot this walks is a task nothing here would ever have detached. */
    for (int pass = 0; pass < PS_DETACH_PASSES; pass++) {
        for (int i = 0; i < c->n; i++) {
            /* A task in another address space still carrying a private copy of
             * a trap byte. ps_restore_copy writes only if our byte is actually
             * there, so a child that execve'd in the meantime is left alone
             * rather than having eight bytes of the parent's old text spliced
             * into its new image. */
            if (c->t[i].copied_bp) {
                if (ps_restore_copy(c->t[i].tid, c->t[i].copied_bp,
                                    c->t[i].copied_orig) != 0)
                    bad = 1;
                else
                    ps_rewind(c->t[i].tid, c->t[i].copied_bp);
            }
            /* ps_trap_addr, not bp_base: ps_disarm above cleared bp_base on
             * success, and a thread parked at base+1 at teardown time is
             * exactly who this rewind is for. */
            ps_rewind(c->t[i].tid, ps_trap_addr(c));
        }
        /* Snapshot: ps_detach_one can drop a vanished tid, and can table a new
         * one, mid-walk. */
        pid_t snap[PS_TABLE_SLOTS];
        int n = 0;
        for (int i = 0; i < c->n; i++)
            snap[n++] = c->t[i].tid;
        if (n == 0)
            break;
        /* TEST LEVER (inert unless ASMSPY_PS_TEST_WALK_PUMP_MS is set): let the
         * leader RUN and pump here, where a retrying ps_detach_one already
         * pumps, so clones land after this pass's snapshot on demand. Both
         * halves are states the walk itself produces — ASMSPY_PS_ACT_FOLLOW_
         * CHILD CONTs the parent even under `tearing_down`, and every
         * ps_ensure_stopped inside ps_detach_one pumps — but only ever for as
         * long as a clone happens to take, which is why the window could not be
         * reached from a test. See ps_read_walk_pump_ms. */
        long wp = pass == 0 ? ps_read_walk_pump_ms() : 0;
        if (wp > 0) {
            for (int i = 0; i < c->n; i++)
                if (c->t[i].tid == c->pid && c->t[i].stopped &&
                    !c->t[i].listening) {
                    ps_cont(c, c->t[i].tid, 0);
                    break;
                }
            ps_pump(c, 0, wp * 1000, PS_SPIN_US, NULL);
        }
        int freed = 0;
        for (int i = 0; i < n; i++) {
            if (ps_detach_one(c, snap[i], 0) != 0) {
                bad = 1;
                continue;
            }
            /* Out of the table only once it is provably no longer ours —
             * ps_detach_one does not ps_del, and the old `c->n = 0` below was
             * doing it for every task at once, detached or not. */
            if (ps_find(c, snap[i]) >= 0) {
                ps_del(c, snap[i]);
                freed++;
                if (pass > 0)
                    c->late_detached++; /* tabled after an earlier pass had
                                         * already taken its snapshot */
            }
        }
        if (c->n == 0)
            break;
        if (freed == 0)
            break; /* a pass that frees nobody will not fare better twice */
        /* Anything still here was tabled during the walk, or refused: stop it
         * before the next pass tries to detach it. */
        ps_stop_all(c, PS_STOP_FOREVER);
    }

    c->bp_base = 0;
    /* NOT `c->n = 0`. A task still in the table here is a task still SEIZED,
     * with the pump about to go away — the exact permanent hang ps_detach_one's
     * comment describes — and zeroing the count reported it as a clean handback.
     * Damage is what this return value is for. */
    if (c->n)
        bad = 1;
    return bad ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* PHASE 1 — residency                                                 */
/* ------------------------------------------------------------------ */

/* Fold one sampled PC into the per-function histogram. Folding HERE rather than
 * keeping raw PCs is what keeps the table bounded by the number of functions
 * seen instead of by the number of samples, which in turn is what makes the
 * pure rank's fold lossless (its doc comment requires out_cap >= bucket count,
 * and a raw-PC table would blow past any fixed cap on a hot loop). */
static void ps_hit(ps_sym_hit_t *h, int *nh, uint64_t start) {
    for (int i = 0; i < *nh; i++)
        if (h[i].start == start) {
            h[i].count++;
            return;
        }
    if (*nh >= PS_MAX_SYMS)
        return;
    h[*nh].start = start;
    h[*nh].count = 1;
    (*nh)++;
}

/* Sample the target's PCs for `window_ms` at ~ASMSPY_PS_HZ, round-robin over
 * the threads /proc says are actually EXECUTING (ps_thread_executing).
 *
 * The interrupt-read-resume cycle is the whole cost model: MEASURED at 0.27% of
 * the window for 100 threads, which is the same as for 1 — one sample is taken
 * per tick regardless of how many threads exist, so the thread count buys
 * coverage, not cost.
 *
 * An interrupt that does not land within PS_INTR_US is dropped rather than
 * retried: it was absorbed by a signal-delivery-stop that the pipeline consumed
 * and re-injected, and a statistical sampler owes no particular thread a
 * reading. Returns the number of distinct functions observed. */
static int ps_phase1(ps_ctx_t *c, const asmspy_symtab_t *syms, long window_ms,
                     ps_sym_hit_t *hits, unsigned long long *taken) {
    int nh = 0;
    int rr = 0;
    long long t_end = ps_now_us() + window_ms * 1000;
    long period_us = 1000000L / ASMSPY_PS_HZ;

    while (ps_now_us() < t_end && c->n > 0) {
        long long tick = ps_now_us();

        /* Pick the next EXECUTING thread, round-robin so a busy leader cannot
         * starve the workers (and vice versa). */
        pid_t tid = 0;
        for (int k = 0; k < c->n; k++) {
            int i = (rr + k) % c->n;
            /* Never a fork/vfork CHILD: it is a different process running
             * different code, and its /proc path is not under our pid, so
             * folding its pc into this target's histogram would be a lie. Never
             * a LISTENed one either — it is suspended by the user's job control,
             * not by us, and interrupting it would undo that. */
            if (!ps_kind_may_sample(c->t[i].kind) || c->t[i].listening)
                continue;
            if (ps_thread_executing(c->pid, c->t[i].tid)) {
                tid = c->t[i].tid;
                rr = i + 1;
                break;
            }
        }
        if (tid) {
            ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
            /* NULL: no entry is armed during phase 1, so there is no arrival to
             * collect — and a pump that could park a thread here would be a
             * thread this loop never resumes. */
            if (ps_pump(c, tid, PS_INTR_US, PS_SPIN_US, NULL) ==
                PS_EV_STOPPED) {
                asmspy_regs_t regs;
                if (asmspy_regs_read(tid, &regs) == 0) {
                    uint64_t pc = asmspy_reg_pc(&regs);
                    const asmspy_sym_t *s = asmspy_symtab_at(syms, pc);
                    if (s && s->size > 0)
                        ps_hit(hits, &nh, s->addr);
                    (*taken)++;
                }
                ps_cont(c, tid, 0); /* ours: the pipeline classified it */
            }
        }

        /* Spend the rest of the tick PUMPING, never sleeping. See ps_pump: from
         * the seize onward every signal the target takes is a stop only we can
         * end, so a nanosleep here is a stretch of time in which the target may
         * be suspended and unable to become RUNNING again — which then also
         * stops us from ever picking it above, a starvation that measured as
         * intr=1 over a 1.5 s window. */
        long long spent = ps_now_us() - tick;
        if (spent < period_us)
            ps_pump(c, 0, (long)(period_us - spent), 0, NULL);
    }
    return nh;
}

/* ------------------------------------------------------------------ */
/* PHASE 2 — direct-call expansion                                     */
/* ------------------------------------------------------------------ */

/* Admit `s` as a candidate, or find it if it is already one. Returns its index,
 * or -1 when it does not qualify (or the table is full). */
static int ps_push_cand(ps_cand_t *v, int *n, const asmspy_sym_t *s,
                        const char *module, unsigned long long residency) {
    if (!s)
        return -1;
    /* The vacuity rule (asmspy_autoregion.h): asmspy_symtab_at resolves a
     * zero-size symbol at its exact start ONLY, so such a symbol looks like a
     * pure stream of entry hits by construction — and the producer needs a real
     * extent anyway, since it takes (base, len). */
    if (s->size == 0)
        return -1;
    if (!asmspy_ar_match(s->module, module))
        return -1;
    for (int i = 0; i < *n; i++)
        if (v[i].addr == s->addr)
            return i;
    if (*n >= ASMSPY_PS_MAX_CAND)
        return -1;
    v[*n].addr = s->addr;
    v[*n].size = s->size;
    v[*n].name = s->name;
    v[*n].module = s->module;
    v[*n].residency = residency;
    v[*n].arrivals = 0;
    v[*n].first_us = 0;
    v[*n].sites = 0;
    return (*n)++;
}

/* Widen the shortlist with the DIRECT-call targets of each shortlisted body.
 *
 * This is the phase that exists because residency is measurably the wrong
 * answer, not merely a noisy one. A time-based PC histogram is dominated by the
 * functions entered once that never return — main, an event loop,
 * auto_victim's grind_forever, MEASURED 394:5 against the correct pick — and
 * arming an entry breakpoint on one of those is a capture that never fires.
 * The right answer is usually a CALLEE of the residency winner, and a callee
 * that returns quickly is exactly what residency cannot see. Reading the caller
 * for its `call` targets is how we reach it without an entry-evidence sampler.
 *
 * CORRECTION 5: asmtest_disas_call_target is #ifdef ASMTEST_HAVE_CAPSTONE and
 * returns 0 silently without it — which, from here, is indistinguishable from
 * "this function makes no direct calls". So the disposition is checked up front
 * and REPORTED (asmspy_ps_expand_note), because a silent fall-through to a
 * residency-only ranking is precisely the answer this module exists to reject. */
static void ps_phase2(ps_ctx_t *c, const asmspy_symtab_t *syms,
                      const char *module, ps_cand_t *cands, int *ncand,
                      int shortlist, char *why, size_t whylen) {
    const char *note = asmspy_ps_expand_note(asmtest_disas_available() ? 1 : 0);
    if (note) {
        ps_note(why, whylen, note);
        return;
    }

    uint8_t code[PS_SCAN_BYTES];
    for (int i = 0; i < shortlist && i < *ncand; i++) {
        uint64_t base = cands[i].addr;
        size_t len = cands[i].size;
        if (len > sizeof code)
            len = sizeof code;
        struct iovec liov = {code, len};
        struct iovec riov = {(void *)(uintptr_t)base, len};
        ssize_t got = process_vm_readv(c->pid, &liov, 1, &riov, 1, 0);
        if (got <= 0)
            continue;

        size_t off = 0;
        while (off < (size_t)got) {
            int is_call = 0;
            size_t ilen = asmtest_disas_probe(ASMSPY_HOST_ARCH, code,
                                              (size_t)got, off, &is_call, NULL);
            if (ilen == 0)
                break; /* undecodable: the rest of this body is not walkable */
            if (is_call) {
                uint64_t tgt = 0;
                /* An INDIRECT call yields 0 here and is simply not expanded:
                 * resolving it needs the live register state at the call, which
                 * is a single-step engine's job, not a sampler's. */
                if (asmtest_disas_call_target(ASMSPY_HOST_ARCH, code,
                                              (size_t)got, base, off, &tgt)) {
                    int k = ps_push_cand(
                        cands, ncand, asmspy_symtab_at(syms, tgt), module, 0);
                    /* Count the SITE even when the candidate already existed
                     * from residency: "this entry is named by a call in a body
                     * we watched run" is evidence residency cannot produce, and
                     * it is what makes this phase's contribution visible to the
                     * caller (and testable — see ps_cand_t::sites). */
                    if (k >= 0)
                        cands[k].sites++;
                }
            }
            off += ilen;
        }
    }
}

/* ------------------------------------------------------------------ */
/* PHASE 3 — arrival confirmation                                      */
/* ------------------------------------------------------------------ */

/* Arm `base`, run the target, and measure how long until a thread ARRIVES
 * there — plus how many arrive, up to ASMSPY_PS_HIT_BUDGET.
 *
 * CORRECTION 4 is why the clock is the point and the counter is not. Under a
 * per-candidate hit budget the counts SATURATE and tie: measured, a tiny hot
 * callee (50 arrivals, residency 1) tied exactly with libc's sched_yield (50
 * arrivals, residency 1), leaving the tie to be broken by residency — the very
 * metric the entry rule exists to replace. Time-to-first-arrival separated the
 * same pair 105 us vs 6125 us. So the count is kept as EVIDENCE that the entry
 * is live, and the clock is the rank.
 *
 * This phase is also the only one that answers the question the caller actually
 * has. The producer arms an int3 at the region entry and waits; "will that fire
 * promptly" is not predicted here, it is MEASURED, with the same primitive.
 *
 * `budget_us` bounds the WHOLE candidate — the stop sweep, the arm, the arrival
 * wait and the disarm — not just the wait. It used to start its clock AFTER
 * ps_stop_all, which is where the phase's advertised bound went wrong (finding
 * 7): a sweep is O(threads) x PS_STOP_US in the bad case, so the part outside
 * the clock could dwarf the part inside it. The disarm is the one step still
 * allowed to overrun: leaving our byte in a live process is not a cost to trade
 * against a deadline. */
static void ps_phase3_one(ps_ctx_t *c, ps_cand_t *cd, long budget_us) {
    long long t_end = ps_now_us() + budget_us;
    c->bp_arrived = 0; /* a NEW candidate: nothing has reached this one yet. Not
                        * cleared by ps_plant, because phase 3 re-plants the SAME
                        * address after every arrival and the fact being tracked
                        * is about the ENTRY, not about one planting of it. */
    ps_stop_all(c, budget_us);
    if (c->n == 0)
        return;
    /* ps_can_poke, not `stopped`: a LISTENed thread is in group-stop and every
     * PEEK/POKE on it fails ESRCH, and a fork child would be planted into the
     * WRONG address space. Picking either loses the candidate silently. */
    pid_t planter = 0;
    for (int i = 0; i < c->n && !planter; i++)
        if (ps_can_poke(&c->t[i]))
            planter = c->t[i].tid;
    if (!planter || ps_plant(c, planter, cd->addr) != 0) {
        ps_resume_all(c);
        return; /* W^X text, or the thread died: not this candidate's fault,
                 * and not something to report as "never entered" */
    }

    long long t0 = ps_now_us();
    ps_resume_all(c);

    while (cd->arrivals < ASMSPY_PS_HIT_BUDGET) {
        /* Against t_end (set at entry), not against the arm: what the caller
         * was promised is a per-candidate cost, and the sweep above already
         * spent part of it. */
        long left = (long)(t_end - ps_now_us());
        if (left <= 0)
            break;
        pid_t who = 0;
        int ev = ps_pump(c, 0, left, PS_SPIN_US, &who);
        if (ev == PS_EV_GONE)
            break;
        if (ev != PS_EV_ARRIVED || !who)
            break; /* budget expired with nobody arriving */

        if (cd->arrivals == 0) {
            long long dt = ps_now_us() - t0;
            /* Clamp to 1: a 0 would read as "not measured" in the output
             * struct, and an arrival that fast is the strongest signal there
             * is, not the absence of one. */
            cd->first_us = dt > 0 ? (unsigned long long)dt : 1ULL;
        }
        cd->arrivals++;

        /* `who` is stopped AT base (ps_perform rewound it) with the trap byte
         * still planted, and it is ASMSPY_PS_OWN — only that cell of the table
         * yields an arrival, so "can this task poke the target's text" is
         * answered by construction here rather than by a second check.
         *
         * Take the byte out, step ONE instruction, put it back, release —
         * re-planting BEFORE the step would trap it again at the same pc,
         * forever.
         *
         * CHECKED, and bp_planted is cleared only once the byte is provably out.
         * The old order cleared the flag FIRST and discarded this poke's return:
         * a failure left the trap in the text while the bookkeeping said it was
         * out, so ps_unplant short-circuited, ps_disarm cleared bp_base and
         * reported clean, and the target died on its next arrival with the
         * sampler reporting success. That is C2 exactly, one function over. */
        /* ps_restore_copy, not a raw POKETEXT: the same A29 rule ps_unplant
         * now follows. `who` has been running since the plant, and a write
         * that has not looked at what is there is a write into whatever got
         * mapped at this address in the meantime (2026-08-06 final review,
         * finding 5). Its 0 means "our byte is not there any more", which is
         * exactly the state bp_planted = 0 records. */
        if (ps_restore_copy(who, cd->addr, c->bp_orig) != 0)
            break; /* leave bp_planted set: ps_disarm below owns the retry, and
                    * reports a leak if it cannot get the byte out either */
        c->bp_planted = 0;
        if (cd->arrivals < ASMSPY_PS_HIT_BUDGET) {
            c->stepping = who;
            int i = ps_find(c, who);
            if (ptrace(PTRACE_SINGLESTEP, who, NULL, NULL) == 0) {
                if (i >= 0)
                    c->t[i].stopped = 0;
                /* NULL: another thread tripping the (now removed) entry in this
                 * window is rewound and released, not parked — parking it would
                 * abandon `who` mid-step. */
                ps_pump(c, who, PS_STOP_US, PS_SPIN_US, NULL);
            }
            c->stepping = 0;
            int wi = ps_find(c, who);
            if (wi < 0)
                break; /* it exited under the step: nothing left to re-arm */
            /* Re-arm through `who` ONLY if `who` can poke the TARGET. A task in
             * a different address space would have the trap planted in ITS text
             * — arming nothing, and leaving that task to die of a byte we then
             * never restore. ps_can_poke also rejects a LISTENed thread, whose
             * POKETEXT is refused ESRCH and would silently lose the candidate. */
            if (!ps_can_poke(&c->t[wi]) || ps_plant(c, who, cd->addr) != 0)
                break;
        }
        ps_cont(c, who, 0); /* ours: the pipeline classified this stop */
    }

    /* Disarm unconditionally, from every exit above — including the ones that
     * broke out early. A trap left behind does not fail loudly: the target runs
     * on and dies on its NEXT arrival, with no tracer to take the SIGTRAP,
     * whole seconds after we are gone. ps_disarm keeps bp_base when it cannot
     * get the byte out, and sets `leaked`, which stops any further arming.
     *
     * PS_STOP_FOREVER, deliberately, and this is the ONE step allowed to
     * overrun the per-candidate bound: there is no deadline at which leaving
     * our byte in a live process is the better answer. */
    ps_disarm(c, PS_STOP_FOREVER);
    ps_resume_all(c);
}

/* ------------------------------------------------------------------ */
/* the entry point                                                     */
/* ------------------------------------------------------------------ */

static int ps_resolve(void *ctx, uint64_t addr, uint64_t *start, uint64_t *size,
                      const char **name, const char **module) {
    const asmspy_symtab_t *s = (const asmspy_symtab_t *)ctx;
    const asmspy_sym_t *f = asmspy_symtab_at(s, addr);
    if (!f)
        return -1;
    *start = f->addr;
    *size = f->size;
    *name = f->name;
    *module = f->module;
    return 0;
}

/* Descending by "arrives soonest": confirmed candidates only, ordered by
 * ascending first_us, ties by ascending address — never by input order, which
 * for a statistical sampler is a coin flip that would make the pick
 * unreproducible run to run on identical behaviour (the same determinism rule
 * both pure ranks in asmspy_autoregion.h carry). */
static void ps_sort(ps_cand_t *v, int n) {
    for (int i = 1; i < n; i++) {
        ps_cand_t key = v[i];
        int j = i;
        while (j > 0 && (v[j - 1].first_us > key.first_us ||
                         (v[j - 1].first_us == key.first_us &&
                          v[j - 1].addr > key.addr))) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = key;
    }
}

/* A self-skip, with its reason APPENDED — a run that reached the teardown may
 * already have something to say (no Capstone, an ungrasped thread set), and the
 * refusal is additional context, not a replacement for it. */
static int ps_skip(char *why, size_t whylen, const char *msg) {
    ps_note(why, whylen, msg);
    return -1;
}

int asmspy_ptrace_sample(pid_t pid, const asmspy_symtab_t *syms,
                         const char *module, asmspy_autocand_t *out, int max,
                         int window_ms, char *why, size_t whylen) {
    if (why && whylen)
        why[0] = '\0';
    if (pid <= 0 || !syms || !out || max <= 0)
        return ps_skip(why, whylen, "bad arguments to the ptrace sampler");
    if (syms->n == 0)
        return ps_skip(why, whylen,
                       "the target has no resolvable function symbols, so "
                       "nothing this sampler observed could be named");
    /* The same refusal every other engine here makes, for the same reason: this
     * file reads PCs and disassembles as the HOST arch, and on an i386 tracee
     * that does not fail — it produces confident nonsense. */
    if (asmspy_elf_class(pid) == 32)
        return ps_skip(why, whylen,
                       "32-bit tracee: this sampler decodes as the host "
                       "architecture and would name the wrong code");
    if (window_ms <= 0)
        window_ms = ASMSPY_PS_DEFAULT_WINDOW_MS;

    ps_ctx_t c;
    memset(&c, 0, sizeof c);
    c.pid = pid;
    c.cap = ps_read_cap();
    if (ps_seize_all(&c) < 0) {
        ps_detach_all(&c);
        return ps_skip(why, whylen,
                       "PTRACE_SEIZE refused: check ptrace_scope, or whether "
                       "the target is ours to trace");
    }

    /* ---- phase 1 -------------------------------------------------- */
    ps_sym_hit_t hits[PS_MAX_SYMS];
    unsigned long long taken = 0;
    int nh = ps_phase1(&c, syms, window_ms, hits, &taken);

    /* Fold through the SAME pure rank the software-clock sampler uses, so the
     * two portable samplers cannot disagree about what counts as a residency
     * candidate (size > 0, containment, the --module rule). out_cap == nh makes
     * it lossless, which is what that rank's doc comment asks of a caller. */
    ps_cand_t cands[ASMSPY_PS_MAX_CAND];
    int ncand = 0;
    if (nh > 0) {
        asmspy_ip_hit_t ips[PS_MAX_SYMS];
        for (int i = 0; i < nh; i++) {
            ips[i].ip = hits[i].start;
            ips[i].count = hits[i].count;
        }
        asmspy_autocand_t *ranked =
            (asmspy_autocand_t *)calloc((size_t)nh, sizeof *ranked);
        if (ranked) {
            size_t nr = asmspy_autoregion_rank_ip(ips, (size_t)nh, ps_resolve,
                                                  (void *)syms, module, ranked,
                                                  (size_t)nh);
            for (size_t i = 0; i < nr && ncand < ASMSPY_PS_MAX_CAND; i++) {
                const asmspy_sym_t *s = asmspy_symtab_at(syms, ranked[i].addr);
                ps_push_cand(cands, &ncand, s, module, ranked[i].arrivals);
            }
            free(ranked);
        }
    }
    int shortlist = ncand < ASMSPY_PS_SHORTLIST ? ncand : ASMSPY_PS_SHORTLIST;

    /* ---- phase 2 -------------------------------------------------- */
    /* Expanded from the residency shortlist, which is why `shortlist` is
     * snapshotted BEFORE the call: the callees this adds must not themselves be
     * re-scanned, or a deep call chain would fill the candidate table with
     * functions nothing ever observed running. */
    ps_phase2(&c, syms, module, cands, &ncand, shortlist, why, whylen);

    /* ---- phase 3 -------------------------------------------------- */
    /* THE ARM GATE. Phase 3's int3 is one byte of SHARED process text, so it is
     * only ever safe over a thread set that is entirely OURS. When it is not,
     * the answer is not "arm anyway and hope" — it is to hand back what phases 1
     * and 2 found, UNCONFIRMED, and say so. See asmspy_ps_arm_note. */
    /* THE ARM PRECONDITION. Be precise about which part does what, because the
     * three mechanisms here are easy to confuse and only two of them are safety:
     *
     *   - what keeps a task cloned MID-WINDOW safe is the table HEADROOM: it is
     *     always tabled, therefore always traced, therefore can never meet the
     *     trap untraced. That is the safety property.
     *   - what keeps the RESULT honest is the per-candidate re-check in the loop
     *     below and the post-loop re-read of `covered`: a run that lost coverage
     *     part-way stops arming and is not presented as a measurement.
     *   - the MARGIN (`n + PS_ARM_HEADROOM <= cap`) buys neither of those. It
     *     buys DETERMINISM: without it the answer depended on whether a victim's
     *     second worker existed at seize time or was cloned microseconds later,
     *     and the capped test failed about one run in three. A precondition that
     *     depends on a race is not a precondition. It is also conservative near
     *     the cap, which is the right direction to be wrong in. */
    int covered_before = c.covered && c.n + PS_ARM_HEADROOM <= c.cap;
    if (covered_before) {
        long per_us = ASMSPY_PS_CONFIRM_MS * 1000L;
        long long t3_end = ps_now_us() + (long long)window_ms * 1000;
        int nconf = 0;
        int lose_after = ps_read_lose_after(); /* test lever, see its doc */
        for (int i = 0; i < ncand && c.n > 0 && !c.leaked && c.covered; i++) {
            /* Stop early once the window's worth of confirming has been spent
             * AND there is already an answer to give. This only avoids charging
             * a caller who is running a WINDOW, not a search, for candidates it
             * no longer needs; when nothing has confirmed yet the remaining
             * candidates are exactly what is worth paying for.
             *
             * THE HARD BOUND, stated honestly (2026-08-06 final review, finding
             * 7). It is ASMSPY_PS_MAX_CAND x ASMSPY_PS_CONFIRM_MS — 800 ms at
             * the defaults — PLUS whatever each candidate's DISARM costs, and
             * only the disarm, because ps_phase3_one now runs its stop sweep and
             * its arrival wait against one deadline it takes at entry. The
             * earlier version started that clock after the sweep, so a thread in
             * uninterruptible sleep cost PS_STOP_US (200 ms) per sweep per
             * candidate OUTSIDE the advertised figure, with the target held
             * stopped throughout — this repo's own host notes record amdgpu
             * putting threads in exactly that state, and a D-state process
             * enters nothing, so the `nconf > 0` early break cannot fire either.
             * The disarm is left uncapped on purpose: see ps_phase3_one. */
            if (ps_now_us() >= t3_end && nconf > 0)
                break;
            ps_phase3_one(&c, &cands[i], per_us);
            if (cands[i].arrivals > 0)
                nconf++;
            /* TEST-ONLY (ASMSPY_PS_TEST_LOSE_AFTER): drop coverage right after
             * candidate `i` got its real phase-3 measurement, so cands[0..i]
             * carry genuine arrivals/first_us while the rest of the batch
             * never gets vetted -- the partial-confirmation shape a caller
             * must still refuse in full (2026-08-06 T7 review). Inert
             * (lose_after == -1) in real runs. */
            if (i == lose_after)
                c.covered = 0;
        }
    }
    /* Coverage is re-read AFTER the loop, not only before it. A task cloned
     * mid-window is tabled out of the headroom (safe) but still costs us full
     * coverage, and a run that LOST coverage part-way has confirmed only some
     * of its candidates against only some of the thread set — not a measurement
     * to present as one. Both the note and the drop-and-sort below key off
     * "covered at the start AND still covered at the end". */
    int confirmed = covered_before && c.covered;
    const char *arm_note = asmspy_ps_arm_note(confirmed);
    if (arm_note)
        ps_note(why, whylen, arm_note);

    /* The verdict is the TEARDOWN's, not the sticky flag's. `leaked` stops any
     * further arming the moment a disarm fails, which is right — but
     * ps_detach_all disarms again, and a leak it REPAIRS is not a damaged
     * target. Telling an operator "I broke your process" when it is provably
     * intact is its own harm. */
    int dirty = (ps_detach_all(&c) != 0);

    /* TEST-ONLY REPORT, and only when a lever manufactured the shape it counts.
     * Every one of these is a fact the caller cannot see from outside — whether
     * the window the lever opened actually caught anything — so without it the
     * checks built on those levers would pass on a run where nothing happened.
     * Absent from every real run: the levers are unset, so the numbers are not
     * printed, not merely zero. */
    if (ps_read_late_trap_ms() > 0 || ps_read_walk_pump_ms() > 0 ||
        ps_read_steal_byte() || ps_read_stop_cost_us() > 0) {
        char tn[192];
        snprintf(tn, sizeof tn,
                 "test-window: drained=%u post-disarm=%u late-detached=%u "
                 "guard-held=%u blind-writes=%u sweep-us=%llu sweep-clamped=%u "
                 "sweep-over=%u",
                 c.drain_traps, c.late_traps, c.late_detached, c.guard_held,
                 c.blind_writes, c.sweep_us, c.sweep_clamped, c.sweep_over);
        ps_note(why, whylen, tn);
    }

    /* A trap we could not remove, or a task we could not release, is not a
     * result with a caveat — it is a target we have DAMAGED. Refusing here
     * rather than returning a pick is the only honest answer: a caller acting on
     * a region inside a process that is about to take a SIGTRAP with no tracer
     * would be arming a second trap in a corpse. */
    if (dirty)
        return ps_skip(
            why, whylen,
            "the target could not be handed back clean — a trap byte "
            "or a seized task was left behind; do not trace this "
            "process further without checking it");

    /* Confirmation, not re-ordering: a candidate whose entry was never reached
     * is DROPPED. Arming one of those is exactly the hang this module exists to
     * avoid, and phase 3 measured that it would.
     *
     * When phase 3 was SKIPPED there is no such measurement, so nothing may be
     * dropped and nothing may be re-sorted: phase 1+2's own order (residency
     * rank, then the calls it named) is the best evidence available, and
     * ps_sort's key would be uniformly zero. The caller is told they are
     * unconfirmed and walks them the way it walks the software-clock rank's,
     * which carries exactly the same weaker guarantee. */
    int keep = 0;
    if (!confirmed) {
        keep = ncand;
    } else {
        for (int i = 0; i < ncand; i++)
            if (cands[i].arrivals > 0)
                cands[keep++] = cands[i];
        ps_sort(cands, keep);
    }

    int n = keep < max ? keep : max;
    for (int i = 0; i < n; i++) {
        out[i].addr = cands[i].addr;
        out[i].size = cands[i].size;
        out[i].name = cands[i].name;
        out[i].module = cands[i].module;
        out[i].arrivals = cands[i].arrivals;
        out[i].sites = cands[i].sites; /* phase 2's direct call sites; 0 for a
                                        * residency-only candidate */
        out[i].first_us = cands[i].first_us;
    }

    /* An empty window is a RETRY, not a verdict — but "nothing qualified" has
     * four different causes and they send an operator to four different places,
     * so say WHICH stage came up empty rather than handing back a bare 0.
     * Appended, not assigned: phase 2's Capstone note and the arm gate's note
     * are independent facts about the same window and must not overwrite. */
    if (n == 0) {
        const char *msg;
        if (taken == 0)
            msg = "no thread of the target was observed executing user code in "
                  "the window (every one was blocked in a syscall)";
        else if (nh == 0)
            msg = "every sampled program counter fell outside a sized function "
                  "— a stripped or JIT-only target";
        else if (ncand == 0)
            msg = "nothing the target was running passed the module filter";
        else
            msg = "no candidate entry was reached within the confirm budget — "
                  "the target may have changed phase";
        ps_note(why, whylen, msg);
    }
    return n;
}
