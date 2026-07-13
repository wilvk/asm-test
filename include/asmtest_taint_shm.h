/*
 * asmtest_taint_shm.h — the cross-address-space results channel for the DynamoRIO taint
 * tier's LAUNCH-under-DR path (dynorio-taint-tier-plan.md, Increment 5). Under
 * `drrun -c <taint client>.so -- <workload>`, the launched workload + the DR client share
 * one address space, but the out-of-process VALIDATOR that drains and oracle-diffs the
 * results is a SEPARATE process — so the sink report is backed by a POSIX shared-memory
 * segment both map. This header is the fixed layout they agree on.
 *
 * CROSS-PROCESS RULE: the segment is mapped at DIFFERENT virtual addresses in the two
 * processes, so any stored POINTER (e.g. at_taint_report_t.hits) is a producer-space
 * address and is MEANINGLESS to the consumer. The consumer reads only by OFFSET: the
 * fixed hits[] array and the scalar counters (hits_len / hits_total / truncated), never
 * the .hits pointer. The producer still fills via the pointer (valid in its own space),
 * which is the same physical memory the consumer reads by offset.
 *
 * Two channels ride the segment, drained at different times:
 *  - The SINK report (hits) is written SYNCHRONOUSLY by the sink clean call, so it is
 *    present the instant the fixture returns.
 *  - The VALUE / TAINT trace (at_drval_t + its steps[] + step_taint[]) is drx_buf-
 *    buffered and flushes at PROCESS EXIT (in the client's exit event, after the
 *    workload's main returns), so it is complete only once the launched process has
 *    fully exited — i.e. after `drrun` returns. The validator therefore runs AFTER drrun
 *    (the Makefile sequences them) and does NOT rely on the done flag (set in main,
 *    before the flush) for the value trace.
 *
 * Because the embedded at_drval_t layout depends on -DASMTEST_TAINT (it gains the
 * step_taint fields), the workload AND the validator both compile -DASMTEST_TAINT -Isrc
 * so they agree on this struct. The client fills steps[]/step_taint[] via at_drval_t
 * pointers that are PRODUCER-space addresses into this same segment; the consumer reads
 * steps[]/step_taint[] + the scalar drval.steps_len by OFFSET, never the pointers.
 */
#ifndef ASMTEST_TAINT_SHM_H
#define ASMTEST_TAINT_SHM_H

#include <stdint.h>

#include "asmtest_taint.h"

#include "dataflow_dr.h" /* at_drval_t / at_vstep_t (compile the harness with -Isrc) */

/* Default segment name (a POSIX shm name: leading slash, no other slashes). The workload
 * and validator take an override as argv[1]; this is the fallback both default to. */
#define AT_SHM_NAME      "/asmtest_taint_launch"
#define AT_SHM_HITS_CAP  16
#define AT_SHM_STEPS_CAP 64

/* The shared channel. done transitions 0->1 (release) once the workload's fixture has
 * returned and every synchronous sink hit is in place; the validator spins on it
 * (acquire) before draining. */
typedef struct at_shm_channel {
    volatile uint32_t
        done; /* 0 until the workload finishes; then 1 (release/acquire) */
    uint32_t pad0;
    int64_t
        result; /* the fixture's return value (a liveness sanity)          */
    /* The client registers &report at the sink marker and appends hits + bumps the
     * counters here. report.hits points at hits[] below in PRODUCER space; the consumer
     * ignores that pointer and reads hits[] + the counters by offset. */
    at_taint_report_t report;
    at_taint_hit_t hits[AT_SHM_HITS_CAP];
    /* Value / taint trace, drained by the client's drx_buf flush at process exit. The
     * workload registers &drval at the region marker with drval.steps -> steps[] and
     * drval.step_taint -> step_taint[] (mem left NULL — the taint SET needs no values);
     * the consumer reads drval.steps_len + steps[].off + step_taint[] by offset. */
    at_drval_t drval;
    at_vstep_t steps[AT_SHM_STEPS_CAP];
    at_tag_t step_taint[AT_SHM_STEPS_CAP];
} at_shm_channel_t;

#endif /* ASMTEST_TAINT_SHM_H */
