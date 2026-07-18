/*
 * ibs_backend.h — INTERNAL window primitives for the statistical AMD IBS-Op lane.
 *
 * NOT a public header: it ships nothing in include/ and carries no ABI promise
 * (mirroring src/amd_backend.h / src/stealth_helper.h). The PUBLIC IBS surface is
 * include/asmtest_ibs.h (available/decode/survey_pid/survey_process). This header
 * adds the two primitives hwtrace.c needs for its Zen-2 F6 fallback and that do NOT
 * fit the survey_* shape: a window that ARMS IBS-Op on the calling thread, lets the
 * caller run its own workload inline, then DRAINS — the same begin/run/end shape as
 * asmtest_hwtrace_sample_begin_amd/_end_amd, so the branch-stack survey and its IBS
 * fallback share one control flow.
 *
 * Both are Linux/x86-64/AMD-only; everywhere else they self-skip (EUNAVAIL), like the
 * rest of the lane. They reuse asmtest_ibs_opts_t / asmtest_ibs_survey_t (freed with
 * asmtest_ibs_survey_free) from the public header.
 */
#ifndef ASMTEST_IBS_BACKEND_H
#define ASMTEST_IBS_BACKEND_H

#include "asmtest_ibs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Arm IBS-Op on the CALLING thread and return an opaque window handle in *ctx_out.
 * Sampling is live on return, so the caller runs its window body next and calls
 * asmtest_ibs_window_end to stop + collect. Returns ASMTEST_IBS_OK, ASMTEST_IBS_EINVAL
 * on a NULL out-param, or ASMTEST_IBS_EUNAVAIL when IBS-Op is unavailable / cannot open.
 * `opts` may be NULL (defaults). Pair EVERY OK with exactly one asmtest_ibs_window_end
 * (which frees the handle). */
int asmtest_ibs_window_begin(const asmtest_ibs_opts_t *opts, void **ctx_out);

/* Disable the window `ctx` (from asmtest_ibs_window_begin), drain its ring into the
 * aggregated edge histogram *out (sorted by descending count, same shape as a survey),
 * and free the handle. *out is always zero-initialised first (free it with
 * asmtest_ibs_survey_free). Returns ASMTEST_IBS_OK, or ASMTEST_IBS_EINVAL on a NULL
 * argument. */
int asmtest_ibs_window_end(void *ctx, asmtest_ibs_survey_t *out);

/* INTERNAL test seam (no ABI): the max-record bound the near-full-ring loss
 * heuristic reserves, callchain-aware. Lets a pure test pin the callchain-sized
 * sizing without opening perf (a base-bound build returns the base size for
 * has_callchain=1 and the test fails). */
size_t asmtest_ibs_max_record(int has_callchain);

/* ===========================================================================
 * IBS-Fetch front-end coverage lane (zen2-ibs-tracing-plan Phase 7). INTERNAL.
 *
 * The retire-side asmtest_ibs_* lane above samples RETIRED ops and yields
 * control-flow EDGES. This lane samples the FRONT END: the `ibs_fetch` PMU
 * (perf type read from sysfs, `swfilt` user-only, same envelope as ibs_op)
 * tags an instruction FETCH and delivers the fetched linear address plus the
 * fetch's i-cache / ITLB miss status and latency. Aggregated over a run it is a
 * fetch-address COVERAGE / hot-fetch histogram — "which code was fetched, and
 * where the front-end stalls are" — a view the retire-side Op sampler misses
 * (speculatively fetched but never-retired code shows up here, not there).
 *
 * INTERNAL-only (no include/ surface, no ABI promise, not in any binding): this
 * is a diagnostic producer, kept out of the public asmtest_ibs.h to avoid
 * growing the mirrored-binding surface. Same INVARIANT as the whole lane —
 * statistical, never a member of the exact insns[]/blocks[] parity contract.
 *
 * Split by portability exactly like the Op lane: asmtest_ibs_decode_fetch /
 * asmtest_ibs_fetch_survey_free are PURE (host-independent, unit-tested on every
 * CI host); asmtest_ibs_fetch_available / _skip_reason / _survey_fetch_pid are
 * Linux/x86-64/AMD only and self-skip everywhere else.
 * =========================================================================== */

/* One decoded IBS-Fetch sample: a fetched linear instruction address plus the
 * front-end status the fetch record carries. Trust `fetch_addr` only when the
 * sample is `valid` (IbsFetchVal): the linear-address register is meaningful only
 * for a valid fetch tag. */
typedef struct {
    uint64_t fetch_addr; /* IbsFetchLinAd: fetched linear instruction address */
    unsigned valid;      /* IbsFetchVal (bit 49): the fetch sample is valid   */
    unsigned complete;   /* IbsFetchComp (bit 50): the fetch completed        */
    unsigned icache_miss; /* IbsIcMiss (bit 51): the fetch missed the i-cache */
    unsigned itlb_miss;   /* IbsL1TlbMiss (bit 55): missed the L1 ITLB        */
    unsigned latency; /* IbsFetchLat (bits 47:32): fetch latency in clocks    */
} asmtest_ibs_fetch_sample_t;

/* One aggregated fetch-address coverage bucket (a fetch hotspot). Miss counts and
 * latency_sum are honest sums over the `count` samples at this address — a mean is
 * latency_sum/count; they are statistical, never exact per-instruction truth. */
typedef struct {
    uint64_t addr;        /* fetched linear instruction address (bucket key)  */
    uint64_t count;       /* valid IBS-Fetch samples aggregated at this addr  */
    uint64_t icache_miss; /* of those, how many missed the i-cache            */
    uint64_t itlb_miss;   /* of those, how many missed the L1 ITLB            */
    uint64_t latency_sum; /* sum of fetch latency over the samples            */
} asmtest_ibs_fetch_hot_t;

/* A fetch-coverage survey: hot fetch addresses sorted by DESCENDING count, plus
 * honest provenance (valid/total sampled, aggregate miss counts, loss, throttle). */
typedef struct {
    asmtest_ibs_fetch_hot_t *hot; /* malloc'd; free via _fetch_survey_free    */
    size_t n;                     /* number of distinct fetch addresses       */
    uint64_t samples;             /* total IBS-Fetch samples drained          */
    uint64_t valid_samples;       /* samples with IbsFetchVal set (aggregated)*/
    uint64_t icache_misses;       /* total i-cache-miss fetch samples         */
    uint64_t itlb_misses;         /* total L1-ITLB-miss fetch samples         */
    uint64_t lost;                /* dropped samples (PERF_RECORD_LOST)       */
    int throttled;                /* throttled / ring near-full               */
} asmtest_ibs_fetch_survey_t;

/* 1 iff this is a Linux/x86-64 AMD host whose IBS supports fetch sampling
 * (FetchSam), the ibs_fetch PMU is present, and the kernel exposes `swfilt` (so
 * user-only sampling opens unprivileged at perf_event_paranoid=2). Cached; safe to
 * call repeatedly. Independent of asmtest_ibs_available (the Op lane's probe). */
int asmtest_ibs_fetch_available(void);

/* Human-readable reason asmtest_ibs_fetch_available() returned 0 (static string;
 * empty when available). e.g. "not AMD", "no ibs_fetch PMU",
 * "no IBS fetch sampling (FetchSam)", "no swfilt (kernel < ~6.2)". */
const char *asmtest_ibs_fetch_skip_reason(void);

/* PURE, host-independent decode of one ibs_fetch PERF_SAMPLE_RAW payload into a
 * fetch sample. `raw` points at the payload ([u32 caps][u64 regs...] — what follows
 * the PERF_SAMPLE_RAW size field: reg[0]=IbsFetchCtl, reg[1]=IbsFetchLinAd,
 * reg[2]=IbsFetchPhysAd) and `raw_len` is its byte length. No hardware, no perf
 * headers — unit-tested on ANY host. All decoded status fields are filled whenever
 * the record is long enough; the return code says whether the sample is usable:
 *   ASMTEST_IBS_OK      — a VALID fetch sample; *out->fetch_addr is meaningful.
 *   ASMTEST_IBS_NOEDGE  — decoded fine but IbsFetchVal is clear: nothing to
 *                         aggregate (status fields filled; fetch_addr untrusted).
 *   ASMTEST_IBS_EINVAL  — NULL argument.
 *   ASMTEST_IBS_EDECODE — raw_len too short to hold the fetch linear-addr register. */
int asmtest_ibs_decode_fetch(const void *raw, size_t raw_len,
                             asmtest_ibs_fetch_sample_t *out);

/* Attach the ibs_fetch PMU to one thread `tid` (0 => the calling thread), sample
 * for `ms` milliseconds, and return the aggregated fetch-address coverage histogram
 * in *out (hot addresses sorted by descending count). Out-of-band and unprivileged:
 * no ptrace, no single-step; the sampled thread runs unperturbed. As with the Op
 * survey, a MEANINGFUL self-profile needs the work on ANOTHER thread while this call
 * drains. Returns ASMTEST_IBS_OK, ASMTEST_IBS_EINVAL on bad args, or
 * ASMTEST_IBS_EUNAVAIL when ibs_fetch cannot open (see asmtest_ibs_fetch_available;
 * substrate-present-but-perf-blocked, e.g. paranoid=4 without CAP_PERFMON, also
 * returns EUNAVAIL — a skip, not a failure). *out is always zero-initialised first. */
int asmtest_ibs_survey_fetch_pid(pid_t tid, unsigned ms,
                                 const asmtest_ibs_opts_t *opts,
                                 asmtest_ibs_fetch_survey_t *out);

/* Free the hot-address array a fetch survey owns and zero the struct (idempotent;
 * NULL-safe). Pure/host-independent. */
void asmtest_ibs_fetch_survey_free(asmtest_ibs_fetch_survey_t *s);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_IBS_BACKEND_H */
