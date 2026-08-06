/* asmspy_ptracesample.c — the PERF-FREE region picker.
 *
 * The design note, the measurements it was tuned to, and the six corrections
 * this file carries live in cli/asmspy_ptracesample.h. What follows is the
 * machinery, phase by phase; each phase names the measurement that forced its
 * shape rather than restating the header.
 *
 * ONE RULE ABOVE ALL OTHERS, because the failure mode is silent and fatal to a
 * process we do not own: every ptrace-stop this file consumes is classified in
 * exactly ONE place, ps_dispatch, and only ps_dispatch decides what signal to
 * resume with. Every other resume in the file is resuming a stop ps_dispatch
 * has already classified as OURS (a PTRACE_INTERRUPT or our own int3), which is
 * why those may pass 0. A second classification site is how the prototype ate
 * 89% of a victim's SIGALRMs. */
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
    /* A fork/vfork CHILD: a different PROCESS that appeared under our options,
     * not a thread of the target. It must never be used to poke the target's
     * text (a fork child has its own address space, so restoring through it
     * would clear `bp_planted` while the parent's byte is still in) and must
     * never be sampled (its /proc path is not under our pid). */
    int foreign;
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
    long bp_orig;   /* the word POKETEXT overwrote              */
    pid_t stepping; /* the tid whose PTRACE_SINGLESTEP we await */
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
 * "another thread of our target" from "a process that forked out of it". */
static int ps_tgid_of(pid_t tid);

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
     * Unreadable /proc counts as FOREIGN. Misreading a thread as foreign costs
     * a sample and an early disarm; misreading a fork child as a thread kills
     * it. The costs are not symmetric, so neither is the default. */
    if (tid != c->pid) {
        int tgid = ps_tgid_of(tid);
        c->t[c->n].foreign = (tgid != (int)c->pid);
    }
    /* A candidate is armed right now: whatever this task is, if it took a COW
     * snapshot of the text it has our byte. bp_base rather than bp_planted,
     * because the step-and-rearm window clears bp_planted while a snapshot taken
     * before the restore still carries the trap. Restoring an original word that
     * is already in place is a no-op, so the conservative direction is free. */
    if (c->t[c->n].foreign && c->bp_base) {
        c->t[c->n].copied_bp = c->bp_base;
        c->t[c->n].copied_orig = c->bp_orig;
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

/* Resume a stop ps_dispatch has already classified. `sig` is what ps_dispatch
 * decided to deliver — 0 ONLY for a stop that was ours to begin with. */
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

/* The thread group `tid` belongs to, or -1 when /proc cannot answer. */
static int ps_tgid_of(pid_t tid) {
    char path[64], buf[1024];
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
    return 0;
}

/* Can this table entry POKE the target's text? A thread we actually hold in a
 * ptrace-stop, in the target's own address space. Excludes:
 *   - a running thread          (PEEK/POKETEXT are refused, silently)
 *   - a LISTENed thread         (group-stop, not a stop we hold: ESRCH)
 *   - a fork/vfork child        (its own mm, or a shared one we must not clear)
 */
static int ps_can_poke(const ps_thr_t *t) {
    return t->stopped && !t->listening && !t->foreign;
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
 * ours. 0 = the byte is out (or was never in), -1 = it is still in the target. */
static int ps_unplant(ps_ctx_t *c) {
    if (!c->bp_planted)
        return 0;
    for (int i = 0; i < c->n; i++)
        if (ps_can_poke(&c->t[i]) &&
            ptrace(PTRACE_POKETEXT, c->t[i].tid, (void *)(uintptr_t)c->bp_base,
                   (void *)(uintptr_t)c->bp_orig) == 0) {
            c->bp_planted = 0;
            return 0;
        }
    return -1;
}

/* THE SAFETY NET, lifted verbatim in spirit from rgn_rewind_from_bp. On x86 a
 * thread trap-stopped just past the int3 has pc == base+1, which is the MIDDLE
 * of the region's first real instruction once the original byte is back:
 * resuming it there executes garbage in a process we do not own. Every path
 * that sees a stopped thread rewinds it before letting it run. On AArch64 `brk`
 * faults AT base, so there is nothing to rewind and this only confirms the
 * thread really is at our trap.
 *
 * Returns 1 if `tid` is at the breakpoint (x86: and has been rewound). */
static int ps_rewind(pid_t tid, uint64_t base) {
    asmspy_regs_t regs;
    if (!base || asmspy_regs_read(tid, &regs) != 0)
        return 0;
#if defined(__aarch64__)
    return asmspy_reg_pc(&regs) == base;
#else
    if (asmspy_reg_pc(&regs) != base + 1)
        return 0;
    asmspy_set_pc(&regs, base);
    return asmspy_regs_write(tid, &regs) == 0;
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
/* THE event classifier — correction 1 lives here                      */
/* ------------------------------------------------------------------ */

/* Classify ONE waitpid result and act on it. `hold` is the tid whose stop the
 * caller wants kept (0 = none); everything else is resumed here.
 *
 * CORRECTION 1. The prototype had no classifier: it waitpid()ed and
 * unconditionally PTRACE_CONTed with sig=0. man 2 ptrace is unambiguous about
 * the cost — "If sig is 0, then a signal is not delivered" — and against a
 * 100 Hz ITIMER_REAL victim that destroyed 89% of the target's SIGALRMs and
 * collapsed its forward progress ~99% (2 utime ticks and 0 progress lines per
 * 2 s, against 200/93 both at baseline and after detach). The reason it shipped
 * is that the identical code costs a SIGNAL-FREE spinner about 1%, so the
 * "~1% overhead, no perturbation" figure was true and meaningless.
 *
 * Under PTRACE_SEIZE the two cases are distinguishable without guessing:
 *   - PTRACE_EVENT_STOP  => a group-stop or OUR PTRACE_INTERRUPT. Nothing was
 *                           being delivered, so resuming with 0 loses nothing.
 *   - event == 0         => a SIGNAL-DELIVERY-stop. WSTOPSIG is a signal that
 *                           the target was about to take, and it is delivered
 *                           by passing it back through PTRACE_CONT.
 * SIGTRAP is the one signal that needs a third question, since it is also how
 * our own trap and our own single-step report: PTRACE_GETSIGINFO's si_code
 * answers it. */
static int ps_dispatch(ps_ctx_t *c, pid_t w, int st, pid_t hold,
                       pid_t *arrived) {
    if (WIFEXITED(st) || WIFSIGNALED(st)) {
        ps_del(c, w);
        return PS_EV_GONE;
    }
    if (!WIFSTOPPED(st))
        return PS_EV_NONE;

    int sig = WSTOPSIG(st);
    int event = (st >> 16) & 0xff;

    /* A followed clone's very first stop is where we learn its tid. */
    int idx = ps_add(c, w);
    if (idx < 0) {
        /* Physically out of slots. `covered` is already false, so no FURTHER
         * candidate will arm — but the one armed RIGHT NOW still is, and
         * releasing an untraced task into an armed address space is the death
         * described at PS_MAX_THREADS. We cannot table it, so we cannot protect
         * it; what we CAN do is take the trap out of whichever address space it
         * is about to run in. Same mm (a thread): the candidate is disarmed
         * early — arrivals lost, nobody dies. Different mm (a fork child): this
         * is exactly the restore it needed. bp_planted is deliberately NOT
         * cleared, because we do not know which case this was, and a redundant
         * restore later is free while a skipped one leaks. */
        if (c->bp_base) {
            ptrace(PTRACE_POKETEXT, w, (void *)(uintptr_t)c->bp_base,
                   (void *)(uintptr_t)c->bp_orig);
            ps_rewind(w, c->bp_base);
        }
        ptrace(PTRACE_DETACH, w, NULL, NULL);
        return PS_EV_NONE;
    }

    /* A FORK CHILD holding a copy-on-write copy of our trap byte. Release it the
     * moment we can touch it: restore ITS copy in ITS address space, rewind it
     * off base+1, and detach. Left as it is, it dies of a SIGTRAP with no
     * tracer — the same fatality as an unseized thread, one address space over.
     * This runs BEFORE the job-control branch on purpose: a child's own attach
     * stop must not be mistaken for a stop the user asked for. */
    if (c->t[idx].foreign) {
        uint64_t cb = c->t[idx].copied_bp;
        if (!cb) {
            /* Nothing of ours to put back — it appeared while nothing was
             * armed. Hand it straight back; it is not our process to trace. */
            ps_rewind(w, c->bp_base);
            ptrace(PTRACE_DETACH, w, NULL, NULL);
            ps_del(c, w);
        } else if (ptrace(PTRACE_POKETEXT, w, (void *)(uintptr_t)cb,
                          (void *)(uintptr_t)c->t[idx].copied_orig) == 0) {
            ps_rewind(w, cb);
            ptrace(PTRACE_DETACH, w, NULL, NULL);
            ps_del(c, w);
        } else {
            /* Could not clean it here; keep it TRACED (so its trap is ours to
             * absorb) and try again at teardown. */
            c->t[idx].stopped = 1;
        }
        return PS_EV_NONE;
    }

    if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
        event == PTRACE_EVENT_VFORK) {
        c->t[idx].stopped = 1;
        /* CORRECTION 2, the half that makes the option useful: table the child
         * so it is followed and, above all, so the phase-3 int3 in the SHARED
         * text has a tracer when the new task reaches it. Do NOT resume the
         * child here — its own attach-stop has not necessarily arrived yet; it
         * surfaces in this same loop and is handled there. */
        unsigned long child = 0;
        if (ptrace(PTRACE_GETEVENTMSG, w, NULL, &child) == 0 && child &&
            event == PTRACE_EVENT_CLONE) {
            /* A THREAD, tabled HERE so it is followed even if its own
             * attach-stop is delayed. */
            if (ps_add(c, (pid_t)child) < 0)
                c->covered = 0; /* a task of the target we cannot follow */
        }
        /* A FORK/VFORK child is deliberately NOT tabled here. It is a different
         * PROCESS; it is classified from /proc and released at its OWN
         * attach-stop, which it cannot run past. Tabling it here re-adds a child
         * we may have ALREADY released — ptrace(2) does not order the two stops
         * — and such an entry never leaves, because a detached task never
         * reports again. MEASURED against forkhot_victim: the table reached 152
         * stale tids in a 900 ms window, after which every teardown walk paid
         * PS_STOP_US per corpse and the run took minutes. */
        ps_cont(c, w, 0);
        return PS_EV_NONE;
    }

    if (event == PTRACE_EVENT_STOP) {
        /* Job control (^Z, tty stops). PTRACE_LISTEN leaves the thread stopped
         * — honouring the stop the user asked for — yet traced; PTRACE_CONT
         * here would resume a process that is supposed to be suspended.
         *
         * `listening`, NOT `stopped`: a LISTENed tracee is in group-stop, which
         * is not a ptrace-stop we hold, so PEEK/POKE/CONT on it all fail ESRCH.
         * See ps_thr_t. */
        if ((sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
             sig == SIGTTOU) &&
            !c->tearing_down) {
            if (ptrace(PTRACE_LISTEN, w, NULL, NULL) == 0) {
                c->t[idx].listening = 1;
                c->t[idx].stopped = 0;
            } else {
                /* LISTEN refused: an ordinary held stop is wrong for job
                 * control but INFINITELY better than a thread nothing ever
                 * resumes, which is the only other option here. */
                c->t[idx].stopped = 1;
                ps_cont(c, w, 0);
            }
            return PS_EV_NONE;
        }
        c->t[idx].stopped = 1;
        c->t[idx].listening = 0;
        if (w == hold)
            return PS_EV_STOPPED; /* our PTRACE_INTERRUPT landed */
        ps_cont(c, w, 0);
        return PS_EV_NONE;
    }

    c->t[idx].stopped = 1;
    c->t[idx].listening = 0;

    if (sig == SIGTRAP) {
        /* our own PTRACE_SINGLESTEP over the entry (phase 3's step-and-rearm) */
        if (c->stepping == w)
            return PS_EV_STOPPED;
        /* OUR entry trap, identified by the pc — and ps_rewind has just made the
         * thread safe to run again. Whether the byte is still planted decides
         * only what happens NEXT: a caller collecting arrivals takes it, and
         * anyone else releases it. What must never happen is falling through to
         * the re-injection below, because an int3 reports si_code == SI_KERNEL
         * whoever planted it, and a target with no SIGTRAP handler dies of ours.
         * MEASURED: that is exactly how clone_victim died. */
        /* `!foreign` is load-bearing, not defensive. A fork child's COW copy of
         * our int3 traps at the SAME address, so without this its SIGTRAP is
         * indistinguishable from an arrival of the target — and phase 3 would
         * then count it, restore through it, and RE-PLANT through it, into a
         * different address space. It is released by the foreign branch above
         * instead. (A foreign task can only reach here at all if it was
         * classified after its first stop, which ps_add now prevents; the guard
         * is what makes that a design rather than a race.) */
        if (c->bp_base && !c->t[idx].foreign && ps_rewind(w, c->bp_base)) {
            if (arrived) {
                *arrived = w;
                return PS_EV_ARRIVED;
            }
            ps_cont(c, w, 0);
            return PS_EV_NONE;
        }
        /* The TARGET'S own int3 / hardware breakpoint: delivered, so its signal
         * machinery runs as it would untraced. A stray ptrace-synthesised trap
         * (SI_USER/SI_TKILL) is swallowed. */
        ps_cont(c, w, ps_sigtrap_is_app(w) ? SIGTRAP : 0);
        return PS_EV_NONE;
    }

    /* CORRECTION 1: a real signal, delivered. */
    ps_cont(c, w, sig);
    return PS_EV_NONE;
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
 * plant/restore goes through here first. 0 if stopped, -1 if it vanished. */
static int ps_ensure_stopped(ps_ctx_t *c, pid_t tid) {
    int i = ps_find(c, tid);
    if (i < 0)
        return -1;
    if (c->t[i].stopped)
        return 0;
    ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
    long long t0 = ps_now_us();
    while (ps_now_us() - t0 < PS_STOP_US) {
        /* `arrived` NULL: this is not an arrival collector. A thread that trips
         * the entry while we are getting `tid` stopped is rewound and RELEASED
         * inside ps_dispatch rather than parked here. */
        ps_pump(c, tid, PS_INTR_US, PS_SPIN_US, NULL);
        i = ps_find(c, tid);
        if (i < 0)
            return -1;
        if (c->t[i].stopped)
            return 0;
        /* The interrupt can be absorbed by a concurrent signal-delivery-stop
         * (which ps_dispatch consumed and re-injected); re-issue it. */
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
 * pump and is not in that round's snapshot. */
static void ps_stop_all(ps_ctx_t *c) {
    for (int round = 0; round < 2; round++) {
        pid_t snap[PS_TABLE_SLOTS];
        int n = 0;
        for (int i = 0; i < c->n; i++)
            if (!c->t[i].stopped && (!c->t[i].listening || c->tearing_down))
                snap[n++] = c->t[i].tid;
        if (n == 0)
            return;
        for (int i = 0; i < n; i++)
            ps_ensure_stopped(c, snap[i]);
    }
}

static void ps_resume_all(ps_ctx_t *c) {
    for (int i = 0; i < c->n; i++)
        /* NOT the listening ones: PTRACE_CONT on a group-stopped tracee either
         * fails or, worse, resumes a process the user deliberately suspended. */
        if (c->t[i].stopped && !c->t[i].listening)
            ps_cont(c, c->t[i].tid, 0); /* ours: see the file-header rule */
}

/* Stop everything, take the trap byte out, rewind anyone parked at base+1, and
 * only THEN forget the address.
 *
 * Two attempts, because the usual reason the first fails is that nothing was
 * stopped yet. If the byte is still in after that, `leaked` is set and bp_base
 * is KEPT: clearing it is how the address of a live int3 was lost, after which
 * teardown poked address zero and the target died seconds later with the
 * sampler reporting success. Returns 0 clean, -1 leaked. */
static int ps_disarm(ps_ctx_t *c) {
    ps_stop_all(c);
    if (ps_unplant(c) != 0) {
        ps_stop_all(c);
        if (ps_unplant(c) != 0) {
            c->leaked = 1;
            return -1;
        }
    }
    for (int i = 0; i < c->n; i++)
        ps_rewind(c->t[i].tid, c->bp_base);
    c->bp_base = 0; /* ONLY now, and only because the byte is provably out */
    return 0;
}

/* Detach ONE thread, insisting. PTRACE_DETACH requires a ptrace-stop; on a
 * running tracee it fails ESRCH — and the old code discarded that return, so the
 * thread stayed SEIZED after we returned, with the pump gone. Its next signal
 * then becomes a signal-delivery-stop nobody will ever consume and it hangs
 * PERMANENTLY. Returns 0 (detached, or it exited), -1 (still ours, and stuck). */
static int ps_detach_one(ps_ctx_t *c, pid_t tid) {
    for (int attempt = 0; attempt < PS_DETACH_TRIES; attempt++) {
        if (ptrace(PTRACE_DETACH, tid, NULL, NULL) == 0)
            return 0;
        if (ps_ensure_stopped(c, tid) != 0)
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
 * byte and dies of it. They were previously unset while ps_dispatch carried
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
 * Returns 0 if the target was handed back clean, -1 if anything was left behind
 * (a trap byte still in, or a task still seized) — which the caller must not
 * report as success. */
static int ps_detach_all(ps_ctx_t *c) {
    int bad = 0;
    /* From here on a job-control stop must NOT be re-LISTENed: PTRACE_DETACH is
     * refused ESRCH on a LISTENed tracee (MEASURED), and ps_detach_one's retry
     * would re-LISTEN it on every round and never converge. Interrupting it
     * out of LISTEN and detaching from the resulting group-stop leaves the
     * process in state 'T' — still suspended, exactly as the user left it. */
    c->tearing_down = 1;
    if (c->bp_base && ps_disarm(c) != 0)
        bad = 1;
    else
        ps_stop_all(c);

    for (int i = 0; i < c->n; i++) {
        /* A fork child still carrying its private copy of a trap byte: its own
         * address space, its own restore. */
        if (c->t[i].copied_bp) {
            if (ptrace(PTRACE_POKETEXT, c->t[i].tid,
                       (void *)(uintptr_t)c->t[i].copied_bp,
                       (void *)(uintptr_t)c->t[i].copied_orig) != 0)
                bad = 1;
            else
                ps_rewind(c->t[i].tid, c->t[i].copied_bp);
        }
        ps_rewind(c->t[i].tid, c->bp_base);
    }
    /* Snapshot again: ps_detach_one can drop a vanished tid mid-walk. */
    pid_t snap[PS_TABLE_SLOTS];
    int n = 0;
    for (int i = 0; i < c->n; i++)
        snap[n++] = c->t[i].tid;
    for (int i = 0; i < n; i++)
        if (ps_detach_one(c, snap[i]) != 0)
            bad = 1;

    c->bp_base = 0;
    c->n = 0;
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
 * retried: it was absorbed by a signal-delivery-stop that ps_dispatch consumed
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
            if (c->t[i].foreign || c->t[i].listening)
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
                ps_cont(c, tid, 0); /* ours: ps_dispatch classified it */
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
 * promptly" is not predicted here, it is MEASURED, with the same primitive. */
static void ps_phase3_one(ps_ctx_t *c, ps_cand_t *cd, long budget_us) {
    ps_stop_all(c);
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
        long left = (long)(budget_us - (ps_now_us() - t0));
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

        /* `who` is stopped AT base (ps_dispatch rewound it) with the trap byte
         * still planted. Take the byte out, step ONE instruction, put it back,
         * release — re-planting BEFORE the step would trap it again at the same
         * pc, forever. `who` is stopped, so it can restore its own text. */
        c->bp_planted = 0;
        ptrace(PTRACE_POKETEXT, who, (void *)(uintptr_t)cd->addr,
               (void *)(uintptr_t)c->bp_orig);
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
        ps_cont(c, who, 0); /* ours: ps_dispatch classified this stop */
    }

    /* Disarm unconditionally, from every exit above — including the ones that
     * broke out early. A trap left behind does not fail loudly: the target runs
     * on and dies on its NEXT arrival, with no tracer to take the SIGTRAP,
     * whole seconds after we are gone. ps_disarm keeps bp_base when it cannot
     * get the byte out, and sets `leaked`, which stops any further arming. */
    ps_disarm(c);
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
        for (int i = 0; i < ncand && c.n > 0 && !c.leaked && c.covered; i++) {
            /* Stop early once the window's worth of confirming has been spent
             * AND there is already an answer to give. The HARD bound is the loop
             * itself (ASMSPY_PS_MAX_CAND x ASMSPY_PS_CONFIRM_MS, 800 ms at the
             * defaults); this only avoids charging a caller who is running a
             * WINDOW, not a search, for candidates it no longer needs. When
             * nothing has confirmed yet the remaining candidates are exactly
             * what is worth paying for. */
            if (ps_now_us() >= t3_end && nconf > 0)
                break;
            ps_phase3_one(&c, &cands[i], per_us);
            if (cands[i].arrivals > 0)
                nconf++;
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
