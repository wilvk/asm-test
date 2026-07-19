/*
 * ss_backend.c — single-step (EFLAGS.TF / #DB -> SIGTRAP) native-trace backend.
 * See docs/internal/plans/zen2-singlestep-trace-plan.md, asmtest_hwtrace.h,
 * docs/native-tracing.md.
 *
 * Where the Intel PT / AMD LBR backends reconstruct the instruction stream from a
 * hardware *trace* (a PT AUX ring, or a 16-deep branch stack), this backend gets
 * the SAME exact stream from the x86 single-step debug exception. With EFLAGS.TF
 * set, the CPU raises a trap-class #DB after every instruction, delivered as
 * SIGTRAP; the handler reads RIP and records it. The trade is exact-cheap (a trace
 * ring) vs. exact-slow (a fault per instruction) — so this is the exact,
 * zero-dependency, unprivileged backend for SMALL registered routines on ANY
 * x86-64 host (Intel, any-Zen AMD, VM, CI, container), and the only exact
 * in-process option where no branch-trace facility exists (e.g. AMD Zen 2).
 *
 * Output is identical to the other backends: ordered in-region instruction
 * offsets matching Unicorn / DynamoRIO / Intel PT, and block offsets that match
 * after the same single-entry/ends-at-branch normalization.
 *
 * PER-THREAD STATE (scoped-tracing-core-plan §1). The capture state is a per-thread
 * TLS range stack, so nested begin/end pairs on one thread COMPOSE (innermost frame
 * included in the outer) and concurrent scopes on different threads are independent
 * — lifting the single-region/single-thread MVP. The handler dereferences the range
 * stack in signal context, so the TLS objects it touches (tls_frames, tls_depth) are
 * forced to the INITIAL-EXEC model: the general-dynamic first-touch of a __thread var
 * in a dlopen'd .so can route through __tls_get_addr and lazily malloc, which is not
 * async-signal-safe. The 512 KiB ordered-RIP buffer stays heap-malloc'd (never TLS);
 * only pointers/offsets live in the (small, fixed-depth) TLS stack. g_armed stays a
 * process-global non-TLS atomic — a coarse belt that spares the UNARMED early-return
 * path from touching TLS at all; the real per-thread "am I stepping" gate is the TLS
 * range-stack depth. The process-wide SIGTRAP disposition is installed once under an
 * explicit arm-refcount (a second concurrent begin must not overwrite g_old_sa with
 * asm-test's own handler).
 *
 * Signal-safety: the SIGTRAP handler does ONLY async-signal-safe work — a
 * bounds-checked store of each in-region RIP offset into a preallocated buffer,
 * and re-asserting TF in the saved context. All Capstone-based block normalization
 * (which allocates) runs in a post-pass in asmtest_ss_end(), in normal context.
 *
 * MACOS-INTEL FRONT-END (zen2-singlestep-trace-plan.md Phase 5). The mechanism is
 * identical on Darwin/x86-64: TF is set with the same pushfq/popfq, the #DB the CPU
 * raises is translated by XNU into a BSD SIGTRAP (EXC_BREAKPOINT falls through to the
 * signal path when no Mach exception port claims it — i.e. no debugger attached), and
 * re-asserting TF in the saved thread state re-arms stepping across sigreturn exactly
 * as on Linux. The ONLY platform differences are (1) the feature-test macro that
 * exposes the machine context and (2) the mcontext field access: Linux reaches RIP/
 * EFLAGS through uc_mcontext.gregs[REG_RIP]/[REG_EFL], Darwin through the
 * uc_mcontext POINTER's __ss.__rip/__ss.__rflags. Both are isolated behind the small
 * SS_RIP()/SS_SET_TF() shims below; everything else (TLS range stack, arm-refcount,
 * Capstone post-pass) is shared verbatim.
 */
/* Must precede every include so the machine context is fully exposed: glibc hides
 * REG_RIP/REG_EFL behind _GNU_SOURCE; Darwin hides ucontext_t/mcontext_t behind the
 * default (non-strict) visibility, which _DARWIN_C_SOURCE guarantees even under
 * -std=c11 (which would otherwise define __STRICT_ANSI__ and hide them). */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include "asmtest_hwtrace.h" /* ASMTEST_HW_* status codes (shared, not re-#define'd) */
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))

#include <dlfcn.h> /* dlsym(RTLD_DEFAULT, …) — resolve the default blocking-libc denylist */
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> /* mmap/munmap (§Z1 sparse whole-window buffer) */
#include <sys/time.h> /* setitimer/ITIMER_REAL — the whole-window watchdog (T2) */
#include <unistd.h> /* syscall (§Z4 arm-tid) */
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <sys/syscall.h> /* SYS_gettid (Linux) */
#include <ucontext.h>
#endif

/* Machine-context shims (the only platform-specific reads in the handler). Darwin's
 * uc_mcontext is a pointer to struct __darwin_mcontext64; Linux's is an inline struct
 * with a gregs[] array indexed by REG_RIP/REG_EFL. */
#if defined(__APPLE__)
#define SS_RIP(uc)    ((uint64_t)(uc)->uc_mcontext->__ss.__rip)
#define SS_SET_TF(uc) ((uc)->uc_mcontext->__ss.__rflags |= SS_TF)
/* §Z4: the OS thread id that armed a frame (region-free close assert). Linux uses the
 * gettid syscall; Darwin uses the 64-bit pthread thread id, narrowed to int. */
#define SS_ARM_TID() ss_darwin_tid()
#else
#define SS_RIP(uc)    ((uint64_t)(uc)->uc_mcontext.gregs[REG_RIP])
#define SS_SET_TF(uc) ((uc)->uc_mcontext.gregs[REG_EFL] |= (greg_t)SS_TF)
#define SS_ARM_TID()  ((int)syscall(SYS_gettid))
#endif

#if defined(__APPLE__)
static inline int ss_darwin_tid(void) {
    uint64_t t = 0;
    pthread_threadid_np(NULL, &t);
    return (int)t;
}
#endif

#define SS_TF 0x100ULL /* EFLAGS.TF, bit 8 */

/* Capacity of each frame's internal ordered-RIP buffer. The handler stores every
 * executed in-region instruction offset here (async-signal-safe array write); the
 * post-pass replays it to fill the trace and derive blocks. Sized for the
 * small-routine envelope; a routine that executes more in-region instructions than
 * this overflows and is honestly flagged truncated (never emit partial as complete). */
#ifndef SS_STREAM_CAP
#define SS_STREAM_CAP                                                          \
    (1u << 16) /* 65536 offsets = 512 KiB (region frames: malloc'd) */
#endif

/* WHOLE-WINDOW capacity (§Z1). A region frame captures a small leaf, but a region-free
 * whole-window frame records EVERY instruction the thread runs — for a managed caller
 * that is the whole runtime, far past 64k. So its buffer is a large SPARSE mmap
 * (lazily page-committed, so RAM cost is only what is actually recorded) instead of a
 * fixed malloc: the cap becomes millions of instructions at ~zero cost for small
 * windows. Still bounded (overflow → truncated), just a much higher, cheap ceiling.
 * Writing to an as-yet-uncommitted anonymous page from the SIGTRAP handler takes a
 * transparent minor page fault (kernel-serviced, not a userspace signal) — safe. */
#ifndef SS_WINDOW_CAP
#define SS_WINDOW_CAP (1u << 20) /* 1,048,576 addresses = 8 MiB VA, sparse */
#endif

/* Per-thread nesting depth bound. Kept SMALL and fixed because the range stack is
 * initial-exec TLS and draws on the shared static-TLS surplus (~1–2 KiB; exhaustion
 * fails a later dlopen with "cannot allocate memory in static TLS block"). */
#ifndef SS_MAX_FRAMES
#define SS_MAX_FRAMES 8
#endif

/* One active capture frame. base/base_ip/len/trace describe the region; stream is
 * the heap ordered-RIP buffer; gen tags the frame for stale-handle rejection.
 *
 * WHOLE-WINDOW (§Z1, the region-free empty-ctor form). When `whole_window` is set the
 * frame has NO [base,len): base=NULL, base_ip=0, len=0. The handler then records the
 * ABSOLUTE rip of every instruction this thread executes in the window (no in-range
 * filter — "trace whatever ran"), and the post-pass appends those absolute addresses
 * to the trace as-is (a whole-window trace's insns[] hold absolute addresses, which
 * asmtest_hwtrace_render_window / _render_versioned decode; contrast the region form's
 * base-relative offsets). It overflows the same bounded ring and is honestly flagged
 * `truncated` — a DESCEND_ALL window emits far more RIPs than any leaf. */
typedef struct {
    const uint8_t *base;
    uint64_t base_ip;
    size_t len;
    asmtest_trace_t *trace;
    uint64_t *
        stream; /* ordered RIPs (offsets, or absolute if whole); malloc or mmap */
    size_t
        cap; /* stream capacity in entries (SS_STREAM_CAP or SS_WINDOW_CAP)  */
    volatile uint32_t stream_len;
    volatile sig_atomic_t overflow;
    uint32_t gen;
    int whole_window; /* §Z1: 1 => record absolute RIPs, no [base,len) filter; mmap'd */
    int arm_tid; /* §Z4: OS tid that armed this frame (region-free close assert) */
    /* Whole-window guards (zeroconfig-scoped-tracing-hardening T1/T2). All scalars, so
     * they survive the frame's pop (only `stream` is freed) — T3's window_guard accessor
     * reads `guard` render-on-close style. steps/insn_budget bound instruction count;
     * watchdog_ms records whether this frame armed the wall-clock watchdog (so its pop
     * can drop the refcount); guard is the sig_atomic_t stop reason the handler writes. */
    uint64_t steps; /* whole-window instructions stepped so far          */
    uint64_t
        insn_budget; /* step ceiling (UINT64_MAX => disabled)             */
    uint32_t
        watchdog_ms; /* armed watchdog ms (UINT32_MAX => none armed)      */
    volatile sig_atomic_t
        guard; /* SS_GUARD_* stop reason (0 => still stepping)      */
} ss_frame_t;

/* Which guard stopped a whole-window frame (mirrored 1:1 by the public
 * ASMTEST_HW_GUARD_* enum in asmtest_hwtrace.h; T3's accessor returns this scalar). */
enum {
    SS_GUARD_NONE = 0,
    SS_GUARD_RING,     /* bounded capture ring overflowed (keeps stepping)  */
    SS_GUARD_BUDGET,   /* per-frame instruction budget reached              */
    SS_GUARD_DENY,     /* stepped RIP fell in a process-global deny region  */
    SS_GUARD_WATCHDOG, /* the ITIMER_REAL/SIGALRM watchdog expired          */
};

/* Whole-window guard defaults (resolved in ss_push_frame when the caller passes 0).
 * The budget is 4x the ring cap (4,194,304 steps): chosen so the shipped runaway-loop
 * test — which runs ~1.8 M instructions — still hits the RING first and stays
 * byte-identical. The watchdog is 10 s, not the descent tier's 2 s: this is a CI-hang
 * bound, not a perf knob, and must sit far above the runaway test's few seconds of
 * legitimate stepping so existing assertions stay deterministic. */
#define SS_WINDOW_BUDGET_DEFAULT      (4ull * (uint64_t)SS_WINDOW_CAP)
#define SS_WINDOW_WATCHDOG_MS_DEFAULT 10000u

/* Process-global deny table (NOT per-frame: it must not bloat the initial-exec static
 * TLS surplus). A stepped whole-window RIP inside any live entry ends that frame's
 * capture. Writes happen only in NORMAL context under g_ss_lock, entry first then the
 * length (append-only publish), so the lock-free handler read is safe. Cleared when the
 * arm-refcount drops to 0 (asmtest_ss_end). */
#define SS_MAX_DENY 32
static struct {
    uint64_t base, len;
} g_deny[SS_MAX_DENY];
static volatile uint32_t g_deny_len;

/* Per-thread range stack + depth + generation counter. INITIAL-EXEC: the handler
 * dereferences these in signal context (see file header). */
static __thread ss_frame_t tls_frames[SS_MAX_FRAMES]
    __attribute__((tls_model("initial-exec")));
static __thread int tls_depth __attribute__((tls_model("initial-exec")));
static __thread uint32_t tls_gen_ctr __attribute__((tls_model("initial-exec")));

/* Process-global coarse belt: nonzero iff ANY thread is stepping. Spares an unarmed
 * thread's handler from touching TLS; the real per-thread gate is tls_depth > 0. */
static volatile sig_atomic_t g_armed;

/* Process-wide SIGTRAP disposition, gated by an explicit arm-refcount: install on
 * the 0->1 transition, restore on 1->0. A plain mutex (off the signal path) makes
 * the count + install/restore atomic across concurrent begins. */
static struct sigaction g_old_sa;
static int g_installed;
static int g_arm_refcount;
static pthread_mutex_t g_ss_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- Whole-window deny table (T1) --------------------------------------------- */

/* Append one deny region. Caller holds g_ss_lock; publish is entry-first then length
 * so the lock-free handler read (ss_rip_denied) never sees a half-written entry. */
static void ss_deny_append(uint64_t base, uint64_t len) {
    uint32_t n = g_deny_len;
    if (n >= SS_MAX_DENY || len == 0)
        return; /* full or empty extent — silently skipped (best-effort bound) */
    g_deny[n].base = base;
    g_deny[n].len = len;
    g_deny_len = n + 1; /* publish AFTER the entry is fully written */
}

/* The lazily-resolved default deny set: blocking-libc entry points that would step the
 * runtime forever if a window reached them. Resolved ONCE per process in normal context
 * via dlsym(RTLD_DEFAULT, …) (async-signal-UNSAFE — never in the handler), 64-byte
 * extent per entry (enough to catch the entry stub). This mirrors the fork-path half of
 * the descent tier's asmtest_descent_use_default_denylist (src/ptrace_backend.c
 * descend_deny_syms). DELIBERATELY EXCLUDES `write`: Console/log output inside a managed
 * window would otherwise end nearly every capture at the first print — the divergence
 * from the descent set, which CAN deny write because there it means "step over", not
 * "stop the capture". */
static const char *const ss_deny_default_syms[] = {
    "read",  "poll",    "select", "epoll_wait", "nanosleep", "usleep",  "sleep",
    "wait4", "waitpid", "accept", "connect",    "recvmsg",   "sem_wait"};
static struct {
    uint64_t base, len;
} g_deny_default[sizeof ss_deny_default_syms / sizeof *ss_deny_default_syms];
static uint32_t g_deny_default_len;
static int g_deny_default_resolved;

/* Resolve the default deny symbols once (caller holds g_ss_lock). */
static void ss_deny_resolve_default(void) {
    if (g_deny_default_resolved)
        return;
    g_deny_default_resolved = 1;
    for (size_t i = 0;
         i < sizeof ss_deny_default_syms / sizeof *ss_deny_default_syms; i++) {
        void *sym = dlsym(RTLD_DEFAULT, ss_deny_default_syms[i]);
        if (sym != NULL) {
            g_deny_default[g_deny_default_len].base = (uint64_t)(uintptr_t)sym;
            g_deny_default[g_deny_default_len].len = 64;
            g_deny_default_len++;
        }
    }
}

/* Append the (cached) default deny set into the active table. Caller holds g_ss_lock. */
static void ss_deny_append_default(void) {
    ss_deny_resolve_default();
    for (uint32_t i = 0; i < g_deny_default_len; i++)
        ss_deny_append(g_deny_default[i].base, g_deny_default[i].len);
}

/* Handler-side (lock-free) deny check: is `rip` in any published deny region? Worst
 * case is SS_MAX_DENY (32) integer compares per trap — noise against the ~1000x cost of
 * a single-step #DB. Reads the length first, then entries[0..length): the append-only
 * publish order makes that safe without a lock. */
static inline int ss_rip_denied(uint64_t rip) {
    uint32_t n = g_deny_len;
    for (uint32_t i = 0; i < n; i++)
        if (rip >= g_deny[i].base && rip < g_deny[i].base + g_deny[i].len)
            return 1;
    return 0;
}

/* ---- Whole-window wall-clock watchdog (T2) ------------------------------------ */

/* A whole-window frame armed with a watchdog is bounded in wall-clock time even when
 * the stepped code blocks in a syscall: a repeating ITIMER_REAL fires SIGALRM, whose
 * handler only sets a flag; the SA_RESTART-cleared disposition makes the blocked syscall
 * return EINTR (breaking the block), and the next trap observes the flag, stops the
 * frame, and flags it truncated. Mirrors descend_watchdog_arm/_disarm
 * (src/ptrace_backend.c) exactly — the difference is only in the semantics of the stop
 * (in-process "stop the capture" vs out-of-process "the loop terminates the tracee").
 *
 * Two honest limitations (same as the descent tier): (a) ITIMER_REAL's SIGALRM is
 * delivered PROCESS-WIDE — in a multi-threaded process the kernel may deliver it to a
 * thread other than the blocked stepping one; the flag + the repeating interval make
 * expiry eventually observed at the next trap, but breaking a *blocked* syscall is only
 * guaranteed when the stepping thread can receive SIGALRM (single-threaded windows do;
 * a timer_create+SIGEV_THREAD_ID upgrade is the recorded hardening if a real
 * multi-thread consumer needs it). (b) The EINTR is visible to the traced code — the
 * same intrusiveness class as single-stepping itself; the trace is flagged, never
 * silently wrong. */
static volatile sig_atomic_t g_ww_alarm_fired;
static struct sigaction g_ww_old_sa;
static struct itimerval g_ww_old_it;
static int
    g_ww_refcount; /* whole-window frames that armed a watchdog (arm on 0->1) */

static void ss_window_alarm(int sig) {
    (void)sig;
    g_ww_alarm_fired = 1; /* only sets the flag — async-signal-safe */
}

/* Arm the repeating watchdog. Caller holds g_ss_lock; clears the fired flag, saves the
 * previous SIGALRM disposition AND the previous itimer for exact restore on disarm. */
static void ss_window_watchdog_arm(uint32_t ms) {
    g_ww_alarm_fired = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = ss_window_alarm;
    sa.sa_flags =
        0; /* NO SA_RESTART: SIGALRM must interrupt a blocked syscall (EINTR) */
    sigaction(SIGALRM, &sa, &g_ww_old_sa);
    struct itimerval it;
    it.it_value.tv_sec = ms / 1000u;
    it.it_value.tv_usec = (long)(ms % 1000u) * 1000L;
    /* Re-fire periodically so a single missed signal still breaks a persistent block. */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 100 * 1000L;
    if (it.it_value.tv_sec == 0 && it.it_value.tv_usec == 0)
        it.it_value.tv_usec = 1000L;
    setitimer(ITIMER_REAL, &it, &g_ww_old_it);
}

/* Restore the caller's SIGALRM disposition + itimer exactly (caller holds g_ss_lock). */
static void ss_window_watchdog_disarm(void) {
    setitimer(ITIMER_REAL, &g_ww_old_it, NULL);
    sigaction(SIGALRM, &g_ww_old_sa, NULL);
}

/* Set / clear EFLAGS.TF for the current thread. `or`/`and` on the flags image
 * pushed by pushfq; the instruction *after* popfq is the first to trap when
 * setting, and clearing via popfq suppresses the trap that would follow it. */
static inline void ss_arm_tf(void) {
    __asm__ __volatile__("pushfq\n\torq $0x100,(%%rsp)\n\tpopfq"
                         :
                         :
                         : "cc", "memory");
}
static inline void ss_disarm_tf(void) {
    __asm__ __volatile__("pushfq\n\tandq $-257,(%%rsp)\n\tpopfq" /* ~0x100 */
                         :
                         :
                         : "cc", "memory");
}

/* The trap handler: record the in-region offset into EVERY active frame on this
 * thread whose range contains RIP (so a nested inner region is a subset of its
 * outer), re-assert TF, return. No Capstone, no malloc, no locks — all deferred. */
static void ss_on_sigtrap(int sig, siginfo_t *si, void *uctx) {
    (void)sig;
    (void)si;
    if (!g_armed)
        return;        /* process-global belt: no thread stepping */
    int d = tls_depth; /* initial-exec TLS read: async-signal-safe */
    if (d <= 0)
        return; /* this thread isn't stepping (e.g. mid-disarm after depth->0) */
    ucontext_t *uc = (ucontext_t *)uctx;
    uint64_t rip = SS_RIP(uc);

    for (int i = 0; i < d; i++) {
        ss_frame_t *f = &tls_frames[i];
        if (f->whole_window) {
            /* §Z1: region-free — record the ABSOLUTE rip of every executed
             * instruction (no [base,len) filter). Bounded by the sparse-mmap cap;
             * writing an uncommitted page here takes a transparent minor fault.
             *
             * The four guards (T1/T2) are integer compares/writes only — no
             * allocation, no locks, no syscalls, so the handler stays
             * async-signal-safe. Once a stop reason latches (guard != NONE) the frame
             * is done; stepping ceases for it (see the TF re-assert gate below). */
            if (f->guard != SS_GUARD_NONE)
                continue; /* already stopped — nothing more to record */
            /* Watchdog (T2): expiry is observed here, at the next trap after SIGALRM
             * broke a blocked syscall (or between two stepped instructions). */
            if (g_ww_alarm_fired) {
                f->guard = SS_GUARD_WATCHDOG;
                continue; /* do not record; the denied/expired RIP is not ours */
            }
            /* Deny (T1): a stepped RIP inside a denied region ends the capture — the
             * denied call then runs at native speed (in-process "stop", vs the descent
             * tier's "step over"). Do NOT record this RIP. */
            if (ss_rip_denied(rip)) {
                f->guard = SS_GUARD_DENY;
                continue;
            }
            /* Budget (T1): bound the instruction count. The step that trips the budget
             * is still recorded, so a budget of N yields ~N instructions. */
            f->steps++;
            if (f->steps >= f->insn_budget)
                f->guard = SS_GUARD_BUDGET;
            /* Ring: record + overflow. On the FIRST overflow latch SS_GUARD_RING but
             * KEEP stepping — ring overflow bounds memory, not perturbation (the
             * shipped behaviour, unchanged). */
            if (f->stream_len < f->cap) {
                f->stream[f->stream_len++] = rip;
            } else {
                f->overflow = 1;
                if (f->guard == SS_GUARD_NONE)
                    f->guard = SS_GUARD_RING;
            }
        } else if (rip >= f->base_ip && rip < f->base_ip + f->len) {
            if (f->stream_len < f->cap)
                f->stream[f->stream_len++] = rip - f->base_ip;
            else
                f->overflow = 1;
        }
    }

    /* Re-assert TF so sigreturn resumes stepping — but ONLY if at least one frame on
     * this thread still wants stepping: any region frame (always steps), or any
     * whole-window frame not yet stopped (guard NONE, or RING which keeps stepping).
     * When no frame wants it, do NOT re-assert: stepping ceases and the window's
     * remainder runs at full speed until end_window (the guard-fired path). */
    int want_step = 0;
    for (int i = 0; i < d; i++) {
        ss_frame_t *f = &tls_frames[i];
        if (!f->whole_window) {
            want_step = 1;
            break;
        }
        if (f->guard == SS_GUARD_NONE || f->guard == SS_GUARD_RING) {
            want_step = 1;
            break;
        }
    }
    if (want_step)
        SS_SET_TF(uc);
}

/* Release a frame's ordered-RIP buffer with the matching deallocator: whole-window
 * frames are sparse-mmap'd (munmap by capacity), region frames are malloc'd. */
static void ss_free_stream(ss_frame_t *f) {
    if (f->stream == NULL)
        return;
    if (f->whole_window)
        munmap(f->stream, f->cap * sizeof(uint64_t));
    else
        free(f->stream);
    f->stream = NULL;
}

/* Push a capture frame on this thread. On the OUTERMOST push, install the
 * process-wide SIGTRAP disposition (0->1 arm-refcount) and arm this thread's TF.
 * Returns the frame handle (idx/gen) via out params (either may be NULL), or a
 * negative status: EFULL when this thread's range stack is full, EINVAL on bad args
 * / allocation / sigaction failure. `whole_window` selects the region-free §Z1 mode
 * (base/len must be NULL/0); the region mode requires a non-NULL base + nonzero len. */
static int ss_push_frame(const void *base, size_t len, asmtest_trace_t *trace,
                         int whole_window, uint64_t insn_budget,
                         uint32_t watchdog_ms, int use_default_denylist,
                         const asmtest_hwtrace_deny_t *deny, size_t deny_len,
                         uint32_t *out_idx, uint32_t *out_gen) {
    if (whole_window) {
        if (base != NULL || len != 0)
            return ASMTEST_HW_EINVAL; /* region-free frame carries no [base,len) */
        /* Resolve whole-window guard defaults (T1/T2). 0 => default; the sentinel
         * (UINT64_MAX / UINT32_MAX) stays disabled. Region frames pass the disabled
         * sentinels and never consult these (their handler branch has no guards). */
        if (insn_budget == 0)
            insn_budget = SS_WINDOW_BUDGET_DEFAULT;
        if (watchdog_ms == 0)
            watchdog_ms = SS_WINDOW_WATCHDOG_MS_DEFAULT;
    } else if (base == NULL || len == 0) {
        return ASMTEST_HW_EINVAL;
    }
    if (tls_depth >= SS_MAX_FRAMES)
        return ASMTEST_HW_EFULL; /* this thread's range stack is full */
    /* Region frames use a fixed malloc; whole-window frames use a large SPARSE mmap
     * (lazily committed) so the cap is millions of instructions at ~zero cost for
     * small windows. Either failure is a clean EINVAL. */
    size_t cap = whole_window ? (size_t)SS_WINDOW_CAP : (size_t)SS_STREAM_CAP;
    uint64_t *stream;
    if (whole_window) {
        stream = (uint64_t *)mmap(NULL, cap * sizeof(uint64_t),
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stream == MAP_FAILED)
            return ASMTEST_HW_EINVAL;
    } else {
        stream = (uint64_t *)malloc(cap * sizeof(uint64_t));
        if (stream == NULL)
            return ASMTEST_HW_EINVAL;
    }

    int idx = tls_depth;
    ss_frame_t *f = &tls_frames[idx];
    f->base = (const uint8_t *)base;
    f->base_ip = whole_window ? 0 : (uint64_t)(uintptr_t)base;
    f->len = len;
    f->trace = trace;
    f->stream = stream;
    f->cap = cap;
    f->stream_len = 0;
    f->overflow = 0;
    f->gen = ++tls_gen_ctr;
    f->whole_window = whole_window;
    f->arm_tid = SS_ARM_TID();
    /* Whole-window guard state (T1/T2). Region frames carry disabled values (their
     * handler branch never consults them). watchdog_ms records whether THIS frame
     * armed the shared watchdog, so its pop can drop the refcount. */
    f->steps = 0;
    f->guard = SS_GUARD_NONE;
    f->insn_budget = whole_window ? insn_budget : UINT64_MAX;
    f->watchdog_ms = whole_window ? watchdog_ms : UINT32_MAX;
    if (out_idx != NULL)
        *out_idx = (uint32_t)idx;
    if (out_gen != NULL)
        *out_gen = f->gen;

    /* Process-wide SIGTRAP install on the 0->1 arm-refcount transition. Save the
     * caller's original disposition into g_old_sa ONLY then, so a second concurrent
     * begin cannot overwrite it with asm-test's own just-installed handler. The deny
     * table + watchdog (T1/T2) are published under the SAME lock, BEFORE depth/TF are
     * armed below, so the handler never reads a half-built guard set. */
    pthread_mutex_lock(&g_ss_lock);
    if (g_arm_refcount == 0) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = ss_on_sigtrap;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGTRAP, &sa, &g_old_sa) != 0) {
            pthread_mutex_unlock(&g_ss_lock);
            ss_free_stream(f);
            return ASMTEST_HW_EINVAL;
        }
        g_installed = 1;
    }
    g_arm_refcount++;
    if (whole_window) {
        /* T1: publish this window's deny regions (default set + caller extras). */
        if (use_default_denylist)
            ss_deny_append_default();
        if (deny != NULL)
            for (size_t i = 0; i < deny_len; i++)
                ss_deny_append((uint64_t)(uintptr_t)deny[i].base,
                               (uint64_t)deny[i].len);
        /* T2: arm the shared wall-clock watchdog on the first frame that wants it. */
        if (watchdog_ms != UINT32_MAX) {
            if (g_ww_refcount == 0)
                ss_window_watchdog_arm(watchdog_ms);
            g_ww_refcount++;
        }
    }
    pthread_mutex_unlock(&g_ss_lock);

    /* Publish the frame (bump depth) BEFORE arming TF: the handler reads tls_depth. */
    tls_depth = idx + 1;
    g_armed = 1; /* belt */
    ss_arm_tf(); /* arm THIS thread; from here it single-steps */
    return ASMTEST_HW_OK;
}

/* Region-keyed / handle-keyed begin: a bounded [base,len) frame (register form).
 * Region frames pass the guards DISABLED (the whole-window guards apply only to the
 * region-free path). */
int asmtest_ss_begin_ex(const void *base, size_t len, asmtest_trace_t *trace,
                        uint32_t *out_idx, uint32_t *out_gen) {
    return ss_push_frame(base, len, trace, 0, UINT64_MAX, UINT32_MAX, 0, NULL,
                         0, out_idx, out_gen);
}

/* Compat wrapper for the name-keyed single-region path. */
int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace) {
    return asmtest_ss_begin_ex(base, len, trace, NULL, NULL);
}

/* §Z1: region-free whole-window begin. Pushes a frame with no [base,len); the
 * handler records absolute RIPs for every instruction this thread runs in the window
 * (native-leaf only — pointing single-step at live managed code is forbidden, see
 * the scoped-tracing plans). Same handle + EFULL/EINVAL convention as _begin_ex.
 *
 * The guard config (zeroconfig-scoped-tracing-hardening T1/T2) rides along: insn_budget
 * (0 => 4x-ring default, UINT64_MAX => off), watchdog_ms (0 => 10 s default,
 * UINT32_MAX => off), use_default_denylist (opt-in blocking-libc set), and deny/deny_len
 * (extra regions, copied here). The hwtrace-layer begin_window_ex owns validation and
 * the NULL-config defaults; this layer owns the defaults resolution and arming. */
int asmtest_ss_begin_window(asmtest_trace_t *trace, uint64_t insn_budget,
                            uint32_t watchdog_ms, int use_default_denylist,
                            const asmtest_hwtrace_deny_t *deny, size_t deny_len,
                            uint32_t *out_idx, uint32_t *out_gen) {
    return ss_push_frame(NULL, 0, trace, 1, insn_budget, watchdog_ms,
                         use_default_denylist, deny, deny_len, out_idx,
                         out_gen);
}

/* Post-pass: replay a frame's captured ordered offsets into its trace, deriving the
 * block partition from fall-through discontinuities (the same single-entry/ends-at-
 * branch model the other backends use). Runs in normal context, so Capstone is safe. */
static void ss_normalize(ss_frame_t *fr) {
    asmtest_trace_t *t = fr->trace;
    if (t == NULL)
        return;

    /* §Z1 whole-window: the stream holds ABSOLUTE RIPs and there is no [base,len)
     * to disassemble against here (the bytes are decoded later — from live self
     * memory by asmtest_hwtrace_render_window, or a versioned code-image for moving
     * managed code). So the post-pass only replays the absolute addresses into the
     * trace (collapsing REP self-repeats) and propagates overflow as `truncated`; no
     * per-offset disas and no block partition (attributed at render, not here). */
    if (fr->whole_window) {
        uint64_t prev = 0;
        int have_prev = 0;
        for (uint32_t i = 0; i < fr->stream_len; i++) {
            uint64_t addr = fr->stream[i];
            if (have_prev && addr == prev)
                continue; /* collapse a TF self-repeat (REP string insn) */
            trace_append_insn(t, addr); /* insns[] carries absolute addresses */
            prev = addr;
            have_prev = 1;
        }
        if (fr->overflow || fr->guard != SS_GUARD_NONE)
            t->truncated =
                true; /* ring overflow OR any guard fired (deny/budget/watchdog) */
        return;
    }

    const uint8_t *g_base = fr->base;
    uint64_t g_base_ip = fr->base_ip;
    size_t g_len = fr->len;
    uint32_t n = fr->stream_len;
    int have_prev = 0;
    int prev_was_branch = 0;    /* previous recorded insn was a CTI */
    uint64_t expected_next = 0; /* fall-through offset of the previous insn */
    uint64_t prev_off = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = fr->stream[i];

        /* Collapse consecutive identical offsets: a REP-prefixed string insn traps
         * after every iteration under TF, recording the same offset repeatedly. */
        if (have_prev && off == prev_off)
            continue;

        /* Block boundary (matches the PT/DR/Unicorn partition): region entry, a
         * non-fall-through target, OR the fall-through right after a branch-class
         * instruction (a NOT-taken conditional branch). */
        if (!have_prev || off != expected_next || prev_was_branch)
            trace_append_block(t, off);

        trace_append_insn(t, off);

        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, g_base, g_len, g_base_ip,
                                 off, NULL, 0);
        if (l == 0) {
            t->truncated = true;
            return;
        }
        prev_was_branch =
            asmtest_disas_is_branch(ASMTEST_ARCH_X86_64, g_base, g_len, off);
        expected_next = off + l;
        prev_off = off;
        have_prev = 1;
    }

    /* If the last recorded instruction is a non-branch whose fall-through is still
     * strictly inside the region, stepping stopped early (e.g. the routine cleared
     * TF) — flag the partial trace rather than present a prefix as complete. */
    if (have_prev && !prev_was_branch && expected_next < g_len)
        t->truncated = true;

    if (fr->overflow)
        t->truncated = true; /* in-region run exceeded the capture buffer */
}

/* Pop the calling thread's TOP frame (LIFO), normalize it, and (on the OUTERMOST pop)
 * disarm this thread's TF + drop the process-wide arm-refcount, restoring the caller's
 * original SIGTRAP disposition on the 1->0 transition. A no-op when this thread has no
 * frame (e.g. a cross-thread close): TLS makes the arming thread's stack invisible
 * here, so the frame stays and the caller's tid-assert backstop flags the misuse. */
void asmtest_ss_end(void) {
    int d = tls_depth;
    if (d <= 0)
        return; /* nothing on this thread's stack */
    ss_frame_t *fr = &tls_frames[d - 1];
    if (d == 1) {
        /* Outermost: drop depth to 0 FIRST so the handler early-returns (never
         * re-asserts TF) during the disarm's own trailing trap, then clear TF. */
        tls_depth = 0;
        ss_disarm_tf();
    } else {
        tls_depth = d - 1; /* nested pop: outer frames keep stepping */
    }
    ss_normalize(fr);
    ss_free_stream(
        fr); /* munmap (whole-window) or free (region); keeps base/len/gen */

    pthread_mutex_lock(&g_ss_lock);
    /* T2: if this frame armed the watchdog, drop the refcount; disarm on the last one
     * (restores the caller's SIGALRM disposition + itimer exactly). Done before the
     * SIGTRAP refcount so the guard state unwinds in reverse-arm order. */
    if (fr->whole_window && fr->watchdog_ms != UINT32_MAX) {
        if (g_ww_refcount > 0)
            g_ww_refcount--;
        if (g_ww_refcount == 0)
            ss_window_watchdog_disarm();
    }
    if (g_arm_refcount > 0)
        g_arm_refcount--;
    if (g_arm_refcount == 0) {
        if (g_installed) {
            sigaction(SIGTRAP, &g_old_sa, NULL);
            g_installed = 0;
        }
        g_armed = 0;
        g_deny_len = 0; /* T1: clear the active deny table on the last close */
    }
    pthread_mutex_unlock(&g_ss_lock);
}

/* Call fn(args[0..n)) through the SysV integer/pointer ABI. Register-resident arities
 * only (0-6): the args map 1:1 onto rdi,rsi,rdx,rcx,r8,r9, so no stack spill is
 * needed. This runs BETWEEN arm and disarm but OUTSIDE the frame's [base,len), so the
 * handler filters every instruction here (and any reverse-P/Invoke thunk fn jumps
 * through) out of the trace — only fn's in-region body is recorded. */
static long ss_dispatch_call(void *fn, const long *a, int n) {
    switch (n) {
    case 0:
        return ((long (*)(void))fn)();
    case 1:
        return ((long (*)(long))fn)(a[0]);
    case 2:
        return ((long (*)(long, long))fn)(a[0], a[1]);
    case 3:
        return ((long (*)(long, long, long))fn)(a[0], a[1], a[2]);
    case 4:
        return ((long (*)(long, long, long, long))fn)(a[0], a[1], a[2], a[3]);
    case 5:
        return ((long (*)(long, long, long, long, long))fn)(a[0], a[1], a[2],
                                                            a[3], a[4]);
    default:
        return ((long (*)(long, long, long, long, long, long))fn)(
            a[0], a[1], a[2], a[3], a[4], a[5]);
    }
}

/* B (managed-safe lazy-arm) — managed-singlestep-lazy-arm-plan §B1. Push a [base,len)
 * region frame (arms this thread's TF), dispatch fn(args…) in native code, then
 * pop+normalize (disarms). Because the handler filters to [base,len), ONLY fn's body
 * is recorded and NOTHING the caller runs between arm and disarm is stepped — the whole
 * point for a managed caller, where stepping the runtime's machinery (a pthread_create
 * that blocks SIGTRAP inside the window) is fatal, not degraded. Integer/pointer args
 * only, 0-6 (register-resident); nargs<0/>6 or a NULL fn return EINVAL so the caller
 * can fall back to the out-of-process stepper. *result_out (may be NULL) gets fn's
 * return value; the frame handle rides out_idx/out_gen (either may be NULL) so a
 * render-on-close (asmtest_ss_frame_lookup) can read the normalized trace. */
int asmtest_ss_call_scoped(const void *base, size_t len, asmtest_trace_t *trace,
                           void *fn, const long *args, int nargs,
                           long *result_out, uint32_t *out_idx,
                           uint32_t *out_gen) {
    if (fn == NULL || nargs < 0 || nargs > 6 || (nargs > 0 && args == NULL))
        return ASMTEST_HW_EINVAL;
    int rc = ss_push_frame(base, len, trace, 0, UINT64_MAX, UINT32_MAX, 0, NULL,
                           0, out_idx, out_gen);
    if (rc != ASMTEST_HW_OK)
        return rc;
    long r = ss_dispatch_call(fn, args, nargs);
    asmtest_ss_end();
    if (result_out != NULL)
        *result_out = r;
    return ASMTEST_HW_OK;
}

/* Floating-point sibling of ss_dispatch_call: call fn(args…) through the SysV FP ABI,
 * a homogeneous (double…)->double signature. Register-resident arities only (0-8):
 * the eight FP arg registers xmm0..xmm7 are exactly the cast's slots, no stack spill.
 * Same region-filter guarantee — every instruction here is outside [base,len). */
static double ss_dispatch_call_fp(void *fn, const double *a, int n) {
    switch (n) {
    case 0:
        return ((double (*)(void))fn)();
    case 1:
        return ((double (*)(double))fn)(a[0]);
    case 2:
        return ((double (*)(double, double))fn)(a[0], a[1]);
    case 3:
        return ((double (*)(double, double, double))fn)(a[0], a[1], a[2]);
    case 4:
        return ((double (*)(double, double, double, double))fn)(a[0], a[1],
                                                                a[2], a[3]);
    case 5:
        return ((double (*)(double, double, double, double, double))fn)(
            a[0], a[1], a[2], a[3], a[4]);
    case 6:
        return ((double (*)(double, double, double, double, double, double))fn)(
            a[0], a[1], a[2], a[3], a[4], a[5]);
    case 7:
        return ((double (*)(double, double, double, double, double, double,
                            double))fn)(a[0], a[1], a[2], a[3], a[4], a[5],
                                        a[6]);
    default:
        return ((double (*)(double, double, double, double, double, double,
                            double, double))fn)(a[0], a[1], a[2], a[3], a[4],
                                                a[5], a[6], a[7]);
    }
}

/* FP variant of asmtest_ss_call_scoped (managed-singlestep-lazy-arm-plan §B2, the
 * (double…)->double shim family): arm [base,len), call the FP-ABI fn, disarm. Same
 * managed-safe guarantee — only fn's body is stepped. Homogeneous double args only,
 * 0-8; a NULL fn / nargs outside 0-8 returns EINVAL so the caller falls back
 * out-of-process. *result_out (may be NULL) gets fn's xmm0 return. */
int asmtest_ss_call_scoped_fp(const void *base, size_t len,
                              asmtest_trace_t *trace, void *fn,
                              const double *args, int nargs, double *result_out,
                              uint32_t *out_idx, uint32_t *out_gen) {
    if (fn == NULL || nargs < 0 || nargs > 8 || (nargs > 0 && args == NULL))
        return ASMTEST_HW_EINVAL;
    int rc = ss_push_frame(base, len, trace, 0, UINT64_MAX, UINT32_MAX, 0, NULL,
                           0, out_idx, out_gen);
    if (rc != ASMTEST_HW_OK)
        return rc;
    double r = ss_dispatch_call_fp(fn, args, nargs);
    asmtest_ss_end();
    if (result_out != NULL)
        *result_out = r;
    return ASMTEST_HW_OK;
}

/* §Z4: the OS tid this backend stamps frames with, for the CALLING thread. The handle
 * carries this value (see asmtest_ss_frame_lookup), and a handle's tid is compared
 * against a frame's arm_tid — so both MUST come from this one clock. Callers that mint
 * a handle read it here rather than calling gettid themselves: two independent notions
 * of "my tid" that ever disagreed would fail every lookup, turning every close into a
 * silent false-truncate. Returns the same value as the SS_ARM_TID() stamp in
 * ss_push_frame, by construction. */
int asmtest_ss_self_tid(void) { return SS_ARM_TID(); }

/* Resolve a frame handle (idx+gen+arm_tid) on the CALLING thread to its region +
 * trace. The frame data survives a pop (only stream is freed), so a render-on-close can
 * read the normalized trace. Returns 1 + fills the out params on a live match, 0 on a
 * stale/unknown/FOREIGN handle.
 *
 * §Z4: {idx,gen} names a frame only within ONE thread — tls_frames and tls_gen_ctr are
 * both __thread, so every thread's first frame is {0,1} and handles collide ACROSS
 * threads. Resolving another thread's handle here would index the CALLER's table and
 * match the caller's own live frame on gen alone: the wrong frame, reported as a good
 * one. So the handle carries the tid that armed it and it must equal this frame's
 * stamp. This lives here, not in the callers, because arm_tid is private to
 * ss_frame_t: the layer that owns the invariant is the layer that enforces it, once,
 * for every caller — and "is this handle mine?" is part of resolving it, not a policy
 * each caller re-derives. What to DO with a miss stays the caller's business. */
int asmtest_ss_frame_lookup(uint32_t idx, uint32_t gen, int arm_tid,
                            const void **base, size_t *len,
                            asmtest_trace_t **trace) {
    /* Unsigned compare: a signed cast would let the 0xffffffff sentinel handle
     * (begin_scope's failure marker) pass the bound and index out of range. */
    if (idx >= (uint32_t)SS_MAX_FRAMES)
        return 0;
    ss_frame_t *f = &tls_frames[idx];
    if (f->gen != gen || f->gen == 0)
        return 0;
    /* Foreign handle: armed by another thread, so it names a frame in ANOTHER TLS
     * table that is invisible (and uncloseable) from here. No syscall — f->arm_tid is
     * this thread's own stamp already, so this compares the handle's origin against
     * the caller's identity for free. A thread that exits and has its tid recycled
     * cannot alias: the new thread's TLS starts zeroed, so gen==0 rejects first. */
    if (f->arm_tid != arm_tid)
        return 0;
    if (base != NULL)
        *base = f->base;
    if (len != NULL)
        *len = f->len;
    if (trace != NULL)
        *trace = f->trace;
    return 1;
}

/* T3: resolve a whole-window handle to the guard code that stopped it (SS_GUARD_*).
 * Same {idx,gen,arm_tid} discipline as asmtest_ss_frame_lookup (so it is render-on-close
 * safe on the ARMING thread after end_window — the `guard` scalar survives the pop),
 * returning the guard on a live match or -1 on a stale/foreign/unknown handle. */
int asmtest_ss_frame_guard(uint32_t idx, uint32_t gen, int arm_tid) {
    if (idx >= (uint32_t)SS_MAX_FRAMES)
        return -1;
    ss_frame_t *f = &tls_frames[idx];
    if (f->gen != gen || f->gen == 0)
        return -1;
    if (f->arm_tid != arm_tid)
        return -1;
    return (int)f->guard;
}

#else /* not x86-64 Linux/macOS — link-compatible stubs */

int asmtest_ss_begin_ex(const void *base, size_t len, asmtest_trace_t *trace,
                        uint32_t *out_idx, uint32_t *out_gen) {
    (void)base;
    (void)len;
    (void)trace;
    (void)out_idx;
    (void)out_gen;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace) {
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_ss_begin_window(asmtest_trace_t *trace, uint64_t insn_budget,
                            uint32_t watchdog_ms, int use_default_denylist,
                            const asmtest_hwtrace_deny_t *deny, size_t deny_len,
                            uint32_t *out_idx, uint32_t *out_gen) {
    (void)trace;
    (void)insn_budget;
    (void)watchdog_ms;
    (void)use_default_denylist;
    (void)deny;
    (void)deny_len;
    (void)out_idx;
    (void)out_gen;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_ss_call_scoped(const void *base, size_t len, asmtest_trace_t *trace,
                           void *fn, const long *args, int nargs,
                           long *result_out, uint32_t *out_idx,
                           uint32_t *out_gen) {
    (void)base;
    (void)len;
    (void)trace;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    (void)out_idx;
    (void)out_gen;
    return ASMTEST_HW_ENOSYS;
}
int asmtest_ss_call_scoped_fp(const void *base, size_t len,
                              asmtest_trace_t *trace, void *fn,
                              const double *args, int nargs, double *result_out,
                              uint32_t *out_idx, uint32_t *out_gen) {
    (void)base;
    (void)len;
    (void)trace;
    (void)fn;
    (void)args;
    (void)nargs;
    (void)result_out;
    (void)out_idx;
    (void)out_gen;
    return ASMTEST_HW_ENOSYS;
}
void asmtest_ss_end(void) {}
int asmtest_ss_self_tid(void) {
    return -1; /* no frames to stamp on this arch; matches the idle/sentinel tid */
}
int asmtest_ss_frame_lookup(uint32_t idx, uint32_t gen, int arm_tid,
                            const void **base, size_t *len,
                            asmtest_trace_t **trace) {
    (void)idx;
    (void)gen;
    (void)arm_tid;
    (void)base;
    (void)len;
    (void)trace;
    return 0;
}
int asmtest_ss_frame_guard(uint32_t idx, uint32_t gen, int arm_tid) {
    (void)idx;
    (void)gen;
    (void)arm_tid;
    return -1;
}

#endif
