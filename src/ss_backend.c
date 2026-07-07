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

#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#define ASMTEST_HW_OK     0
#define ASMTEST_HW_EINVAL (-1)
#define ASMTEST_HW_EFULL  (-6)
#define ASMTEST_HW_ENOSYS (-5)

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__))

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> /* mmap/munmap (§Z1 sparse whole-window buffer) */
#include <unistd.h>   /* syscall (§Z4 arm-tid) */
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
} ss_frame_t;

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
             * writing an uncommitted page here takes a transparent minor fault. */
            if (f->stream_len < f->cap)
                f->stream[f->stream_len++] = rip;
            else
                f->overflow = 1;
        } else if (rip >= f->base_ip && rip < f->base_ip + f->len) {
            if (f->stream_len < f->cap)
                f->stream[f->stream_len++] = rip - f->base_ip;
            else
                f->overflow = 1;
        }
    }

    /* Re-assert TF so sigreturn resumes stepping (in-region OR in a callee). */
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
                         int whole_window, uint32_t *out_idx,
                         uint32_t *out_gen) {
    if (whole_window) {
        if (base != NULL || len != 0)
            return ASMTEST_HW_EINVAL; /* region-free frame carries no [base,len) */
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
    if (out_idx != NULL)
        *out_idx = (uint32_t)idx;
    if (out_gen != NULL)
        *out_gen = f->gen;

    /* Process-wide SIGTRAP install on the 0->1 arm-refcount transition. Save the
     * caller's original disposition into g_old_sa ONLY then, so a second concurrent
     * begin cannot overwrite it with asm-test's own just-installed handler. */
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
    pthread_mutex_unlock(&g_ss_lock);

    /* Publish the frame (bump depth) BEFORE arming TF: the handler reads tls_depth. */
    tls_depth = idx + 1;
    g_armed = 1; /* belt */
    ss_arm_tf(); /* arm THIS thread; from here it single-steps */
    return ASMTEST_HW_OK;
}

/* Region-keyed / handle-keyed begin: a bounded [base,len) frame (register form). */
int asmtest_ss_begin_ex(const void *base, size_t len, asmtest_trace_t *trace,
                        uint32_t *out_idx, uint32_t *out_gen) {
    return ss_push_frame(base, len, trace, 0, out_idx, out_gen);
}

/* Compat wrapper for the name-keyed single-region path. */
int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace) {
    return asmtest_ss_begin_ex(base, len, trace, NULL, NULL);
}

/* §Z1: region-free whole-window begin. Pushes a frame with no [base,len); the
 * handler records absolute RIPs for every instruction this thread runs in the window
 * (native-leaf only — pointing single-step at live managed code is forbidden, see
 * the scoped-tracing plans). Same handle + EFULL/EINVAL convention as _begin_ex. */
int asmtest_ss_begin_window(asmtest_trace_t *trace, uint32_t *out_idx,
                            uint32_t *out_gen) {
    return ss_push_frame(NULL, 0, trace, 1, out_idx, out_gen);
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
        if (fr->overflow)
            t->truncated =
                true; /* whole-window run exceeded the capture buffer */
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
    if (g_arm_refcount > 0)
        g_arm_refcount--;
    if (g_arm_refcount == 0) {
        if (g_installed) {
            sigaction(SIGTRAP, &g_old_sa, NULL);
            g_installed = 0;
        }
        g_armed = 0;
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
    int rc = ss_push_frame(base, len, trace, 0, out_idx, out_gen);
    if (rc != ASMTEST_HW_OK)
        return rc;
    long r = ss_dispatch_call(fn, args, nargs);
    asmtest_ss_end();
    if (result_out != NULL)
        *result_out = r;
    return ASMTEST_HW_OK;
}

/* Resolve a frame handle (idx+gen) on the CALLING thread to its region + trace. The
 * frame data survives a pop (only stream is freed), so a render-on-close can read the
 * normalized trace. Returns 1 + fills the out params on a live match, 0 on a
 * stale/unknown handle. */
int asmtest_ss_frame_lookup(uint32_t idx, uint32_t gen, const void **base,
                            size_t *len, asmtest_trace_t **trace) {
    /* Unsigned compare: a signed cast would let the 0xffffffff sentinel handle
     * (begin_scope's failure marker) pass the bound and index out of range. */
    if (idx >= (uint32_t)SS_MAX_FRAMES)
        return 0;
    ss_frame_t *f = &tls_frames[idx];
    if (f->gen != gen || f->gen == 0)
        return 0;
    if (base != NULL)
        *base = f->base;
    if (len != NULL)
        *len = f->len;
    if (trace != NULL)
        *trace = f->trace;
    return 1;
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
int asmtest_ss_begin_window(asmtest_trace_t *trace, uint32_t *out_idx,
                            uint32_t *out_gen) {
    (void)trace;
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
void asmtest_ss_end(void) {}
int asmtest_ss_frame_lookup(uint32_t idx, uint32_t gen, const void **base,
                            size_t *len, asmtest_trace_t **trace) {
    (void)idx;
    (void)gen;
    (void)base;
    (void)len;
    (void)trace;
    return 0;
}

#endif
