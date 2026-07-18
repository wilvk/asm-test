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

/* opts.flags bits (Phase 5 sampling-quality / call-graph knobs). flags == 0
 * selects the Phase-5 DEFAULTS: dispatched-op count control (uniform instruction
 * coverage) + sample-period jitter ON (breaks aliasing against periodic loops). */
/* opt OUT of the dispatched-op default -> count clock cycles (cnt_ctl=0). */
#define ASMTEST_IBS_OPT_COUNT_CYCLES (1u << 0)
/* attach a frame-pointer caller stack per sample (PERF_SAMPLE_CALLCHAIN +
 * exclude_callchain_kernel). HONESTY NOTE: no in-tree consumer decodes the
 * callchain — the drain parses PAST it to reach the RAW payload — so today
 * this flag buys no call-graph output; it costs ring bandwidth (records grow
 * to ~1.2 KB worst case) and a kernel get_callchain_buffers allocation that
 * can fail perf_event_open with ENOMEM. The internal window lane ignores it
 * (see ibs_backend.c). A future consumer is recorded in
 * docs/internal/archive/plans/zen2-ibs-tracing-plan.md Phase 5b. Op lane only. */
#define ASMTEST_IBS_OPT_CALLCHAIN (1u << 1)
/* asmtest_ibs_survey_process only: capture SYSTEM-WIDE per-CPU (all present +
 * future threads, no per-thread race), filtered to the target pid in software.
 * Needs CAP_PERFMON / perf_event_paranoid<=0, else the survey self-skips. */
#define ASMTEST_IBS_OPT_SYSTEM_WIDE (1u << 2)
/* disable sample-period jitter (use a fixed period). */
#define ASMTEST_IBS_OPT_NO_JITTER (1u << 3)

/* Capture options. Pass NULL for defaults. The struct grows ADDITIVELY: the two
 * legacy fields (sample_period, flags) sit at fixed offsets and are honoured for
 * any non-NULL opts; every field AFTER struct_size is read only when the caller's
 * struct_size covers it, so an older caller (struct_size 0 / a smaller value) gets
 * the defaults for the tail and a newer caller's unknown tail is ignored (the
 * perf_event_attr / asmtest_hwtrace_options_t precedent). Zero-initialise and set
 * struct_size = sizeof(asmtest_ibs_opts_t) — or use ASMTEST_IBS_OPTS_INIT. Total
 * size is unchanged from Phase 0 (the tail lands in the old reserved words). */
typedef struct {
    uint64_t sample_period; /* 0 => default 0x4000; rounded to /16   */
    unsigned flags;         /* ASMTEST_IBS_OPT_* mask (0 = defaults) */
    unsigned struct_size;   /* sizeof(asmtest_ibs_opts_t); 0=>legacy */
    unsigned period_jitter; /* jitter fraction 1/N; 0 => default     */
    unsigned reserved[3];   /* reserved (0)                          */
} asmtest_ibs_opts_t;

/* Zero-initialise + self-describe: `asmtest_ibs_opts_t o = ASMTEST_IBS_OPTS_INIT;`
 * sets struct_size so the backend honours the appended fields (see struct_size). */
#define ASMTEST_IBS_OPTS_INIT                                                  \
    { .struct_size = (unsigned)sizeof(asmtest_ibs_opts_t) }

/* 1 iff this is a Linux/x86-64 AMD host whose IBS-Op supports op sampling (OpSam)
 * and branch target (BrnTrgt), the ibs_op PMU is present, and the kernel exposes
 * the `swfilt` software-filter bit (so user-only sampling opens unprivileged at
 * perf_event_paranoid=2). Cached; safe to call repeatedly. */
int asmtest_ibs_available(void);

/* A human-readable reason asmtest_ibs_available() returned 0 (a static string;
 * empty string when available). e.g. "not AMD", "no ibs_op PMU",
 * "no swfilt (kernel < ~6.2)", "no IBS branch target (BrnTrgt)".
 *
 * NOTE this answers "why is the SUBSTRATE absent" — CPUID + sysfs only. It does
 * NOT mean sampling will work: asmtest_ibs_available() never calls
 * perf_event_open. For "why did a capture return EUNAVAIL", which is the
 * question an operator is actually asking, use asmtest_ibs_unavail_reason(). */
const char *asmtest_ibs_skip_reason(void);

/* A human-readable reason the LAST capture attempt returned ASMTEST_IBS_EUNAVAIL
 * (a static string; empty when nothing has failed). Unlike skip_reason() this
 * reports the real perf_event_open failure, because "the substrate is present but
 * perf refused us" is the interesting case and skip_reason() returns "" on it BY
 * CONSTRUCTION (it answers only the detect chain, which passed). Distinguishes
 * EACCES (perf_event_paranoid / needs CAP_PERFMON) from EINVAL, EMFILE, ENOENT
 * and ESRCH, which previously all collapsed into one indistinguishable code.
 * Prefer this in any user-facing skip message. */
const char *asmtest_ibs_unavail_reason(void);

/* PURE, host-independent decode of one IBS-Op PERF_SAMPLE_RAW payload into an edge.
 * `raw` points at the raw payload ([u32 caps][u64 regs...] — what follows the
 * PERF_SAMPLE_RAW size field) and `raw_len` is its byte length. No hardware and no
 * perf headers, so it compiles and is unit-tested on ANY host. Returns:
 *   ASMTEST_IBS_OK      — a retired TAKEN branch; *out is a usable edge (count = 1).
 *   ASMTEST_IBS_NOEDGE  — well-formed op, but not a retired taken branch, or the
 *                         RIP was flagged invalid: nothing to record (*out zeroed).
 *   ASMTEST_IBS_EINVAL  — NULL argument.
 *   ASMTEST_IBS_EDECODE — raw_len too short to hold the branch-target register,
 *                         or the record's own caps word records no branch target
 *                         (BrnTrgt clear), so reg[7] is not IbsBrTarget. */
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
 * system-wide per-CPU capture (the `ASMTEST_IBS_OPT_SYSTEM_WIDE` flag on
 * `asmtest_ibs_opts_t.flags` — needs CAP_PERFMON / `perf_event_paranoid<=0`).
 * Threads that vanish
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

/* --- portable software-clock IP sampler (any vendor; works in VMs) ------------- */
/* PERF_TYPE_SOFTWARE / PERF_COUNT_SW_TASK_CLOCK + PERF_SAMPLE_IP: no PMU at all,
 * so it samples where IBS-Op cannot — Intel hosts, VMs, containers — under the
 * same out-of-band, unprivileged envelope (exclude_kernel; perf_event_paranoid
 * <= 2 or CAP_PERFMON; the target runs unperturbed, no ptrace).
 *
 * WHAT IT MEASURES, honestly: a time-based IP histogram is RESIDENCY — where
 * the target SPENDS TIME — not control flow. There are no edges here and no
 * entry evidence: a function entered once that never returns (main, an event
 * loop) DOMINATES a residency ranking even though an entry breakpoint on it
 * would never fire again. A caller ranking these to pick a trace region must
 * pair the pick with a bounded entry wait and be prepared to walk candidates
 * (asmspy_autoregion_rank_ip documents the same hazard from the picker side). */
typedef struct {
    uint64_t ip;    /* sampled user-space instruction pointer */
    uint64_t count; /* samples aggregated at this exact ip    */
} asmtest_swclock_ip_t;

typedef struct {
    asmtest_swclock_ip_t *ips; /* malloc'd; DESCENDING count, ties ascending ip
                                * (deterministic run to run); free via _free  */
    size_t n;                  /* number of distinct sampled ips              */
    uint64_t samples;          /* PERF_RECORD_SAMPLEs drained (provenance)    */
    uint64_t lost;             /* samples the kernel reported dropped         */
    int throttled;             /* kernel throttled, or a ring neared overflow */
} asmtest_swclock_survey_t;

/* Sample EVERY thread of `pid` (0 => the calling process) for `ms` milliseconds
 * at `freq_hz` (0 => 997 Hz — a prime, so a periodic workload cannot hide
 * between evenly-spaced ticks) and return the aggregated user-IP histogram.
 * Threads are enumerated from /proc/<pid>/task with one mid-window rescan,
 * exactly like asmtest_ibs_survey_process.
 *
 * Availability is probed BY DOING — there is no substrate to detect, and a
 * probe that does not open reports "available" on precisely the hosts where no
 * capture can open (the --sample lesson). Returns ASMTEST_IBS_OK,
 * ASMTEST_IBS_EINVAL on bad args, or ASMTEST_IBS_EUNAVAIL when no thread's
 * event could open (or off Linux) — asmtest_swclock_unavail_reason() then
 * carries the real errno. *out is always zero-initialised first. */
int asmtest_swclock_survey_process(pid_t pid, unsigned ms, unsigned freq_hz,
                                   asmtest_swclock_survey_t *out);

/* Free the ip array a survey owns and zero the struct (idempotent; NULL-safe). */
void asmtest_swclock_survey_free(asmtest_swclock_survey_t *s);

/* The errno-carrying reason the LAST asmtest_swclock_survey_process returned
 * EUNAVAIL ("" when none has failed): EACCES/EPERM name perf_event_paranoid /
 * CAP_PERFMON / seccomp — the operator's one-liner, mirroring
 * asmtest_ibs_unavail_reason. A static string; do not free. */
const char *asmtest_swclock_unavail_reason(void);

/* One normalized basic-block leader lifted from the edge stream: a block-start
 * offset plus how many sampled edges entered it. STATISTICAL — it marks a block that
 * WAS seen entered, never proves one was not (see asmtest_ibs_normalize_blocks). */
typedef struct {
    uint64_t
        start; /* block-start (a branch TARGET). Region-relative offset when
                       * normalized against a region, else the absolute address.   */
    uint64_t
        entries; /* sampled edges that landed here (statistical hit count)     */
} asmtest_ibs_block_t;

/* A normalized basic-block set: distinct block starts sorted ASCENDING by `start`. */
typedef struct {
    asmtest_ibs_block_t
        *blocks; /* malloc'd; free via asmtest_ibs_blocks_free */
    size_t n;    /* number of distinct block starts           */
} asmtest_ibs_blocks_t;

/* PURE, host-independent: normalize a survey's from->to edge stream into the set of
 * DISTINCT basic-block starts it observed, the SAME branch-edge convention the exact
 * AMD-LBR / native tiers use — every branch TARGET begins a block (the leader the
 * exact tiers record with trace_append_block(to - base_ip)). Duplicate targets (a
 * block reached by several sampled edges) merge, summing their `entries`.
 *
 * If `len != 0` the result is filtered to the region [base, base+len) and reported as
 * region-relative OFFSETS (offset 0 = base, the routine entry), so the block offsets
 * LINE UP with the exact tiers' blocks[] for the same routine; if `len == 0` every
 * target is kept and reported as an absolute address (`base` is then ignored).
 *
 * STATISTICAL only: it marks blocks SEEN entered, never proves a block was NOT
 * executed, and (being Capstone-free) it recovers only branch-target leaders, not the
 * fall-through leaders after a not-taken branch — a strict subset of the exact block
 * set, never a superset. Out of the exact insns[]/blocks[] parity contract. No
 * hardware and no perf headers, so it compiles and is unit-tested on ANY host.
 * Returns ASMTEST_IBS_OK (even with zero blocks), ASMTEST_IBS_EINVAL on a NULL
 * argument, or ASMTEST_IBS_EUNAVAIL if the block array cannot be allocated. *out is
 * always zero-initialised first (free it with asmtest_ibs_blocks_free). */
int asmtest_ibs_normalize_blocks(const asmtest_ibs_survey_t *survey,
                                 uint64_t base, uint64_t len,
                                 asmtest_ibs_blocks_t *out);

/* Free the block array a normalization owns and zero the struct (idempotent;
 * NULL-safe). Pure/host-independent. */
void asmtest_ibs_blocks_free(asmtest_ibs_blocks_t *b);

#ifdef __cplusplus
}
#endif
#endif /* ASMTEST_IBS_H */
