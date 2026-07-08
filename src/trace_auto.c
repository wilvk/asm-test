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

#include "asmtest_ptrace.h" /* block-step / per-insn call-owning escalation tiers */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* One row of the cross-tier cascade, in descending fidelity/preference order. */
typedef struct {
    asmtest_trace_tier_t tier;
    asmtest_trace_backend_t backend; /* only meaningful for the HWTRACE tier */
    asmtest_trace_fidelity_t fidelity;
} cascade_row_t;

static const cascade_row_t CASCADE[] = {
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_INTEL_PT, ASMTEST_FIDELITY_NATIVE},
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_AMD_LBR, ASMTEST_FIDELITY_NATIVE},
    {ASMTEST_TIER_DYNAMORIO, 0, ASMTEST_FIDELITY_NATIVE},
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_SINGLESTEP, ASMTEST_FIDELITY_NATIVE},
    {ASMTEST_TIER_HWTRACE, ASMTEST_HWTRACE_CORESIGHT, ASMTEST_FIDELITY_NATIVE},
    {ASMTEST_TIER_EMULATOR, 0, ASMTEST_FIDELITY_VIRTUAL},
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
 * buffers; zeroes the counts + the truncated bit so the winning tier stands alone). */
static void call_auto_reset(asmtest_trace_t *t) {
    t->insns_len = 0;
    t->insns_total = 0;
    t->blocks_len = 0;
    t->blocks_total = 0;
    t->truncated = false;
}

int asmtest_trace_call_auto(const void *code, size_t len, const long *args,
                            int nargs, unsigned policy, long *result,
                            asmtest_trace_t *trace,
                            asmtest_trace_choice_t *used) {
    if (code == NULL || trace == NULL || len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return ASMTEST_HW_EINVAL;
    if (used != NULL) {
        used->tier = ASMTEST_TIER_HWTRACE;
        used->backend = ASMTEST_HWTRACE_SINGLESTEP;
        used->fidelity = ASMTEST_FIDELITY_NATIVE;
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
                if (used != NULL)
                    used->backend = (asmtest_trace_backend_t)hb;
            }
            asmtest_hwtrace_shutdown();
        }
    }
    if (ran && !trace->truncated)
        return ASMTEST_HW_OK; /* the fast tier captured the whole path */

    /* (2) Complete rootless — BTF block-step (no window ceiling), fork-isolated re-run.
     * This is the key middle rung the static CASCADE[] lacks (block-step is a ptrace
     * entry, not a resolve backend). */
    if (asmtest_ptrace_blockstep_available()) {
        call_auto_reset(trace);
        if (asmtest_ptrace_trace_call_blockstep(code, len, args, nargs, result,
                                                trace) == ASMTEST_PTRACE_OK) {
            ran = 1;
            if (used != NULL)
                used->backend =
                    ASMTEST_HWTRACE_SINGLESTEP; /* single-step fidelity */
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
        if (asmtest_ptrace_trace_call(code, len, args, nargs, result, trace) ==
            ASMTEST_PTRACE_OK) {
            ran = 1;
            if (used != NULL)
                used->backend = ASMTEST_HWTRACE_SINGLESTEP;
        } else {
            trace->truncated = true;
        }
    }

    return ran ? ASMTEST_HW_OK : ASMTEST_HW_EUNAVAIL;
}
