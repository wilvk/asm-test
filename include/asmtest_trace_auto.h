/*
 * asmtest_trace_auto.h — the CROSS-TIER trace orchestrator.
 *
 * asmtest has three native/emulated trace tiers, each a separate library with its
 * own call API but all filling the same asmtest_trace_t shape (one offset basis,
 * one block partition):
 *
 *   - the hardware tier   (libasmtest_hwtrace): Intel PT > AMD LBR > single-step >
 *                          CoreSight, itself resolved by asmtest_hwtrace_resolve();
 *   - the DynamoRIO tier  (libasmtest_drapp): in-process software DBI, no depth
 *                          ceiling, vendor/uarch-independent;
 *   - the emulator tier   (libasmtest_emu): the universal floor — a Unicorn virtual
 *                          CPU that runs on every host but traces an ISOLATED guest,
 *                          not the real in-process CPU.
 *
 * asmtest_hwtrace_resolve() stops at the hardware tier's library boundary. This
 * header is the front-end OVER all three: it walks the full descending-fidelity
 * cascade — Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
 * emulator (the order of docs/internal/analysis/trace-parity-matrix.md, Matrix 8: DynamoRIO
 * ranks ABOVE single-step because its code cache runs at native speed while
 * single-step pays a kernel round-trip per instruction) — and returns the
 * available options for the caller to bracket with that tier's own begin/end.
 *
 * The one rule the cascade must never cross silently: every fall preserves the
 * trace DATA shape, but the native->emulator step changes execution FIDELITY (real
 * CPU, in-process -> virtual CPU, isolated guest). So that step is gated behind an
 * explicit ASMTEST_TRACE_NATIVE_ONLY opt-out, never an automatic last resort.
 *
 * Ships in libasmtest_hwtrace (the natural extension of asmtest_hwtrace_resolve);
 * it calls asmtest_hwtrace_available() directly and dlopen-probes libasmtest_drapp
 * for the DynamoRIO tier (located via $ASMTEST_DRAPP_LIB, else the default soname),
 * so it hard-links neither the DynamoRIO nor the emulator tier — keeping the three
 * libraries decoupled exactly as they are today.
 */
#ifndef ASMTEST_TRACE_AUTO_H
#define ASMTEST_TRACE_AUTO_H

#include <stddef.h>

#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The trace tiers, most-faithful to least. */
typedef enum {
    ASMTEST_TIER_HWTRACE = 0, /* HW branch trace / single-step (real CPU)     */
    ASMTEST_TIER_DYNAMORIO =
        1, /* in-process software DBI (real CPU)           */
    ASMTEST_TIER_EMULATOR =
        2, /* Unicorn virtual CPU (isolated guest)         */
} asmtest_trace_tier_t;

/* Execution fidelity of a tier. NATIVE runs the real bytes on the real CPU in this
 * process; VIRTUAL runs an isolated guest on an emulated CPU. The single
 * NATIVE->VIRTUAL transition is the fidelity line ASMTEST_TRACE_NATIVE_ONLY gates. */
typedef enum {
    ASMTEST_FIDELITY_NATIVE = 0,
    ASMTEST_FIDELITY_VIRTUAL = 1,
} asmtest_trace_fidelity_t;

/* A resolved trace option: which tier to use, which hardware backend within it
 * (meaningful ONLY when tier == ASMTEST_TIER_HWTRACE; otherwise 0/ignore), and the
 * fidelity class so a caller can see at a glance whether a choice crosses the
 * native->emulator line. */
typedef struct {
    asmtest_trace_tier_t tier;
    asmtest_trace_backend_t
        backend; /* valid iff tier == ASMTEST_TIER_HWTRACE */
    asmtest_trace_fidelity_t fidelity;
} asmtest_trace_choice_t;

/* Portable compile-time assert (same idiom as asmtest.h), so the binding layout
 * guard below compiles from C and C++. */
#ifndef ASMTEST_STATIC_ASSERT
#ifdef __cplusplus
#define ASMTEST_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define ASMTEST_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif
/* Pinned for the language bindings: three int-sized enum fields, no padding, so a
 * binding can marshal a choice as three consecutive C ints. */
ASMTEST_STATIC_ASSERT(sizeof(asmtest_trace_choice_t) == 3 * sizeof(int),
                      "asmtest_trace_choice_t must be three int-sized fields");

/* Policy is a bitmask (composable), passed across the FFI as an int. */
#define ASMTEST_TRACE_BEST                                                     \
    0x0 /* most-faithful available; emulator floor allowed */
/* Drop the one ceiling-bounded backend (AMD LBR: Tier-B stitching decodes past its
 * 16-deep stack, but capture is still bounded by the data ring / PMI throttling):
 * the policy to re-resolve under after a trace comes back trace.truncated. Mirrors
 * the hardware tier's ASMTEST_HWTRACE_CEILING_FREE. */
#define ASMTEST_TRACE_CEILING_FREE 0x1
/* Forbid the native->emulator crossing: resolve only the real-CPU tiers (hardware +
 * DynamoRIO). On a host with no native tier this makes the cascade empty, so
 * asmtest_trace_auto() returns ASMTEST_HW_EUNAVAIL instead of silently downgrading
 * execution fidelity to the isolated guest. */
#define ASMTEST_TRACE_NATIVE_ONLY 0x2

/* Resolve this host's full cross-tier fallback cascade, most-faithful first, into
 * out[0..cap), honoring `policy`; return how many entries were written.
 *
 * The fixed order is Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
 * emulator, each included only if its tier reports available on this host
 * (asmtest_hwtrace_available() for the four HW backends, a dlopen probe of
 * asmtest_dr_available() for DynamoRIO, and unconditionally for the emulator floor
 * unless ASMTEST_TRACE_NATIVE_ONLY drops it). On any x86-64 Linux host the result is
 * non-empty (single-step is a native floor); under ASMTEST_TRACE_NATIVE_ONLY a host
 * with no native tier (e.g. macOS arm64) resolves to 0 entries. */
size_t asmtest_trace_resolve(unsigned policy, asmtest_trace_choice_t *out,
                             size_t cap);

/* The single most-preferred AVAILABLE choice under `policy`, written to *out.
 * Returns ASMTEST_HW_OK (0) and fills *out, or ASMTEST_HW_EUNAVAIL when the cascade
 * is empty (only off a native host under ASMTEST_TRACE_NATIVE_ONLY) or `out` is
 * NULL. Dynamic-fallback idiom: resolve under ASMTEST_TRACE_BEST; if the trace comes
 * back truncated, re-resolve under ASMTEST_TRACE_CEILING_FREE and, when the choice
 * differs, re-run. */
int asmtest_trace_auto(unsigned policy, asmtest_trace_choice_t *out);

/* Auto-escalating CALL-OWNING trace — the dynamic-fallback idiom above, IMPLEMENTED.
 * Runs code(args…) under the fastest exact tier and, when the trace comes back
 * `truncated`, escalates to a ceiling-free tier and re-runs, until the trace is complete
 * or the tiers are exhausted. This owns the invocation, so the routine must be
 * RE-RUNNABLE (deterministic / idempotent): it is invoked once per attempted tier — the
 * fast HWTRACE step in THIS process, then the fork-isolated ptrace escalations.
 *
 * `code`/`len` is the registered routine; `args`/`nargs` are 0..6 integer args (SysV; FP
 * or >6 args -> ASMTEST_HW_EINVAL). `policy` is the STARTING policy (ASMTEST_TRACE_BEST;
 * ASMTEST_TRACE_CEILING_FREE skips the ceiling-bounded fast backend up front). On return
 * *result (may be NULL) holds the routine's return value; `trace` (caller-allocated with
 * asmtest_trace_new) is filled by the tier that produced the final trace, and
 * `trace->truncated` reports completeness; *used (may be NULL) reports the {tier, backend}
 * that produced it (so a caller can see whether escalation fired — e.g. used.backend !=
 * ASMTEST_HWTRACE_AMD_LBR).
 *
 * Ladder: best HWTRACE backend (AMD LBR #3-snapshot / Intel PT / in-proc single-step) ->
 * BTF block-step (asmtest_ptrace_trace_call_blockstep — rootless, no ceiling) ->
 * per-instruction single-step (asmtest_ptrace_trace_call). DynamoRIO and the emulator are
 * excluded (no call-owning entry / native->virtual fidelity crossing).
 *
 * Returns ASMTEST_HW_OK when some tier ran (read trace->truncated for completeness),
 * ASMTEST_HW_EUNAVAIL if no call-owning tier is available, ASMTEST_HW_EINVAL on bad args.
 * See docs/internal/plans/auto-escalating-trace-plan.md. */
int asmtest_trace_call_auto(const void *code, size_t len, const long *args,
                            int nargs, unsigned policy, long *result,
                            asmtest_trace_t *trace,
                            asmtest_trace_choice_t *used);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_TRACE_AUTO_H */
