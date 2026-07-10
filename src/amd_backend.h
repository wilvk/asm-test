/*
 * amd_backend.h — INTERNAL contract for the AMD branch-record trace backend, shared by
 * the four AMD trace TUs: amd_backend.c (the decoder), hwtrace.c (sampled capture),
 * branchsnap.c (deterministic BPF snapshot), and msr_lbr.c (MSR-direct snapshot). NOT a
 * public header — it ships nothing in include/ and carries no ABI promise, mirroring the
 * src/stealth_helper.h internal-header pattern.
 *
 * It replaces four hand-copied inline forward-declaration blocks (one per TU) with a
 * single source of truth, so a signature change in amd_backend.c becomes a COMPILE error
 * at every caller instead of silent UB at the call boundary. It also re-exports the
 * ASMTEST_HW_* status codes (via asmtest_hwtrace.h) so the AMD TUs stop locally
 * re-#define-ing them, and defines ASMTEST_AMD_REDUCED_FILTER once (it was byte-duplicated
 * in hwtrace.c and branchsnap.c).
 *
 * The prototypes reproduce amd_backend.c's EXACT platform split: the perf_branch_entry-
 * typed decode/stitch prototypes under the __linux__ && __x86_64__ guard, the void*-typed
 * stubs in the #else (struct perf_branch_entry is Linux-only — exactly why hwtrace.c
 * guarded them), and the stable-signature probes / snapshot entries unconditional.
 */
#ifndef ASMTEST_AMD_BACKEND_H
#define ASMTEST_AMD_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "asmtest_hwtrace.h" /* re-exports ASMTEST_HW_* codes + asmtest_trace_t */

/* Platform-independent (stable-signature) probes and snapshot entries: defined in BOTH
 * branches of amd_backend.c (the CPUID probes + runtime depth) and branchsnap.c (the
 * boundary-snapshot begin/end), so they always link. */
int asmtest_amd_decoder_present(void);
/* AMD LBR-stack freeze-on-PMI availability (CPUID 0x80000022 EAX[2]); without it a
 * sampled window cannot be trusted to end at the region exit (see hwtrace_end_amd). */
int asmtest_amd_freeze_available(void);
/* Whether the deterministic software-event LBR-snapshot substrate is present (AMD
 * LbrExtV2 + perfmon v2 + Linux >= 6.10); the boundary-snapshot capture's hardware+
 * kernel floor (run-time use still needs CAP_BPF/CAP_PERFMON). */
int asmtest_amd_snapshot_available(void);
/* AMD branch stack depth (runtime, from CPUID 0x80000022 EBX; 16 on every shipping
 * part): a richest-window count at the ceiling means the routine overflowed one
 * snapshot, so escalate from Tier-A to Tier-B stitching. */
int asmtest_amd_lbr_depth(void);
/* Deterministic boundary LBR snapshot (branchsnap.c): arm an LBR-on event + a HW
 * execution breakpoint at base+exit_off + the bpf_get_branch_snapshot program, drain and
 * decode at end. Routed here by the `snapshot` option or by default on the substrate that
 * supports it (see hwtrace_begin_amd); ANY nonzero from _begin (no BPF toolchain, no caps,
 * no LbrExtV2 substrate) makes the marker path fall back to the sample_period=1 capture.
 * `branch_filter` mirrors opts.branch_filter (nonzero = reduced LBR filter). */
int asmtest_amd_snapshot_begin(const void *base, size_t len, size_t exit_off,
                               int branch_filter);
int asmtest_amd_snapshot_end(asmtest_trace_t *trace);

#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h> /* struct perf_branch_entry + PERF_SAMPLE_BRANCH_* */

/* Reduced (SCOPE-SAFE) LBR branch filter for the opt-in branch_filter path: drops only
 * direct unconditional jmp (COND | IND_JUMP | ANY_CALL | ANY_RETURN kept). A direct uncond
 * jmp has a statically decodable target, so dropping it frees a 16-deep slot while
 * amd_replay reconstructs it from the region bytes — a byte-identical trace over a longer
 * window. Keeping ALL calls recorded preserves the decoder's in-region from_off anchor.
 * See docs/internal/plans/amd-tracing-plan.md (#2B). */
#define ASMTEST_AMD_REDUCED_FILTER                                             \
    (PERF_SAMPLE_BRANCH_USER | PERF_SAMPLE_BRANCH_COND |                       \
     PERF_SAMPLE_BRANCH_IND_JUMP | PERF_SAMPLE_BRANCH_ANY_CALL |               \
     PERF_SAMPLE_BRANCH_ANY_RETURN)

/* Replay the ordered perf branch-stack array into the same asmtest_trace_t offset stream
 * the Intel PT backend produces (amd_backend.c). Takes the branch array, not an AUX byte
 * stream. */
int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace);
/* Tier-B: stitch the overlapping sample_period=1 branch-stack windows into one gapless
 * sequence (asmtest_amd_stitch), then decode it without the 16-entry ceiling
 * (asmtest_amd_decode_stitched) — lifts the single-window limit past 16 branches. */
size_t asmtest_amd_stitch(const struct perf_branch_entry *const *samples,
                          const size_t *nrs, size_t n_samples, const void *base,
                          uint64_t base_ip, size_t len,
                          struct perf_branch_entry *out, size_t out_cap,
                          int *gap);
int asmtest_amd_decode_stitched(const struct perf_branch_entry *br, size_t nbr,
                                const void *base, size_t len,
                                asmtest_trace_t *trace, int gap);
/* Decode one raw LbrExtV2 FROM/TO MSR pair (msr_lbr.c): 1 = retired branch kept (fills
 * *out), 0 = empty slot or speculative wrong-path dropped. Pure (no MSR I/O) — exposed for
 * the host-independent MSR spec-filter unit test. */
int asmtest_amd_msr_decode_entry(uint64_t from, uint64_t to,
                                 struct perf_branch_entry *out);
/* Offset of the region's LAST exit instruction — ret OR a region-leaving direct tail-call
 * jmp — the boundary snapshot's breakpoint site, or (size_t)-1 if none decodes. *nexit (may
 * be NULL) receives the exit count; the default-on snapshot gates on *nexit == 1. Defined
 * in hwtrace.c; exposed for the host-independent tail-call exit-classification test. */
size_t asmtest_amd_last_exit_off(const void *base, size_t len, int *nexit);

#else /* not Linux x86-64: struct perf_branch_entry is Linux-only -> void*-typed stubs */

int asmtest_amd_decode(const void *br, size_t nbr, const void *base, size_t len,
                       asmtest_trace_t *trace);
size_t asmtest_amd_stitch(const void *const *samples, const size_t *nrs,
                          size_t n_samples, const void *base, uint64_t base_ip,
                          size_t len, void *out, size_t out_cap, int *gap);
int asmtest_amd_decode_stitched(const void *br, size_t nbr, const void *base,
                                size_t len, asmtest_trace_t *trace, int gap);

#endif

#endif /* ASMTEST_AMD_BACKEND_H */
