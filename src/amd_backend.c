/*
 * amd_backend.c — AMD branch-record decode backend for the hardware-trace tier.
 * See docs/internal/plans/amd-tracing-plan.md, asmtest_hwtrace.h, docs/native-tracing.md.
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

#define ASMTEST_HW_OK      0
#define ASMTEST_HW_ENOSYS  (-5)
#define ASMTEST_HW_EDECODE (-8)

#if defined(__linux__) && defined(__x86_64__)
#include <cpuid.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int asmtest_amd_decoder_present(void) {
    /* Reconstruction needs the Capstone length-decoder (via asmtest_disas). */
    return asmtest_disas_available() ? 1 : 0;
}

/* Probe X86_FEATURE_AMD_LBR_PMC_FREEZE (CPUID 0x80000022 EAX[2]): whether this part
 * freezes the LBR stack on a performance-monitor interrupt. The 2024 kernel fix made
 * DEBUGCTLMSR_FREEZE_LBRS_ON_PMI conditional on this bit precisely because it is "not
 * the case for all Zen 4 processors." WITHOUT freeze, the recorded branch stack keeps
 * advancing after the counter overflow transitions to CPL0, so a sampled window can
 * silently NOT end at the region exit — the capture path must not trust a single-window
 * (Tier-A) result to be complete unless the window's newest branch actually left the
 * region. Cached; returns 1 (freeze present), 0 (absent or the leaf is unsupported). */
int asmtest_amd_freeze_available(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    /* The extended leaf must exist before it can report the bit. */
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) == 0 ||
        eax < 0x80000022u) {
        cached = 0;
        return cached;
    }
    if (__get_cpuid_count(0x80000022u, 0, &eax, &ebx, &ecx, &edx) == 0) {
        cached = 0;
        return cached;
    }
    cached = (eax & (1u << 2)) ? 1 : 0; /* EAX[2] = LbrAndPmcFreeze */
    return cached;
}

/* Whether this host has the SUBSTRATE for a deterministic software-event LBR snapshot
 * (AMD-plan P0 #2: a BPF program calling bpf_get_branch_snapshot() at the region
 * boundary reads the frozen 16-entry stack at a DETERMINISTIC point, replacing the
 * sample_period=1 flood + "richest-window" guessing that truncates a too-fast tiny
 * routine). The kernel's amd_pmu_v2_snapshot_branch_stack (merged 2024, wired into
 * perf_snapshot_branch_stack) gates it on: (1) AMD LbrExtV2 — the `amd_lbr_v2` CPU
 * feature (Zen 4/5); (2) perfmon v2 — the `perfmon_v2` feature; (3) Linux >= 6.10.
 * This reports whether all three hold — the capability's HARDWARE+KERNEL floor. The
 * actual capture additionally needs CAP_BPF/CAP_PERFMON at run time (checked when the
 * BPF program loads), so a 1 here means "the substrate is present," not "it will run
 * unprivileged." Returns 0 off AMD/Linux or where any gate is unmet. Cached. */
int asmtest_amd_snapshot_available(void) {
    static int cached = -1;
    if (cached >= 0)
        return cached;
    cached = 0;
    /* (1)+(2): the amd_lbr_v2 and perfmon_v2 CPU features, from /proc/cpuinfo flags
     * (the kernel only sets amd_lbr_v2 on parts whose LBR the snapshot path drives). */
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (f == NULL)
        return cached;
    int have_lbr_v2 = 0, have_perfmon_v2 = 0;
    char line[4096];
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "flags", 5) == 0) {
            if (strstr(line, " amd_lbr_v2") != NULL)
                have_lbr_v2 = 1;
            if (strstr(line, " perfmon_v2") != NULL)
                have_perfmon_v2 = 1;
            break; /* one flags line describes every (identical) core */
        }
    }
    fclose(f);
    if (!have_lbr_v2 || !have_perfmon_v2)
        return cached;
    /* (3): Linux >= 6.10 (the AMD snapshot backport floor). */
    struct utsname u;
    if (uname(&u) != 0)
        return cached;
    int maj = 0, min = 0;
    if (sscanf(u.release, "%d.%d", &maj, &min) != 2)
        return cached;
    if (maj > 6 || (maj == 6 && min >= 10))
        cached = 1;
    return cached;
}

/* AMD's branch stack is 16 deep on every shipping Zen part. A full stack (nbr >=
 * depth) means the routine took at least that many branches and earlier ones were
 * lost — the window overflowed, so a single-snapshot (Tier-A) reconstruction cannot be
 * complete. Tier-B stitching (asmtest_amd_stitch) lifts that ceiling. AMD_LBR_DEPTH is
 * the fallback constant; asmtest_amd_lbr_depth() reads the true runtime depth. */
#define AMD_LBR_DEPTH 16

/* The runtime LbrExtV2 branch-stack depth from CPUID 0x80000022 EBX (lbr_v2_stack_sz),
 * replacing the hardcoded 16 that drives the Tier-A/Tier-B overflow split and the stitch
 * bound. Every shipping Zen part reports 16, so this is future-proofing hygiene — a no-op
 * today, not a behavior change — that removes the silent assumption. Falls back to
 * AMD_LBR_DEPTH when the leaf is unsupported (Zen 3 BRS / pre-LbrExtV2, whose stack is
 * also 16 deep) or reports an out-of-range size. Cached. */
int asmtest_amd_lbr_depth(void) {
    static int cached = 0;
    if (cached > 0)
        return cached;
    int depth = AMD_LBR_DEPTH;
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(0x80000000u, &eax, &ebx, &ecx, &edx) != 0 &&
        eax >= 0x80000022u &&
        __get_cpuid_count(0x80000022u, 0, &eax, &ebx, &ecx, &edx) != 0) {
        /* EBX layout (kernel union cpuid_0x80000022_ebx): [3:0] num_core_pmc,
         * [9:4] lbr_v2_stack_sz, ... — the LBR depth is bits [9:4], NOT the low byte
         * (that is the core PMC count). */
        unsigned int sz = (ebx >> 4) & 0x3fu;
        if (sz >= 1 && sz <= 64)
            depth = (int)sz;
    }
    cached = depth;
    return cached;
}

/* PERF_BR_SPEC_WRONG_PATH — a branch that executed SPECULATIVELY on the wrong path
 * and never retired: a phantom edge LbrExtV2 (Zen 4/5) still records. Its stable UAPI
 * value is 1; named locally so we never textually redefine the header's own enum. The
 * `spec` bitfield only exists on Linux >= 6.1 <linux/perf_event.h>, so its use is gated
 * on ASMTEST_HAVE_PERF_BR_SPEC (the mk/native-trace.mk `-fsyntax-only` member probe).
 * Absent (older header / Zen 3 BRS, which is retired-only with no spec bits) the filter
 * compiles out — a no-op, never a desync. */
#ifdef ASMTEST_HAVE_PERF_BR_SPEC
#define ASMTEST_PERF_BR_SPEC_WRONG_PATH 1
#endif

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
#ifdef ASMTEST_HAVE_PERF_BR_SPEC
        if (e->spec == ASMTEST_PERF_BR_SPEC_WRONG_PATH)
            continue; /* speculative wrong-path branch: never retired — drop the
                       * phantom edge. Expected, NOT a decode desync, so (unlike the
                       * truncation sites below) it must not set trace->truncated. */
#endif
        uint64_t from = e->from, to = e->to;

        /* Decode the straight-line run from the current in-region IP up to and
         * including the branch instruction at `from`. Skip when IP or the branch
         * is outside the region (e.g. executing inside a callee). */
        if (ip >= base_ip && ip < end_ip && from >= ip && from < end_ip) {
            uint64_t o = ip - base_ip;
            const uint64_t from_off = from - base_ip;
            /* Bound the walk. In the default (full-filter) path `o` only ever
             * increases, so it always converges on from_off. Following a dropped
             * direct jmp (reduced filter, below) can move `o` BACKWARD, so a
             * self-referential dropped back-edge could spin; a converging walk
             * visits each in-region offset at most once, so > len steps == a
             * divergent follow — bail truncated (mirrors ptrace_backend.c's
             * block-step bound). */
            for (size_t guard = 0;; guard++) {
                if (guard >
                    len) { /* divergent follow (dropped back-edge cycle) */
                    trace->truncated = true;
                    return;
                }
                /* One Capstone decode yields length + is_call (used to classify an
                 * intermediate direct call below). is_ret is unused here — a mid-run
                 * ret is caught by the !direct branch — but the probe requires it. */
                int is_call = 0, is_ret = 0;
                size_t l = asmtest_disas_probe(ASMTEST_ARCH_X86_64,
                                               (const uint8_t *)base, len, o,
                                               &is_call, &is_ret);
                if (l == 0) { /* undecodable: cannot trust the rest */
                    trace->truncated = true;
                    return;
                }
                trace_append_insn(trace, o);
                if (o == from_off)
                    break; /* recorded the branch instruction itself */
                /* A branch-class instruction seen mid-run (o != from_off) is NOT the
                 * recorded taken branch. Under the default full branch filter every
                 * taken branch is recorded, so this can only be a NOT-taken
                 * conditional (fall through) — the follow logic below is then dead
                 * code and the trace is byte-identical to before. Under the opt-in
                 * REDUCED filter (asmtest_hwtrace_options_t.branch_filter), a taken
                 * DIRECT UNCONDITIONAL jmp is deliberately unrecorded to save an LBR
                 * slot; recognize it and FOLLOW its static target (decodable from the
                 * region bytes). Every OTHER class (ret, indirect jmp/call, direct
                 * call) stays recorded under the reduced filter too, so seeing one
                 * here is a decode desync. This block ends after every CTI, matching
                 * the PT/DR/Unicorn partition. */
                int was_branch = asmtest_disas_is_branch(
                    ASMTEST_ARCH_X86_64, (const uint8_t *)base, len, o);
                if (was_branch) {
                    uint64_t tgt = 0;
                    int direct = asmtest_disas_branch_target(
                        ASMTEST_ARCH_X86_64, (const uint8_t *)base, len,
                        base_ip, o, &tgt);
                    /* A recorded class (ret / indirect transfer / direct call) can
                     * only legally appear AT from_off; mid-run it is a desync. */
                    if (!direct || is_call) {
                        trace->truncated = true;
                        return;
                    }
                    if (asmtest_disas_is_uncond_jump(ASMTEST_ARCH_X86_64,
                                                     (const uint8_t *)base, len,
                                                     o)) {
                        /* Dropped direct unconditional jmp: follow its target. */
                        if (tgt < base_ip || tgt >= end_ip) {
                            /* Leaves the region: the in-region from_off this run was
                             * decoding toward is now unreachable straight-line. */
                            trace->truncated = true;
                            return;
                        }
                        uint64_t no = tgt - base_ip;
                        if (no >
                            from_off) { /* jumped past the recorded source */
                            trace->truncated = true;
                            return;
                        }
                        trace_append_block(trace,
                                           no); /* taken-target block, once */
                        o = no; /* follow: no fall-through insn/block for the jmp */
                        continue;
                    }
                    /* else: a direct conditional jcc, NOT taken (a taken jcc would
                     * be the recorded from) — fall through like the default path. */
                }
                o += l;
                if (o > from_off) { /* walked past the branch: decode desync */
                    trace->truncated = true;
                    return;
                }
                if (was_branch)
                    trace_append_block(trace, o);
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
    if ((int)nbr >= asmtest_amd_lbr_depth())
        trace->truncated = true;
    return ASMTEST_HW_OK;
}

/* Two taken-branch records are the same edge iff they share from+to. (Speculation/
 * abort flags can differ between a sample and its successor for the same edge.) */
static int amd_edge_eq(const struct perf_branch_entry *a,
                       const struct perf_branch_entry *b) {
    return a->from == b->from && a->to == b->to;
}

/* Straight-line decodability of the span [from_ip, to_ip): is `to_ip` reachable by a
 * forward Capstone instruction-length walk from `from_ip` (a branch TARGET / block
 * start) that lands EXACTLY on `to_ip` (the next branch's SOURCE)? The stitcher uses it
 * to reject a from+to overlap match that would splice two edges NOT connected by real
 * code. On an internally-consistent hardware branch-stack window this always holds
 * (from+to adjacency implies byte adjacency), so it is a no-op there; it fires only when
 * DROPPED/throttled samples make the smallest-overlap heuristic splice non-contiguous
 * edges — where an honest gap beats a silently-wrong stitch. Accepts (cannot disprove)
 * when either endpoint is outside [base_ip, base_ip+len) (the span is inside a callee
 * whose bytes we do not hold) or when Capstone is unavailable; rejects a backwards span,
 * an overshoot, or an undecodable byte. Follows a dropped direct unconditional jmp across
 * the span (the reduced-filter analogue of amd_replay's follow) so a legitimate
 * reduced-filter splice is not rejected as a spurious gap; inert on full-filter windows
 * (no dropped jmps to follow). */
static int amd_span_decodable(const void *base, uint64_t base_ip, size_t len,
                              uint64_t from_ip, uint64_t to_ip) {
    const uint64_t end_ip = base_ip + len;
    if (base == NULL || len == 0 || !asmtest_disas_available())
        return 1;
    if (from_ip < base_ip || from_ip >= end_ip || to_ip < base_ip ||
        to_ip > end_ip)
        return 1; /* endpoint in a callee / at the region edge: cannot disprove */
    if (to_ip < from_ip)
        return 0; /* branch source before its block start: not real code */
    uint64_t o = from_ip - base_ip;
    const uint64_t fo = to_ip - base_ip;
    for (size_t guard = 0; o < fo; guard++) {
        if (guard > len)
            return 1; /* divergent follow (dropped back-edge): cannot disprove */
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, (const uint8_t *)base,
                                 len, base_ip, o, NULL, 0);
        if (l == 0)
            return 0; /* undecodable byte */
        if (asmtest_disas_is_uncond_jump(ASMTEST_ARCH_X86_64,
                                         (const uint8_t *)base, len, o)) {
            uint64_t tgt = 0;
            if (asmtest_disas_branch_target(ASMTEST_ARCH_X86_64,
                                            (const uint8_t *)base, len, base_ip,
                                            o, &tgt)) {
                /* Dropped direct unconditional jmp: follow its static target
                 * across the splice (the reduced-filter window omits its edge). */
                if (tgt < base_ip || tgt >= end_ip)
                    return 1; /* leaves the region: cannot disprove */
                o = tgt - base_ip;
                if (o > fo)
                    return 0; /* followed jmp overshoots the splice target: the
                               * source fo is not reached — reject (honest gap),
                               * matching the straight-line overshoot below. */
                continue;     /* loop re-checks o < fo */
            }
            /* indirect jmp: its bytes still decode; fall through to the straight-
             * line advance (if this is the recorded IND_JUMP source it lands fo). */
        }
        if (o + l > fo)
            return 0; /* the walk overshoots the source */
        o += l;
    }
    return 1; /* o == fo: a clean, whole-instruction forward decode */
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
 * the correct prefix. *gap is also set if `out` fills before the merge completes.
 *
 * `base`/`base_ip`/`len` describe the registered code the from/to addresses index
 * (may be NULL/0 to disable the check). A candidate smallest-overlap match is accepted
 * only if the adjacency it would splice (the tail's newest branch target -> the first
 * newly-appended branch source) is real straight-line code (amd_span_decodable); an
 * indecodable splice — a dropped-sample artifact the from+to overlap alone cannot see —
 * is rejected in favor of a larger shift, and an honest gap if none decodes. */
size_t asmtest_amd_stitch(const struct perf_branch_entry *const *samples,
                          const size_t *nrs, size_t n_samples, const void *base,
                          uint64_t base_ip, size_t len,
                          struct perf_branch_entry *out, size_t out_cap,
                          int *gap) {
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
            /* Reject a from+to match whose spliced adjacency isn't real code: the
             * tail's newest target (out[n-1].to) must straight-line-decode to the
             * first edge this shift would append (s[d-1].from). No-op on consistent
             * hardware windows; catches a dropped-sample mis-stitch -> larger shift/gap. */
            if (match && amd_span_decodable(base, base_ip, len, out[n - 1].to,
                                            s[d - 1].from)) {
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
int asmtest_amd_freeze_available(void) { return 0; }
int asmtest_amd_snapshot_available(void) { return 0; }
int asmtest_amd_lbr_depth(void) { return 16; }

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
                          size_t n_samples, const void *base, uint64_t base_ip,
                          size_t len, void *out, size_t out_cap, int *gap) {
    (void)samples;
    (void)nrs;
    (void)n_samples;
    (void)base;
    (void)base_ip;
    (void)len;
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
