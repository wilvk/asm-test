/*
 * asmtest_blockstep_internal.h — PRIVATE IBS-covered-block pre-cover table for the
 * ptrace block-step reconstructors (src/ptrace_backend.c). NOT a public header and NOT
 * a bindings-parity tier header (no new public ABI symbols; mirrors
 * asmtest_descent_internal.h's shipped-uninstalled stance): production code (T8, the
 * ASMTEST_TRACE_IBS_PRECOVER cascade rung) and tests are the only callers.
 *
 * The table memoizes the tracer-side decode of the block-step reconstructors
 * (blockstep_reconstruct in src/ptrace_backend.c): for each IBS-covered leader it
 * pre-walks the straight-line run ONCE and caches the per-instruction facts
 * classify_branch would recompute on every #DB stop. A build seeded from real
 * asmtest_ibs_normalize_blocks output can only ever be looked up by a `from_off` that
 * is itself a real, executed block-start offset, so a cache HIT reproduces exactly
 * what a fresh Capstone scan would decide (same bytes, same primitives) — the walk
 * never uses statistical coverage to skip *recording* anything (the exact parity
 * contract, asmtest_ibs.h's INVARIANT). A MISS falls back to the shipped scan
 * unconditionally.
 */
#ifndef ASMTEST_BLOCKSTEP_INTERNAL_H
#define ASMTEST_BLOCKSTEP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "asmtest_ibs.h" /* asmtest_ibs_blocks_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct asmtest_bs_precover asmtest_bs_precover_t;

/* Build a pre-cover table from a region snapshot (code/len/base_ip, exactly what the
 * region block-step driver already has) plus region-relative covered leaders
 * (asmtest_ibs_normalize_blocks(survey, base_ip, len, &covered) output). Pure —
 * no ptrace, no hardware access. Leaders outside [0,len) or whose first byte is
 * undecodable are silently skipped (never cached, so a lookup for them always
 * misses and falls back to the shipped scan — the "hostile leader" case). Returns
 * NULL on OOM, on a NULL/empty `covered`, or if every leader was skipped. */
asmtest_bs_precover_t *
asmtest_bs_precover_build(const uint8_t *code, size_t len, uint64_t base_ip,
                          const asmtest_ibs_blocks_t *covered);

/* Free a table built by asmtest_bs_precover_build (idempotent; NULL-safe). */
void asmtest_bs_precover_free(asmtest_bs_precover_t *p);

/* Install (or, with NULL, clear) the pre-cover table the NEXT
 * asmtest_ptrace_trace_call_blockstep / asmtest_ptrace_trace_attached_blockstep call in
 * this process consults. Non-owning and not thread-local: the installer keeps `p`
 * alive for the call's duration and clears/frees it afterward (T8 installs it around
 * one such call in the auto-cascade's block-step rung). */
void asmtest_bs_precover_set_current(const asmtest_bs_precover_t *p);

/* Test hooks: cumulative branch-probe decode calls (asmtest_disas_probe, the Capstone
 * open/close a hot re-scan pays for) and pre-cover cache hits, across every block-step
 * reconstruction in this process since the last reset. */
void asmtest_bs_stats(uint64_t *probe_calls, uint64_t *precover_hits);
void asmtest_bs_stats_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_BLOCKSTEP_INTERNAL_H */
