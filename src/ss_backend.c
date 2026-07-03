/*
 * ss_backend.c — single-step (EFLAGS.TF / #DB -> SIGTRAP) native-trace backend.
 * See docs/plans/zen2-singlestep-trace-plan.md, asmtest_hwtrace.h,
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
 * Signal-safety: the SIGTRAP handler does ONLY async-signal-safe work — a
 * bounds-checked store of each in-region RIP offset into a preallocated buffer,
 * and re-asserting TF in the saved context. All Capstone-based block
 * normalization (which allocates) runs in a post-pass in asmtest_ss_end(), in
 * normal context — never from the handler.
 */
/* Must precede every include so glibc exposes REG_RIP/REG_EFL in <ucontext.h>. */
#define _GNU_SOURCE

#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#define ASMTEST_HW_OK     0
#define ASMTEST_HW_EINVAL (-1)
#define ASMTEST_HW_ENOSYS (-5)

#if defined(__linux__) && defined(__x86_64__)

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#define SS_TF 0x100ULL /* EFLAGS.TF, bit 8 */

/* Capacity of the internal ordered-RIP buffer. The handler stores every executed
 * in-region instruction offset here (async-signal-safe array write); the post-pass
 * replays it to fill the trace and derive blocks. Sized for the small-routine
 * envelope; a routine that executes more in-region instructions than this overflows
 * and is honestly flagged truncated (never emit partial as complete). */
#ifndef SS_STREAM_CAP
#define SS_STREAM_CAP (1u << 16) /* 65536 offsets = 512 KiB */
#endif

/* Active-stepper state (single region, single thread — the hwtrace MVP contract).
 * `volatile`/sig_atomic_t members are touched from the SIGTRAP handler. */
static volatile sig_atomic_t g_armed;
static const uint8_t *g_base;
static uint64_t g_base_ip;
static size_t g_len;
static asmtest_trace_t *g_trace;
static uint64_t *g_stream; /* ordered in-region RIP offsets             */
static volatile uint32_t g_stream_len;
static volatile sig_atomic_t
    g_overflow; /* stream filled, entries dropped       */
static struct sigaction g_old_sa;
static int g_installed;

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

/* The trap handler: record the in-region offset, re-assert TF, return. No Capstone,
 * no malloc, no block logic — all of that is deferred to the post-pass. */
static void ss_on_sigtrap(int sig, siginfo_t *si, void *uctx) {
    (void)sig;
    (void)si;
    ucontext_t *uc = (ucontext_t *)uctx;
    if (!g_armed)
        return; /* not our stepping window */
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];

    /* Record conditionally: only instructions inside the registered region. Steps
     * through callees and the begin/end glue execute but are not recorded, so the
     * trace stays clean (mirrors the DynamoRIO/AMD in-region gating). */
    if (rip >= g_base_ip && rip < g_base_ip + g_len) {
        if (g_stream_len < SS_STREAM_CAP)
            g_stream[g_stream_len++] = rip - g_base_ip;
        else
            g_overflow = 1;
    }

    /* Re-assert TF so sigreturn resumes stepping (in-region OR in a callee). */
    uc->uc_mcontext.gregs[REG_EFL] |= (greg_t)SS_TF;
}

int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace) {
    if (base == NULL || len == 0)
        return ASMTEST_HW_EINVAL;

    g_base = (const uint8_t *)base;
    g_base_ip = (uint64_t)(uintptr_t)base;
    g_len = len;
    g_trace = trace;
    g_stream_len = 0;
    g_overflow = 0;
    g_stream = (uint64_t *)malloc((size_t)SS_STREAM_CAP * sizeof(uint64_t));
    if (g_stream == NULL)
        return ASMTEST_HW_EINVAL;

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ss_on_sigtrap;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGTRAP, &sa, &g_old_sa) != 0) {
        free(g_stream);
        g_stream = NULL;
        return ASMTEST_HW_EINVAL;
    }
    g_installed = 1;

    /* Arm LAST: every instruction executed after this — the rest of this function's
     * epilogue, the begin/end glue, and the registered routine — single-steps. The
     * in-region filter keeps the trace to the routine itself. */
    g_armed = 1;
    ss_arm_tf();
    return ASMTEST_HW_OK;
}

/* Post-pass: replay the captured ordered offsets into the trace, deriving the block
 * partition from fall-through discontinuities (the same single-entry/ends-at-branch
 * model the other backends use). Runs in normal context, so Capstone is safe. */
static void ss_normalize(void) {
    asmtest_trace_t *t = g_trace;
    if (t == NULL)
        return;
    uint32_t n = g_stream_len;
    int have_prev = 0;
    int prev_was_branch =
        0; /* previous recorded insn was a CTI (jump/call/ret) */
    uint64_t expected_next = 0; /* fall-through offset of the previous insn */
    uint64_t prev_off = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = g_stream[i];

        /* Collapse consecutive identical offsets: a REP-prefixed string insn traps
         * after every iteration under TF, recording the same offset repeatedly,
         * whereas PT/DR record it once. Skip the duplicate (no insn, no block). */
        if (have_prev && off == prev_off)
            continue;

        /* Block boundary, matching the PT/DR/Unicorn partition (a block ends after
         * every branch-class instruction): region entry, a non-fall-through target
         * (taken branch / callee re-entry), OR the fall-through immediately after a
         * branch-class instruction — i.e. a NOT-taken conditional branch, which
         * fall-through-discontinuity alone would miss. */
        if (!have_prev || off != expected_next || prev_was_branch)
            trace_append_block(t, off);

        trace_append_insn(t, off);

        /* Fall-through of THIS instruction = off + its length (Capstone). A zero
         * length means undecodable (self-modifying / relocated bytes): flag the
         * loss and stop — never trust the rest of the partition. */
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

    /* A well-formed routine leaves the region via a control transfer (its `ret`,
     * or a jump out), so the last recorded in-region instruction is a branch (or
     * its fall-through reaches the region end). If instead the last instruction is
     * a non-branch whose fall-through is still strictly inside the region, stepping
     * stopped early — e.g. the routine cleared EFLAGS.TF (popfq/iret of a flags
     * image with TF=0), suppressing further #DB traps — so the tail ran unrecorded.
     * Flag the partial trace rather than present a prefix as complete. */
    if (have_prev && !prev_was_branch && expected_next < g_len)
        t->truncated = true;

    if (g_overflow)
        t->truncated = true; /* in-region run exceeded the capture buffer */
}

void asmtest_ss_end(void) {
    /* Disarm FIRST: clear TF so stepping stops, then this function and the caller
     * run at full speed. (popfq that clears TF suppresses its own trailing trap.) */
    g_armed = 0;
    ss_disarm_tf();

    if (g_installed) {
        sigaction(SIGTRAP, &g_old_sa, NULL);
        g_installed = 0;
    }
    ss_normalize();

    free(g_stream);
    g_stream = NULL;
    g_trace = NULL;
    g_base = NULL;
    g_len = 0;
}

#else /* not Linux x86-64 — link-compatible stubs */

int asmtest_ss_begin(const void *base, size_t len, asmtest_trace_t *trace) {
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}
void asmtest_ss_end(void) {}

#endif
