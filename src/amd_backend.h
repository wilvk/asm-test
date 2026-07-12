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

/* P9 — one cached /proc/cpuinfo `flags` probe, shared by amd_backend.c (amd_lbr_v2 +
 * perfmon_v2 snapshot gate) and msr_lbr.c (amd_lbr_v2 MSR gate), replacing two hand-rolled
 * fopen/strstr copies whose drift would silently break the byte-identical-trace invariant.
 * asmtest_amd_has_cpu_flag caches the first `flags` line and reports whether `flag` is a
 * space-delimited token on it; asmtest_amd_flags_have is the PURE (no-I/O) matcher it uses,
 * exposed for a host-independent unit test (mirroring asmtest_amd_msr_decode_entry). Both
 * return 0 off x86-64 Linux (stubs). */
int asmtest_amd_has_cpu_flag(const char *flag);
int asmtest_amd_flags_have(const char *line, const char *flag);
/* P5 — how many exits the deterministic boundary snapshot can cover at once: one HW
 * execution breakpoint per exit, each in its own x86 debug register (HBP_NUM == 4; the
 * LBR-on event is a PMU counter, not a DR, so all four are free). A region with more
 * exits than this stays on the sampled path by default. */
#define ASMTEST_AMD_MAX_EXITS 4
/* Deterministic boundary LBR snapshot (branchsnap.c): arm an LBR-on event + one HW
 * execution breakpoint per region exit + the bpf_get_branch_snapshot program attached to
 * each (N bpf_links, ONE shared ringbuf), drain and decode at end. Routed here by the
 * `snapshot` option or by default for a 1..ASMTEST_AMD_MAX_EXITS-exit region on the
 * substrate that supports it (see hwtrace_begin_amd); ANY nonzero from _begin_multi (no
 * BPF toolchain, no caps, no LbrExtV2 substrate, a debugger holding a needed DR) makes
 * the marker path fall back to the sample_period=1 capture. ALL-OR-NOTHING: _begin_multi
 * arms every listed exit or arms none — a partially-covered exit set could silently miss
 * the taken exit. `branch_filter` mirrors opts.branch_filter (nonzero = reduced LBR
 * filter). _begin is the single-exit thin wrapper (the legacy last-exit best-effort the
 * explicit `snapshot` option keeps for >ASMTEST_AMD_MAX_EXITS-exit regions). */
int asmtest_amd_snapshot_begin_multi(const void *base, size_t len,
                                     const size_t *exit_offs, int nexit,
                                     int branch_filter);
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
/* Like asmtest_amd_decode, but also reports (via *reached_exit, may be NULL) whether the
 * reconstruction's TRAILING straight-line run reached a region EXIT (ret / region-leaving
 * direct uncond jmp) with no intervening unrecorded branch — i.e. the routine's last block
 * ran straight to the exit, so a window sampled before the exit branch was recorded (or
 * holding only the entry `call` edge) still reconstructs the FULL retired path. The live
 * ring parser OR-s it into the Tier-A exit-anchor completeness check so such a window is
 * not spuriously truncated; asmtest_amd_decode is the reached_exit==NULL thin wrapper. */
int asmtest_amd_decode_reach(const struct perf_branch_entry *br, size_t nbr,
                             const void *base, size_t len,
                             asmtest_trace_t *trace, int *reached_exit);
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
/* P5 — enumerate ALL of the region's exit instructions (ret OR region-leaving direct
 * tail-call jmp, the same Capstone classification as asmtest_amd_last_exit_off): writes
 * up to `cap` exit offsets into `out` (ascending, may be NULL with cap 0), stores the
 * TOTAL exit count in *nexit (may be NULL), and returns the offset of the true LAST exit
 * — even when it did not fit `out` — or (size_t)-1 if none decodes. The multi-exit
 * boundary snapshot arms one breakpoint per enumerated exit; the default-on selection
 * gates on *nexit in [1, ASMTEST_AMD_MAX_EXITS]. Defined in hwtrace.c; exposed for the
 * host-independent exit-enumeration test. */
size_t asmtest_amd_all_exits(const void *base, size_t len, size_t *out, int cap,
                             int *nexit);
/* Offset of the region's LAST exit instruction — a thin wrapper over
 * asmtest_amd_all_exits(base, len, NULL, 0, nexit); kept for the legacy single-site
 * callers and the host-independent tail-call exit-classification test. */
size_t asmtest_amd_last_exit_off(const void *base, size_t len, int *nexit);

/* F43 test seam (defined in hwtrace.c) — decode a LINEARIZED perf data-ring span
 * [buf, buf+span) of PERF_RECORD_SAMPLE/LOST/THROTTLE records into *trace, selecting the
 * richest-in-region window and applying the Tier-A/Tier-B + truncation rules the live
 * hwtrace_end_amd uses. `dsz` is the ring data size for the near-full-loss heuristic (0
 * disables it). Exposed for the host-independent ring-parse test (crafted buffers exercise
 * the framing / richest-window / nr-clamp / LOST logic that self-skips off AMD hardware). */
void asmtest_amd_ring_parse_decode(uint8_t *buf, size_t span, size_t dsz,
                                   const void *base, size_t len,
                                   asmtest_trace_t *trace);

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
