/*
 * asmtest_ibs.h — statistical AMD IBS (Instruction-Based Sampling) tracing lane.
 *
 * This is a SEPARATE diagnostic producer, not a member of the exact-trace cascade
 * (asmtest_hwtrace.h / asmtest_trace_backend_t). Where the four hwtrace backends
 * reconstruct an *exact, ordered* asmtest_trace_t (which instructions ran), IBS-Op
 * yields *statistical* control-flow EDGES {from -> to}: a retire-side sampler tags
 * one retired op per NMI window and, for taken branches, delivers both the branch
 * source (IbsOpRip) and target (IbsBrTarget). Aggregated over a run, the samples
 * form a hot-edge / hot-block histogram — the AutoFDO/BOLT shape, never an ordered
 * path.
 *
 * Why it exists: on AMD Zen 2 (family 0x17) every branch-STACK facility is absent
 * (no BRS / LbrExtV2), so every hwtrace backend self-skips and the machine falls
 * back to software single-stepping — which is ~1000x slower and unsafe on a live
 * JIT/managed runtime. IBS is the one branch-tracing facility this silicon has, and
 * it observes a running target OUT OF BAND, UNPRIVILEGED (kernel `swfilt` makes
 * user-only sampling open at perf_event_paranoid=2), and WITHOUT perturbing its
 * control flow — precisely the case the single-step tier is dangerous on.
 *
 * INVARIANT: IBS is statistical and must NEVER feed the exact insns[]/blocks[]
 * parity contract. It can prove a block WAS seen; it can never prove one was NOT
 * executed. It is a coverage/profile signal, labelled as such.
 *
 * Linux + x86-64 + AMD only. asmtest_ibs_available() encodes the full detect chain
 * so callers self-skip cleanly everywhere else. No external library: raw
 * perf_event_open + memcpy (the pure decoder needs no perf headers at all, so it is
 * host-independent and unit-tested on every CI host). See
 * docs/internal/plans/zen2-ibs-tracing-plan.md.
 */
#ifndef ASMTEST_IBS_H
#define ASMTEST_IBS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. Values mirror asmtest_hwtrace.h's ASMTEST_HW_* for muscle memory;
 * the positive ASMTEST_IBS_NOEDGE is a "decoded fine, nothing to record" signal,
 * distinct from both success-with-an-edge (0) and the negative errors. */
#define ASMTEST_IBS_OK       0
#define ASMTEST_IBS_NOEDGE   1    /* well-formed op, not a taken branch */
#define ASMTEST_IBS_EINVAL   (-1) /* NULL / bad argument                */
#define ASMTEST_IBS_EUNAVAIL (-3) /* IBS/AMD/Linux/kernel absent        */
#define ASMTEST_IBS_EDECODE  (-8) /* raw record too short or malformed  */

/* One aggregated statistical control-flow edge {from -> to}, sampled by IBS-Op. */
typedef struct {
    uint64_t from;      /* branch SOURCE (IbsOpRip)                */
    uint64_t to;        /* branch TARGET (IbsBrTarget)             */
    uint64_t count;     /* IBS samples aggregated into this edge   */
    unsigned taken;     /* 1: every edge is a retired taken branch */
    unsigned mispred;   /* samples flagged mispredicted            */
    unsigned is_return; /* samples whose retired op was a return   */
} asmtest_ibs_edge_t;

/* A statistical survey: edges sorted by DESCENDING count, plus honest provenance. */
typedef struct {
    asmtest_ibs_edge_t *edges; /* malloc'd; free via survey_free */
    size_t n;                  /* number of distinct edges       */
    uint64_t samples;          /* total IBS-Op samples drained   */
    uint64_t branch_samples;   /* retired taken-branch samples   */
    uint64_t lost;             /* dropped samples (RECORD_LOST)  */
    int throttled;             /* throttled / ring near-full     */
} asmtest_ibs_survey_t;

/* Capture options. Pass NULL for defaults. Reserved fields are 0 today and carry
 * later phases (cnt_ctl dispatched-op sampling, callchain, whole-process). */
typedef struct {
    uint64_t sample_period; /* 0 => default 0x4000; rounded to /16 */
    unsigned flags;         /* reserved (0)                        */
    unsigned reserved[5];   /* reserved (0)                        */
} asmtest_ibs_opts_t;

/* 1 iff this is a Linux/x86-64 AMD host whose IBS-Op supports op sampling (OpSam)
 * and branch target (BrnTrgt), the ibs_op PMU is present, and the kernel exposes
 * the `swfilt` software-filter bit (so user-only sampling opens unprivileged at
 * perf_event_paranoid=2). Cached; safe to call repeatedly. */
int asmtest_ibs_available(void);

/* A human-readable reason asmtest_ibs_available() returned 0 (a static string;
 * empty string when available). e.g. "not AMD", "no ibs_op PMU",
 * "no swfilt (kernel < ~6.2)", "no IBS branch target (BrnTrgt)". */
const char *asmtest_ibs_skip_reason(void);

/* PURE, host-independent decode of one IBS-Op PERF_SAMPLE_RAW payload into an edge.
 * `raw` points at the raw payload ([u32 caps][u64 regs...] — what follows the
 * PERF_SAMPLE_RAW size field) and `raw_len` is its byte length. No hardware and no
 * perf headers, so it compiles and is unit-tested on ANY host. Returns:
 *   ASMTEST_IBS_OK      — a retired TAKEN branch; *out is a usable edge (count = 1).
 *   ASMTEST_IBS_NOEDGE  — well-formed op, but not a retired taken branch, or the
 *                         RIP was flagged invalid: nothing to record (*out zeroed).
 *   ASMTEST_IBS_EINVAL  — NULL argument.
 *   ASMTEST_IBS_EDECODE — raw_len too short to hold the branch-target register. */
int asmtest_ibs_decode_op(const void *raw, size_t raw_len,
                          asmtest_ibs_edge_t *out);

/* Attach IBS-Op to one thread `tid` (0 => the calling thread), sample for `ms`
 * milliseconds, and return the aggregated edge histogram in *out (edges sorted by
 * descending count). Out-of-band and unprivileged: no ptrace, no single-step, and
 * the sampled thread runs unperturbed. For a MEANINGFUL self-profile the work must
 * run on ANOTHER thread while this call drains (a thread that only sleeps in the
 * drain loop generates no user branches to sample). Returns ASMTEST_IBS_OK,
 * ASMTEST_IBS_EINVAL on bad args, ASMTEST_IBS_EUNAVAIL when IBS cannot open (see
 * asmtest_ibs_available). *out is always zero-initialised first, so a caller can
 * inspect it after any return. */
int asmtest_ibs_survey_pid(pid_t tid, unsigned ms,
                           const asmtest_ibs_opts_t *opts,
                           asmtest_ibs_survey_t *out);

/* Attach IBS-Op to EVERY thread of process `pid` (0 => the calling process),
 * sample for `ms` milliseconds, and return one MERGED edge histogram in *out.
 * Opens one out-of-band event per thread enumerated from /proc/<pid>/task and
 * rescans that directory once, mid-window, to pick up threads spawned after the
 * survey starts. Same statistical / out-of-band / unprivileged contract as
 * asmtest_ibs_survey_pid — the whole target runs unperturbed (no ptrace, no
 * single-step).
 *
 * RESIDUAL RACE: a thread both born AND dead entirely between the initial scan and
 * the single mid-window rescan can be missed; the clean fix is a privileged
 * system-wide per-CPU capture (a later opts.system_wide phase). Threads that vanish
 * before their event opens are skipped, not fatal. Coverage is capped at a large
 * fixed number of threads (see ibs_backend.c); a target with more live threads than
 * that is surveyed partially.
 *
 * Returns ASMTEST_IBS_OK (even if only some threads were captured),
 * ASMTEST_IBS_EINVAL on bad args, or ASMTEST_IBS_EUNAVAIL when IBS is unavailable,
 * the process has no readable task list, or no thread could be attached. *out is
 * always zero-initialised first. */
int asmtest_ibs_survey_process(pid_t pid, unsigned ms,
                               const asmtest_ibs_opts_t *opts,
                               asmtest_ibs_survey_t *out);

/* Free the edge array a survey owns and zero the struct (idempotent; NULL-safe). */
void asmtest_ibs_survey_free(asmtest_ibs_survey_t *s);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_IBS_H */
