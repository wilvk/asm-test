/*
 * gccanon_shm.h — the PROBE-LOCAL shm channel for F4 increment 1: live GC-move canonicalization
 * on the ptrace live-attach tier (docs/internal/plans/live-attach-dataflow-followup-plan.md F4).
 *
 * Carries the two halves of the join in opposite directions across a process boundary:
 *
 *   tracer -> profiler   `step_counter`, a live mirror of the L0 value trace's steps_len — exactly
 *                        what asmtest_gcmove_t.step indexes. The profiler samples it as S0 at
 *                        GarbageCollectionStarted, from INSIDE the fence.
 *   profiler -> tracer   the MovedReferences2 {old,new,len} triples, each already STAMPED with the
 *                        S0 of its GC, ready to become asmtest_gcmove_t records.
 *
 * S0 stamping, not drain-time counting, is a MEASURED requirement, not a preference:
 * f4-gc-fence-freeze-probe-findings.md found the freeze assumption FALSE on 47/47 fences (342-558
 * instructions retired across a fence), because single-stepping a futex-blocked thread is what
 * un-blocks it. The tracer's counter cannot be read after the fact and be exact.
 *
 * Modelled on examples/gcfence_probe/gcfence_shm.h and, behind it, the DR tier's
 * include/asmtest_taint_gcmove.h — but necessarily separate from BOTH. The DR tier is in-process
 * with its target (it can publish a function pointer and remap a live shadow AT the fence); this
 * tier is out-of-process and post-pass, so the triples must travel as data and be applied to a
 * captured trace before asmtest_defuse_build. The DR tier's shipping header is untouched, and this
 * one is deliberately probe-local: it is a lane's channel, not an API.
 *
 * Both sides open O_CREAT + ftruncate to sizeof(gccanon_channel_t); whoever loses the race finds it
 * already the right size. Everything here is plain integers — the profiler writes it from inside a
 * GC callback with the EE suspended, where it must not allocate, lock, or re-enter the runtime.
 */
#ifndef GCCANON_SHM_H
#define GCCANON_SHM_H

#include <stdint.h>

#define GCCANON_SHM_NAME "/asmtest_gccanon_attach"
#define GCCANON_MAGIC 0x47434E43u /* "GCNC" — the tracer is live and stepping the region */

/* One GC's batch can be large (the attach probe measured ~500 ranges per compacting gen2 GC, and
 * 160,032 relocating ranges over 320 GCs). Sized to hold several whole batches; overflow is
 * reported honestly via moves_total rather than silently dropped, because a dropped range is
 * precisely a missed relocation. ~2 MB of shm. */
#define GCCANON_MAX_MOVES 65536

/* One relocating range, already stamped. Mirrors asmtest_gcmove_t {old_base,new_base,len,step} plus
 * the provenance the tier needs to police its own model. */
typedef struct gccanon_move {
    uint64_t old_base;
    uint64_t new_base;
    uint64_t len;
    uint32_t step;   /* the profiler-sampled S0 of the GC that delivered it   */
    uint32_t gc_seq; /* WHICH GC — load-bearing, see below                    */
} gccanon_move_t;

/*
 * `gc_seq` exists to catch a real modelling hazard rather than to decorate the record.
 *
 * asmtest_gcmove_canon treats moves that share a `step` as ONE batch and applies AT MOST ONE
 * relocation from it, because a batch models a single GCBulkMovedObjectRanges whose old ranges are
 * disjoint. But this tier's step counter is REGION-GATED: it does not advance while the traced
 * thread is off in the region's call-out, which is exactly where the GC lands. So two GCs in one
 * call-out window are stamped with the SAME S0 and collapse into one batch — and an object moved
 * twice (A->B->C) would canonicalize to B while the load reads C. The edge would go missing and the
 * failure would look like a transform bug.
 *
 * Increment 1 choreographed exactly one GC per window and used gc_seq to CHECK that rather than
 * assume it. Increment 2 PROVOKES the collapse (GCCANON_GCS_PER_WINDOW > 1) and fixes it: gc_seq is
 * now also what orders a window's GCs so their moves can be CHAINED (A->C) before the unmodified
 * transform ever sees them. It is load-bearing in both directions — a detector, then an input.
 */

/* One traced GC's fence, as the profiler saw it. THE EVIDENCE FOR CHAINING: chaining a window's GCs
 * is only sound because no trace record can lie BETWEEN two of them, and these are the numbers that
 * say so — the LIVE trace's steps_len and recs_len, sampled by the profiler from inside the fence at
 * GarbageCollectionStarted and again at GarbageCollectionFinished. If a GC's four samples and its
 * neighbours' all agree, then nothing was appended across the fences OR the gaps between them: every
 * record is before ALL the window's moves or after ALL of them.
 *
 * Recorded per GC, keyed by seq, rather than min/maxed by the tracer's mirror thread, because a
 * mirror cannot tell WHICH fence it is looking at: the victim's driver also collects OUTSIDE the
 * window (to rebuild fragmentation), and one of those landing while the producer was still stepping
 * would poison a global min/max with a perfectly legitimate counter advance. The tracer selects the
 * window's GCs by the gc_seq its move ranges carry, so only the fences under test are examined.
 *
 * The counters travel through the 20 us mirror, so a sample is a LOWER bound on the truth — which is
 * why this is corroboration, not the whole argument. The structural fact is that src/dataflow_ptrace.c
 * runs the region's call-out via int3 + PTRACE_CONT and records NOTHING over the helper, so for the
 * entire window the producer is blocked in waitpid and CANNOT append. */
typedef struct gccanon_gcinfo {
    uint32_t seq;                    /* which GC (matches gccanon_move_t.gc_seq)            */
    uint32_t s0_steps, s0_recs;      /* the live trace's counters at GC START                */
    uint32_t s1_steps, s1_recs;      /* ... and at GC FINISH                                 */
} gccanon_gcinfo_t;

#define GCCANON_MAX_GCINFO 64 /* only TRACED GCs are recorded: a capture sees a handful */

typedef struct gccanon_channel {
    volatile uint32_t magic;         /* tracer: GCCANON_MAGIC while stepping the region     */
    volatile uint32_t step_counter;  /* tracer: live mirror of the value trace's steps_len   */
    volatile uint32_t recs_counter;  /* tracer: live mirror of the value trace's recs_len    */
    volatile uint32_t tracer_done;   /* tracer: 1 once the capture is over                   */
    volatile uint32_t fence_active;  /* profiler: 1 inside a GC (Started..Finished)          */
    volatile uint32_t gcs_seen;      /* profiler: GC windows observed at all                 */
    volatile uint32_t gcs_traced;    /* profiler: GC windows that opened with a LIVE counter */
    volatile uint32_t nmoves;        /* profiler: valid entries in moves[]                   */
    volatile uint32_t moves_total;   /* profiler: relocating ranges SEEN (may exceed nmoves) */
    volatile uint32_t nonreloc_total;/* profiler: old == new ranges — see below              */
    volatile uint32_t last_s0;       /* profiler: S0 of the most recent traced GC            */
    volatile uint32_t ngcinfo;       /* profiler: valid entries in gcs[]                     */
    gccanon_gcinfo_t gcs[GCCANON_MAX_GCINFO];
    gccanon_move_t moves[GCCANON_MAX_MOVES];
} gccanon_channel_t;

/*
 * `nonreloc_total` is counted separately for the reason that has now bitten both prior F4 probes:
 * MovedReferences2 legitimately reports NON-relocating ranges (old == new — a compaction can leave
 * a segment head in place). "Ranges were delivered" is therefore a VACUOUS assertion; the transform
 * exists precisely to follow objects whose address CHANGED. Only old != new ranges are recorded in
 * moves[], and the count of the others is kept so the distinction stays visible.
 */

#endif /* GCCANON_SHM_H */
