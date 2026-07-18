/*
 * trace_auto.c — the cross-tier trace orchestrator (asmtest_trace_auto.h).
 *
 * Walks the full descending-fidelity cascade across all three trace tiers and
 * returns the available options. It calls asmtest_hwtrace_available() directly (it
 * ships in the same libasmtest_hwtrace) and dlopen-probes libasmtest_drapp for the
 * DynamoRIO tier, so it hard-links neither the DynamoRIO nor the emulator library.
 *
 * The order is fixed by docs/internal/analysis/trace-parity-matrix.md, Matrix 8:
 *
 *   Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight -> emulator
 *
 * Note DynamoRIO ranks ABOVE single-step: both are exact and unbounded, but the
 * DynamoRIO code cache runs at native speed while single-step pays a kernel
 * round-trip per instruction (~2.3us/insn). The four hardware backends therefore
 * straddle the DynamoRIO tier rather than forming one contiguous block, which is
 * why this resolver cannot simply concatenate asmtest_hwtrace_resolve()'s output
 * with the other tiers — it interleaves them by overhead.
 */
#include "asmtest_trace_auto.h"

#include "asmtest_blockstep_internal.h" /* T8: IBS covered-block pre-cover table */
#include "asmtest_ibs.h" /* T8: statistical survey for pre-cover  */
#include "asmtest_ptrace.h" /* block-step / per-insn call-owning escalation tiers */

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) && defined(__x86_64__)
#include <dlfcn.h>

/* The DynamoRIO tier is a separate, DYNAMORIO_HOME-gated library the hardware tier
 * deliberately does not depend on. Probe it the way every language binding does:
 * dlopen libasmtest_drapp (via $ASMTEST_DRAPP_LIB, else the default soname) and ask
 * its asmtest_dr_available(), which reports whether libdynamorio is loadable WITHOUT
 * initializing DR — so this probe has no side effects. drapp itself dlopens
 * libdynamorio lazily (only in asmtest_dr_init), so it loads even when DR is absent.
 */
static int dynamorio_tier_available(void) {
    const char *path = getenv("ASMTEST_DRAPP_LIB");
    void *h = dlopen((path && *path) ? path : "libasmtest_drapp.so",
                     RTLD_LAZY | RTLD_LOCAL);
    if (h == NULL)
        return 0;
    int (*avail)(void) = (int (*)(void))dlsym(h, "asmtest_dr_available");
    int r = (avail != NULL) ? avail() : 0;
    dlclose(h);
    return r;
}
#else
static int dynamorio_tier_available(void) { return 0; }
#endif

/* One row of the cross-tier cascade, in descending fidelity/preference order.
 * `mechanism` is the F22/F26/F37 discriminator: the concrete capture mechanism
 * this row's begin/end path drives (every row here is an EXACT producer — the
 * STATISTICAL mechanism/fidelity is reserved for the IBS/sampling survey lane
 * and never appears in this cascade). */
typedef struct {
    asmtest_trace_tier_t tier;
    asmtest_trace_backend_t backend; /* only meaningful for the HWTRACE tier */
    asmtest_trace_fidelity_t fidelity;
    asmtest_trace_mechanism_t mechanism;
} cascade_row_t;

static const cascade_row_t CASCADE[] = {
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_INTEL_PT, ASMTEST_FIDELITY_NATIVE,
     ASMTEST_TRACE_MECH_HW_BRANCH},
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_AMD_LBR, ASMTEST_FIDELITY_NATIVE,
     ASMTEST_TRACE_MECH_HW_BRANCH},
    {ASMTEST_TIER_DYNAMORIO, 0, ASMTEST_FIDELITY_NATIVE,
     ASMTEST_TRACE_MECH_DBI},
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_SINGLESTEP, ASMTEST_FIDELITY_NATIVE,
     ASMTEST_TRACE_MECH_TF_STEP},
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_CORESIGHT, ASMTEST_FIDELITY_NATIVE,
     ASMTEST_TRACE_MECH_HW_BRANCH},
    {ASMTEST_TIER_EMULATOR, 0, ASMTEST_FIDELITY_VIRTUAL,
     ASMTEST_TRACE_MECH_EMULATOR},
};

/* Is this cascade row available on this host, after applying the policy filters? */
static int row_available(const cascade_row_t *row, unsigned policy) {
    switch (row->tier) {
    case ASMTEST_TIER_HWTRACE:
        /* CEILING_FREE drops the one fixed-window backend (AMD LBR). */
        if ((policy & ASMTEST_TRACE_CEILING_FREE) &&
            row->backend == ASMTEST_HWTRACE_AMD_LBR)
            return 0;
        return asmtest_hwtrace_available(row->backend);
    case ASMTEST_TIER_DYNAMORIO:
        return dynamorio_tier_available();
    case ASMTEST_TIER_EMULATOR:
        /* The universal floor — always available — unless NATIVE_ONLY forbids the
         * native->emulator fidelity crossing. */
        return (policy & ASMTEST_TRACE_NATIVE_ONLY) ? 0 : 1;
    }
    return 0;
}

size_t asmtest_trace_resolve(unsigned policy, asmtest_trace_choice_t *out,
                             size_t cap) {
    if (out == NULL || cap == 0)
        return 0;
    size_t n = 0;
    for (size_t i = 0; i < sizeof CASCADE / sizeof CASCADE[0] && n < cap; i++) {
        if (!row_available(&CASCADE[i], policy))
            continue;
        out[n].tier = CASCADE[i].tier;
        out[n].backend = CASCADE[i].backend;
        out[n].fidelity = CASCADE[i].fidelity;
        out[n].mechanism = CASCADE[i].mechanism;
        n++;
    }
    return n;
}

int asmtest_trace_auto(unsigned policy, asmtest_trace_choice_t *out) {
    if (out == NULL)
        return ASMTEST_HW_EUNAVAIL;
    if (asmtest_trace_resolve(policy, out, 1) == 0)
        return ASMTEST_HW_EUNAVAIL;
    return ASMTEST_HW_OK;
}

/* Call code(args…) as a SysV routine with 0..6 integer args. One 6-arg dispatch covers
 * every arity: a callee taking fewer harmlessly ignores the extra register args — the
 * same one-call trick the hwtrace/ptrace call-owning entries use. */
static long call_auto_invoke(const void *code, const long *args, int nargs) {
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs && i < 6; i++)
        a[i] = args[i];
    long (*fn)(long, long, long, long, long, long) =
        (long (*)(long, long, long, long, long, long))(uintptr_t)code;
    return fn(a[0], a[1], a[2], a[3], a[4], a[5]);
}

/* Clear a caller-owned trace between escalation attempts (keeps the insns/blocks
 * buffers; zeroes the counts + the truncated bit so the winning tier stands alone).
 *
 * F24: a reset DISCARDS whatever the prior rung had captured, so `ran` (the "we hold a
 * committed trace" flag) must be cleared in lock-step at every reset site and re-set to 1
 * ONLY when a rung actually commits a usable trace. Otherwise a rung that resets and then
 * FAILS at runtime (fork/ptrace blocked by seccomp/ptrace_scope, ENOMEM) leaves the trace
 * empty while `ran` still reads 1 from an EARLIER rung whose partial this reset wiped — and
 * the function returns ASMTEST_HW_OK with insns_len==0 + truncated==true (a successful
 * empty trace). Skipped (unavailable) rungs never reset, so a rung-1 best-effort partial
 * survives to be returned when nothing downstream runs — the legitimate truncated-but-OK
 * case is preserved. */
static void call_auto_reset(asmtest_trace_t *t) {
    t->insns_len = 0;
    t->insns_total = 0;
    t->blocks_len = 0;
    t->blocks_total = 0;
    t->truncated = false;
}

/* Phase 8 MSR-rung plumbing: asmtest_amd_msr_trace invokes an out-of-line void(void*)
 * callback that IT calls (it owns the enable->run->freeze bracket), so the routine's long
 * result is recovered across that void boundary via this small closure. */
struct msr_call_closure {
    const void *code;
    const long *args;
    int nargs;
    long result;
};
static void msr_call_trampoline(void *p) {
    struct msr_call_closure *c = (struct msr_call_closure *)p;
    c->result = call_auto_invoke(c->code, c->args, c->nargs);
}

/* T8 (ASMTEST_TRACE_IBS_PRECOVER): milliseconds the warm-up child re-runs code(args…)
 * for while the parent surveys it with IBS-Op — bounded, so the bit can only ever add a
 * small, fixed amount of latency ahead of the block-step rung it primes. */
#define TRACE_AUTO_IBS_WARMUP_MS 30

static long ibs_precover_elapsed_ms(const struct timespec *t0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - t0->tv_sec) * 1000L +
           (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

/* Build an IBS pre-cover table for the upcoming block-step rung: fork an isolated
 * warm-up child that re-runs code(args…) in a loop for ~TRACE_AUTO_IBS_WARMUP_MS while
 * THIS process surveys it out-of-band via IBS-Op (asmtest_ibs_survey_process never
 * perturbs the child — no ptrace, no single-step, the exact reason IBS is safe to point
 * at a managed-runtime target), then normalizes the sampled edges into covered-block
 * leaders and builds the memoization table from them.
 *
 * HONESTY: like the MSR rung beside it, the warm-up child re-executes code(args…) an
 * unspecified number of times in a fork-isolated copy — non-idempotent side effects
 * repeat THERE, never in the parent, which runs the routine again itself via the normal
 * block-step call right after this returns.
 *
 * ANY failure along the way (fork, survey, empty coverage, OOM) returns NULL: the bit
 * may never make the cascade fail, or even run slower in a way that matters, where the
 * plain rung previously succeeded — the caller falls back to the uncached scan. */
static asmtest_bs_precover_t *build_ibs_precover(const void *code, size_t len,
                                                 const long *args, int nargs) {
    pid_t child = fork();
    if (child < 0)
        return NULL;
    if (child == 0) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        do {
            call_auto_invoke(code, args, nargs);
        } while (ibs_precover_elapsed_ms(&t0) < TRACE_AUTO_IBS_WARMUP_MS);
        _exit(0);
    }

    asmtest_ibs_survey_t sv;
    memset(&sv, 0, sizeof sv);
    int src =
        asmtest_ibs_survey_process(child, TRACE_AUTO_IBS_WARMUP_MS, NULL, &sv);

    /* Reap the warm-up child: it should already be exiting (its own budget matches
     * the survey window); give it a short grace period, then reclaim it unconditionally
     * so this rung can never leak a zombie or hang past a bounded worst case. */
    int reaped = 0;
    for (int i = 0; i < 50; i++) {
        pid_t r = waitpid(child, NULL, WNOHANG);
        if (r == child || r < 0) {
            reaped = 1;
            break;
        }
        struct timespec req = {0, 1000000L}; /* 1ms */
        nanosleep(&req, NULL);
    }
    if (!reaped) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
    }

    if (src != ASMTEST_IBS_OK) {
        asmtest_ibs_survey_free(&sv);
        return NULL;
    }

    asmtest_ibs_blocks_t blk;
    memset(&blk, 0, sizeof blk);
    int nrc =
        asmtest_ibs_normalize_blocks(&sv, (uint64_t)(uintptr_t)code, len, &blk);
    asmtest_ibs_survey_free(&sv);
    if (nrc != ASMTEST_IBS_OK || blk.n == 0) {
        asmtest_ibs_blocks_free(&blk);
        return NULL;
    }

    asmtest_bs_precover_t *p = asmtest_bs_precover_build(
        (const uint8_t *)code, len, (uint64_t)(uintptr_t)code, &blk);
    asmtest_ibs_blocks_free(&blk);
    return p;
}

int asmtest_trace_call_auto(const void *code, size_t len, const long *args,
                            int nargs, unsigned policy, long *result,
                            asmtest_trace_t *trace,
                            asmtest_trace_choice_t *used) {
    if (code == NULL || trace == NULL || len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return ASMTEST_HW_EINVAL;
    /* F22/F26/F37: *used is meaningful ONLY on ASMTEST_HW_OK. Start it (and
     * leave it, on the EUNAVAIL path) at mechanism == MECH_NONE so a caller
     * can never read a stale rung out of a failed cascade; each rung that
     * COMMITS a trace overwrites all four fields with its own identity. */
    if (used != NULL) {
        used->tier = ASMTEST_TIER_HWTRACE;
        used->backend = ASMTEST_HWTRACE_SINGLESTEP;
        used->fidelity = ASMTEST_FIDELITY_NATIVE;
        used->mechanism = ASMTEST_TRACE_MECH_NONE;
    }
    int ran = 0; /* did any tier produce a trace? */

    /* (1) Fast exact — the best HWTRACE backend, driven through the marker path with
     * THIS wrapper owning the in-process call. ASMTEST_TRACE_CEILING_FREE in `policy`
     * skips the ceiling-bounded backend (AMD LBR) up front. */
    asmtest_hwtrace_policy_t hp = (policy & ASMTEST_TRACE_CEILING_FREE)
                                      ? ASMTEST_HWTRACE_CEILING_FREE
                                      : ASMTEST_HWTRACE_BEST;
    int hb = asmtest_hwtrace_auto(hp);
    if (hb >= 0) {
        asmtest_hwtrace_options_t opts;
        memset(&opts, 0, sizeof opts);
        opts.struct_size = sizeof opts; /* F27 ABI size negotiation */
        opts.backend = (asmtest_trace_backend_t)hb;
        if (asmtest_hwtrace_init(&opts) == ASMTEST_HW_OK) {
            if (asmtest_hwtrace_register_region("trace_call_auto",
                                                (void *)(uintptr_t)code, len,
                                                trace) == ASMTEST_HW_OK) {
                asmtest_hwtrace_begin("trace_call_auto");
                long r = call_auto_invoke(code, args, nargs);
                asmtest_hwtrace_end("trace_call_auto");
                if (result != NULL)
                    *result = r;
                ran = 1;
                if (used != NULL) {
                    used->tier = ASMTEST_TIER_HWTRACE;
                    used->backend = (asmtest_trace_backend_t)hb;
                    used->fidelity = ASMTEST_FIDELITY_NATIVE;
                    /* The one backend whose capture is a CPU step exception
                     * rather than a hardware branch record. */
                    used->mechanism = (hb == ASMTEST_HWTRACE_SINGLESTEP)
                                          ? ASMTEST_TRACE_MECH_TF_STEP
                                          : ASMTEST_TRACE_MECH_HW_BRANCH;
                }
            }
            asmtest_hwtrace_shutdown();
        }
    }
    if (ran && !trace->truncated)
        return ASMTEST_HW_OK; /* the fast tier captured the whole path */

    /* NO explicit BPF-snapshot rung here (considered and REJECTED, P6): the deterministic
     * boundary snapshot is already effectively rung 1 — hwtrace_begin_amd selects it by
     * DEFAULT for every 1..4-exit region (P5 multi-exit: one HW breakpoint per exit, up
     * to the 4 debug registers / ASMTEST_AMD_MAX_EXITS) under CAP_BPF + CAP_PERFMON,
     * with NO extra run of the routine. A re-run rung below would only duplicate a
     * capture rung 1 already completed, or re-attempt one it deterministically failed.
     * Correct escalation order: snapshot inside rung 1 (CAP_BPF+PERFMON, no second run)
     * -> MSR rung 1b (CAP_SYS_ADMIN, a SECOND in-process run). Both share the 16-branch
     * window ceiling, so CEILING_FREE excludes both in LOCK-STEP — keep rung 1's `hp`
     * policy pick and rung 1b's `!(policy & ASMTEST_TRACE_CEILING_FREE)` guard aligned. */

    /* (1b) MSR-direct escalation rung — inserted BEFORE the ~1000x block-step tier. When
     * the fast AMD tier came back truncated, its retired path may still fit one 16-deep
     * MSR read, reconstructed with ZERO PMU interrupts. This rung is the BACKSTOP for a
     * region with MORE exits than the 4 debug registers (> ASMTEST_AMD_MAX_EXITS, which
     * the default-on boundary snapshot leaves on the sampled path) or ANY multi-exit
     * region on a host without CAP_BPF; a 1..4-exit region is completed in rung 1 by the
     * multi-exit snapshot, so a truncated result reaching here means the snapshot could
     * not arm or the region overflows one 16-deep window.
     * asmtest_amd_msr_available() is a true privilege/device gate (root / CAP_SYS_ADMIN +
     * the `msr` module) that also returns 0 off x86-64 Linux, so this rung self-skips
     * cleanly to block-step wherever it is absent. It shares AMD_LBR's 16-deep window
     * ceiling, so CEILING_FREE excludes it in lock-step with the fast AMD_LBR tier.
     * HONESTY: like the fast begin/end tier it sits beside (and UNLIKE the fork-isolated
     * steppers below), asmtest_amd_msr_trace runs the routine IN-PROCESS — a second real
     * execution, so non-idempotent side effects happen again and a faulting routine takes
     * down the tracer. call_auto_reset() first so the truncated fast-tier trace cannot
     * contaminate the read; on ANY miss/failure set truncated and fall through (never
     * early-return on a failed tier) so a too-small-for-MSR routine still reaches block-step. */
    if (!(policy & ASMTEST_TRACE_CEILING_FREE) && asmtest_amd_msr_available()) {
        call_auto_reset(trace);
        ran =
            0; /* F24: the reset discarded any prior partial — re-earn `ran` below */
        struct msr_call_closure cl = {code, args, nargs, 0};
        if (asmtest_amd_msr_trace(code, len, msr_call_trampoline, &cl, trace) ==
                ASMTEST_HW_OK &&
            !trace->truncated) {
            if (result != NULL)
                *result = cl.result;
            ran = 1;
            if (used != NULL) {
                used->tier = ASMTEST_TIER_HWTRACE;
                used->backend = ASMTEST_HWTRACE_AMD_LBR;
                used->fidelity = ASMTEST_FIDELITY_NATIVE;
                used->mechanism = ASMTEST_TRACE_MECH_MSR_LBR;
            }
            return ASMTEST_HW_OK;
        }
        trace->truncated = true; /* miss/failure: fall through to block-step */
    }

    /* (2) Complete rootless — BTF block-step (no window ceiling), fork-isolated re-run.
     * This is the key middle rung the static CASCADE[] lacks (block-step is a ptrace
     * entry, not a resolve backend).
     *
     * T8: ASMTEST_TRACE_IBS_PRECOVER opts into priming this rung's reconstructor with an
     * IBS covered-block survey (build_ibs_precover above) — memoization only, never a
     * change to what gets recorded (asmtest_ibs.h's INVARIANT), so the trace below is
     * byte-for-byte identical with or without the bit. Any survey/build failure leaves
     * `precover` NULL and the call falls back to the plain scan exactly as before this
     * bit existed. */
    if (asmtest_ptrace_blockstep_available()) {
        call_auto_reset(trace);
        ran =
            0; /* F24: reset wiped any prior partial — re-earn `ran` on success only */
        asmtest_bs_precover_t *precover = NULL;
        if ((policy & ASMTEST_TRACE_IBS_PRECOVER) && asmtest_ibs_available()) {
            precover = build_ibs_precover(code, len, args, nargs);
            if (precover != NULL)
                asmtest_bs_precover_set_current(precover);
        }
        int bstep_rc = asmtest_ptrace_trace_call_blockstep(
            code, len, args, nargs, result, trace);
        if (precover != NULL) {
            asmtest_bs_precover_set_current(NULL);
            asmtest_bs_precover_free(precover);
        }
        if (bstep_rc == ASMTEST_PTRACE_OK) {
            ran = 1;
            if (used != NULL) {
                used->tier = ASMTEST_TIER_HWTRACE;
                used->backend =
                    ASMTEST_HWTRACE_SINGLESTEP; /* single-step fidelity */
                used->fidelity = ASMTEST_FIDELITY_NATIVE;
                used->mechanism = ASMTEST_TRACE_MECH_BLOCKSTEP;
            }
            if (!trace->truncated)
                return ASMTEST_HW_OK;
        } else {
            trace->truncated =
                true; /* a failed tier must not read empty-yet-complete */
        }
    }

    /* (3) Complete floor — per-instruction single-step, fork-isolated re-run. */
    if (asmtest_ptrace_available()) {
        call_auto_reset(trace);
        ran =
            0; /* F24: reset wiped any prior partial — re-earn `ran` on success only */
        if (asmtest_ptrace_trace_call(code, len, args, nargs, result, trace) ==
            ASMTEST_PTRACE_OK) {
            ran = 1;
            if (used != NULL) {
                used->tier = ASMTEST_TIER_HWTRACE;
                used->backend = ASMTEST_HWTRACE_SINGLESTEP;
                used->fidelity = ASMTEST_FIDELITY_NATIVE;
                used->mechanism = ASMTEST_TRACE_MECH_PER_INSN;
            }
        } else {
            trace->truncated = true;
        }
    }

    if (!ran && used != NULL) {
        /* F22: no rung committed — a later rung's reset may have wiped an
         * earlier winner AFTER it stamped *used, so clear the stale identity
         * back to the MECH_NONE default rather than returning EUNAVAIL beside
         * a rung that did not produce this (empty) trace. */
        used->tier = ASMTEST_TIER_HWTRACE;
        used->backend = ASMTEST_HWTRACE_SINGLESTEP;
        used->fidelity = ASMTEST_FIDELITY_NATIVE;
        used->mechanism = ASMTEST_TRACE_MECH_NONE;
    }
    return ran ? ASMTEST_HW_OK : ASMTEST_HW_EUNAVAIL;
}
