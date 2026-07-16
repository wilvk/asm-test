/*
 * gcfence_shm.h — the PROBE-LOCAL shm channel for the F4 GC-fence FREEZE measurement
 * (docs/internal/plans/live-attach-dataflow-followup-plan.md F4; findings in
 * docs/internal/analysis/f4-gc-fence-freeze-probe-findings.md).
 *
 * F4 stamps each MovedReferences2 {old,new,len} triple with an `asmtest_gcmove_t.step` — an index
 * into the value trace's insn_off[], i.e. HOW MANY in-region instructions the tracer has recorded
 * so far. The design's load-bearing claim is that a GC fence suspends the EE so completely that a
 * ptrace-single-stepped managed thread retires ZERO instructions across it, so the step counter is
 * frozen and can simply be read AT DRAIN TIME. This channel exists to MEASURE that, not assume it:
 * the tracer publishes its live step counter here, and the profiler — running on the GC thread at
 * both ends of the fence — samples it into a record: S0 at GarbageCollectionStarted, S1 at
 * GarbageCollectionFinished. S1 - S0 is the answer.
 *
 * Modelled on the DR tier's include/asmtest_taint_gcmove.h handshake, but deliberately SEPARATE and
 * probe-local: this tier is out-of-process (tracer and profiler are in DIFFERENT processes, so the
 * DR tier's "publish a function pointer, we share an address space" trick does not apply), it has
 * no shm channel today, and the DR tier's shipping header must not be perturbed by a probe.
 *
 * Both sides open with O_CREAT and ftruncate to sizeof(gcfence_channel_t); whoever loses the race
 * finds it already the right size. Everything here is plain integers — the profiler touches this
 * from inside a GC callback where the EE is suspended, so it must not allocate, lock, or call back
 * into the runtime.
 */
#ifndef GCFENCE_SHM_H
#define GCFENCE_SHM_H

#include <stdint.h>

#define GCFENCE_SHM_NAME "/asmtest_gcfence_probe"
#define GCFENCE_MAGIC 0x47464E43u /* "GFNC" — the tracer is live and stepping */
#define GCFENCE_MAX_RECS 512

/* Two DIFFERENT windows are measured, because they are not the same window and conflating them is
 * exactly how one would get this question wrong:
 *
 *  GCFENCE_KIND_GC      GarbageCollectionStarted .. GarbageCollectionFinished — the GC as the
 *                       profiler sees it. This is the window F4's "read the step counter at drain
 *                       time" would be stamping against, so it is the one the design's claim is
 *                       really about.
 *  GCFENCE_KIND_SUSPEND RuntimeSuspendFinished .. RuntimeResumeStarted — the window in which the EE
 *                       is ACTUALLY, fully suspended (COR_PRF_MONITOR_SUSPENDS, likewise allowed
 *                       after attach). "The EE is fully suspended" is the literal wording of the
 *                       assumption, so it gets measured literally rather than by proxy.
 */
#define GCFENCE_KIND_GC 0u
#define GCFENCE_KIND_SUSPEND 1u

/* One observed window, appended by the profiler when the window closes. */
typedef struct gcfence_rec {
    uint32_t kind;       /* GCFENCE_KIND_*                                      */
    uint32_t seq;        /* 1-based sequence within its kind                    */
    uint32_t gens;       /* GC: bitmask of generations collected (bit3=LOH,4=POH)*/
    uint32_t reason;     /* GC: COR_PRF_GC_REASON / SUSPEND: COR_PRF_SUSPEND_REASON */
    uint32_t traced;     /* 1 if the tracer was live+stepping when it OPENED    */
    uint32_t traced_close; /* 1 if it was STILL stepping when it CLOSED. BOTH   */
                         /*   must be 1 for s1-s0 to mean anything: a window    */
                         /*   that closes after the tracer let go reads a DEAD  */
                         /*   counter at both ends and reports a FALSE zero.    */
    uint32_t s0;         /* step counter when the window OPENED  (GC: Started;  */
                         /*   SUSPEND: RuntimeSuspendFinished, i.e. EE parked)  */
    uint32_t s1;         /* step counter when it CLOSED (GC: Finished; SUSPEND: */
                         /*   RuntimeResumeStarted). s1 - s0 is THE ANSWER.     */
    uint32_t s_pre;      /* SUSPEND: counter at RuntimeSuspendStarted (before   */
                         /*   the EE is parked) — separates "waiting to park a  */
                         /*   thread" from "parked"                             */
    uint32_t s_move;     /* GC: counter at the FIRST MovedReferences2 (the      */
                         /*   actual compaction point), else s0                 */
    uint32_t moved_calls;   /* MovedReferences2 calls in this GC                */
    uint32_t moved_ranges;  /* ranges delivered in this GC                      */
    uint32_t reloc_ranges;  /* of those, ranges that ACTUALLY moved (old!=new)  */
    uint32_t pad;
    uint64_t old0, new0, len0; /* a sample relocating range, 0 if none          */
    uint64_t t0_ns, t1_ns;     /* CLOCK_MONOTONIC at the two ends               */
} gcfence_rec_t;

typedef struct gcfence_channel {
    volatile uint32_t magic;        /* GCFENCE_MAGIC once the tracer is stepping */
    volatile uint32_t step_counter; /* tracer: +1 per COMPLETED PTRACE_SINGLESTEP */
    volatile uint32_t fence_active; /* profiler: 1 inside a GC (Started..Finished) */
    volatile uint32_t ee_suspended;  /* profiler: 1 while the EE is FULLY parked   */
    volatile uint32_t nrec;         /* profiler: # of valid recs                  */
    volatile uint32_t traced_tid;   /* tracer: the managed worker TID it steps    */
    volatile uint32_t tracer_done;  /* tracer: 1 once detached for good           */
    volatile uint32_t gcs_seen;     /* profiler: total GC windows (incl. overflow) */
    volatile uint32_t susp_seen;    /* profiler: total EE-suspend windows          */
    volatile uint32_t pad;
    gcfence_rec_t recs[GCFENCE_MAX_RECS];
} gcfence_channel_t;

#endif /* GCFENCE_SHM_H */
