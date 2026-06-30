/*
 * amd_backend.c — AMD branch-record decode backend for the hardware-trace tier.
 * See docs/plans/amd-lbr-trace-plan.md, asmtest_hwtrace.h, docs/native-tracing.md.
 *
 * AMD has no Intel-PT-style continuous trace ring; the closest native trace is
 * reconstruction from the shallow (16-entry) branch-record stack — Zen 3 BRS or
 * Zen 4 LbrExtV2 — captured via perf PERF_SAMPLE_BRANCH_STACK as an ordered array
 * of {from, to, flags} taken-branch records (hwtrace.c does the capture). This TU
 * turns that array into the same asmtest_trace_t offset stream the Intel PT
 * backend produces, by REPLAYING asm-test's own registered code bytes between the
 * branch waypoints.
 *
 * Unlike libipt, this carries no instruction decoder of its own: it reuses the
 * project's existing Capstone layer through asmtest_disas() (src/disasm.c) for the
 * instruction-length walk. asmtest_amd_decoder_present() is therefore true only
 * when Capstone is linked (asmtest_disas_available()) on a Linux x86-64 build;
 * otherwise the tier self-skips, exactly like the libipt/OpenCSD gating.
 *
 * The capture needs an AMD Zen 3 (BRS) / Zen 4 / Zen 5 (LbrExtV2) host with perf
 * branch-stack permitted; it self-skips on Zen 2 (no branch facility), non-AMD, VMs,
 * and CI. Live capture + decode is verified on a Zen 5 host (Ryzen 9 9950X,
 * amd_lbr_v2 — see examples/test_hwtrace.c test_amd_live and `make docker-hwtrace-amd`);
 * THIS reconstruction is additionally validated host-independently with synthetic
 * perf_branch_entry[] inputs (test_amd_reconstruction), cross-checked against the
 * DynamoRIO/PT block+instruction partition.
 */
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#define ASMTEST_HW_OK 0
#define ASMTEST_HW_ENOSYS (-5)
#define ASMTEST_HW_EDECODE (-8)

#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>

int asmtest_amd_decoder_present(void) {
    /* Reconstruction needs the Capstone length-decoder (via asmtest_disas). */
    return asmtest_disas_available() ? 1 : 0;
}

/* AMD's 16-deep branch stack. A full stack (nbr >= AMD_LBR_DEPTH) means the
 * routine took at least that many branches and earlier ones were lost — the
 * window overflowed, so a single-snapshot (Tier-A) reconstruction cannot be
 * complete. Tier-B stitching (asmtest_amd_stitch) lifts that ceiling. */
#define AMD_LBR_DEPTH 16

/* Replay an ordered (newest-first) taken-branch array into the trace, decoding the
 * in-region straight-line runs between branches with Capstone. Shared by Tier-A
 * (single snapshot) and Tier-B (stitched sequence): it knows nothing about window
 * depth — the callers decide whether the array is complete. Sets trace->truncated
 * only on a decode desync / undecodable byte. */
static void amd_replay(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, uint64_t base_ip, size_t len,
                       asmtest_trace_t *trace) {
    const uint64_t end_ip = base_ip + len;
    uint64_t ip = base_ip;

    /* Block start at the region entry (matches PT/DR normalization). */
    trace_append_block(trace, 0);

    /* perf delivers the branch stack newest-first; replay oldest-first. */
    for (size_t k = nbr; k-- > 0;) {
        const struct perf_branch_entry *e = &br[k];
        if (e->abort)
            continue; /* transactional abort: path rolled back, not executed */
        uint64_t from = e->from, to = e->to;

        /* Decode the straight-line run from the current in-region IP up to and
         * including the branch instruction at `from`. Skip when IP or the branch
         * is outside the region (e.g. executing inside a callee). */
        if (ip >= base_ip && ip < end_ip && from >= ip && from < end_ip) {
            uint64_t o = ip - base_ip;
            const uint64_t from_off = from - base_ip;
            for (;;) {
                size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, (const uint8_t *)base,
                                         len, base_ip, o, NULL, 0);
                if (l == 0) { /* undecodable: cannot trust the rest */
                    trace->truncated = true;
                    return;
                }
                trace_append_insn(trace, o);
                if (o == from_off)
                    break; /* recorded the branch instruction itself */
                o += l;
                if (o > from_off) { /* walked past the branch: decode desync */
                    trace->truncated = true;
                    return;
                }
            }
        }

        /* Follow the branch. A target inside the region begins a new block (and
         * resumes decoding there); a target outside (call/ret leaving) just moves
         * IP — a later record whose `to` re-enters the region resumes decoding. */
        ip = to;
        if (to >= base_ip && to < end_ip)
            trace_append_block(trace, to - base_ip);
    }
}

int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace) {
    if (br == NULL || nbr == 0 || base == NULL || len == 0 || trace == NULL)
        return ASMTEST_HW_EDECODE;
    if (!asmtest_disas_available())
        return ASMTEST_HW_ENOSYS;

    amd_replay(br, nbr, base, (uint64_t)(uintptr_t)base, len, trace);

    /* Window overflow: a full stack means earlier branches were dropped, so the
     * reconstruction is incomplete — flag it (the caller falls back to the
     * DynamoRIO tier, or to Tier-B stitching, which have no depth ceiling). Never
     * emit partial as complete. */
    if (nbr >= AMD_LBR_DEPTH)
        trace->truncated = true;
    return ASMTEST_HW_OK;
}

/* Two taken-branch records are the same edge iff they share from+to. (Speculation/
 * abort flags can differ between a sample and its successor for the same edge.) */
static int amd_edge_eq(const struct perf_branch_entry *a,
                       const struct perf_branch_entry *b) {
    return a->from == b->from && a->to == b->to;
}

/* Tier-B stitching: splice the overlapping 16-deep windows that sample_period=1
 * emits (one per taken branch, so consecutive windows overlap by 15 edges) into one
 * gapless newest-first taken-branch sequence past the depth ceiling.
 *
 * `samples[i]` (newest-first, `nrs[i]` entries) is the branch stack captured at the
 * i-th retained sample, in time order. Builds the union in execution (oldest-first)
 * order in `out`, then reverses it to the newest-first order asmtest_amd_decode_
 * stitched consumes; returns the stitched count. For each new window we take the
 * SMALLEST shift that still overlaps the accumulated tail (the contiguous,
 * largest-overlap assumption) and append only the genuinely new edges — so a loop's
 * repeated identical edges stitch correctly (each window contributes one). If a
 * window shares no edge with the tail (>= a full window of samples were dropped to
 * perf throttling) the sequence has a real gap: *gap is set and stitching stops at
 * the correct prefix. *gap is also set if `out` fills before the merge completes. */
size_t asmtest_amd_stitch(const struct perf_branch_entry *const *samples,
                          const size_t *nrs, size_t n_samples,
                          struct perf_branch_entry *out, size_t out_cap, int *gap) {
    if (gap != NULL)
        *gap = 0;
    if (samples == NULL || nrs == NULL || n_samples == 0 || out == NULL ||
        out_cap == 0)
        return 0;

    size_t n = 0; /* merged length, execution order (oldest-first) in out[] */
    /* Seed with the first window in execution order. */
    {
        const struct perf_branch_entry *s = samples[0];
        size_t m = nrs[0];
        for (size_t j = 0; j < m && n < out_cap; j++)
            out[n++] = s[m - 1 - j];
        if (m > 0 && n < m && gap != NULL)
            *gap = 1; /* out too small for even the first window */
    }

    for (size_t i = 1; i < n_samples; i++) {
        const struct perf_branch_entry *s = samples[i];
        size_t m = nrs[i];
        if (m == 0)
            continue;

        /* Find the smallest shift d in [1, m-1] whose leading overlap (length m-d,
         * the older shared edges) matches the merged tail — largest overlap wins, so
         * d==1 (contiguous) is preferred. e[j] in execution order is s[m-1-j]. */
        size_t best_d = 0;
        for (size_t d = 1; d < m; d++) {
            size_t L = m - d; /* overlap length */
            if (L > n)
                continue; /* merged shorter than this overlap; try a bigger shift */
            int match = 1;
            for (size_t j = 0; j < L; j++) {
                if (!amd_edge_eq(&s[m - 1 - j], &out[n - L + j])) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                best_d = d;
                break;
            }
        }
        if (best_d == 0) {
            if (gap != NULL)
                *gap = 1; /* overlap lost: a real gap (throttled drop) */
            break;
        }
        /* Append the `best_d` newest edges this window contributes (execution
         * order): e[m-best_d .. m-1] == s[best_d-1 .. 0]. */
        for (size_t j = m - best_d; j < m; j++) {
            if (n >= out_cap) {
                if (gap != NULL)
                    *gap = 1; /* ran out of room: incomplete */
                break;
            }
            out[n++] = s[m - 1 - j];
        }
    }

    /* Reverse execution order -> newest-first for the decoder. */
    for (size_t a = 0, b = (n > 0 ? n - 1 : 0); a < b; a++, b--) {
        struct perf_branch_entry t = out[a];
        out[a] = out[b];
        out[b] = t;
    }
    return n;
}

/* Decode a Tier-B stitched (already-complete) branch sequence: like
 * asmtest_amd_decode but WITHOUT the 16-entry overflow flag, since stitching, not
 * the window depth, established completeness. `gap` (from asmtest_amd_stitch) is the
 * honest loss signal: nonzero -> the sequence had an unrecoverable hole. */
int asmtest_amd_decode_stitched(const struct perf_branch_entry *br, size_t nbr,
                                const void *base, size_t len,
                                asmtest_trace_t *trace, int gap) {
    if (br == NULL || nbr == 0 || base == NULL || len == 0 || trace == NULL)
        return ASMTEST_HW_EDECODE;
    if (!asmtest_disas_available())
        return ASMTEST_HW_ENOSYS;

    amd_replay(br, nbr, base, (uint64_t)(uintptr_t)base, len, trace);
    if (gap)
        trace->truncated = true;
    return ASMTEST_HW_OK;
}

#else /* not Linux x86-64 */

int asmtest_amd_decoder_present(void) { return 0; }

/* struct perf_branch_entry is Linux-only; use void* so the prototypes in
 * hwtrace.c's dispatch still link on other platforms. */
int asmtest_amd_decode(const void *br, size_t nbr, const void *base, size_t len,
                       asmtest_trace_t *trace) {
    (void)br;
    (void)nbr;
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

size_t asmtest_amd_stitch(const void *const *samples, const size_t *nrs,
                          size_t n_samples, void *out, size_t out_cap, int *gap) {
    (void)samples;
    (void)nrs;
    (void)n_samples;
    (void)out;
    (void)out_cap;
    if (gap != NULL)
        *gap = 0;
    return 0;
}

int asmtest_amd_decode_stitched(const void *br, size_t nbr, const void *base,
                                size_t len, asmtest_trace_t *trace, int gap) {
    (void)br;
    (void)nbr;
    (void)base;
    (void)len;
    (void)trace;
    (void)gap;
    return ASMTEST_HW_ENOSYS;
}

#endif
