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

#include <stdlib.h>

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
