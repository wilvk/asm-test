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
 * Dependency-free beyond the taint ABI (<stdint.h> only), so the standalone workload +
 * validator can both include it. This increment carries the SINK report only; the value
 * trace is drx_buf-buffered and flushes at process exit (after the workload's main
 * returns), so it is not reliably readable mid-run — a later slice adds a process-exit
 * drain for the full trace. The sink hits are written SYNCHRONOUSLY by the sink clean
 * call, so they are present in the segment the instant the fixture returns.
 */
#ifndef ASMTEST_TAINT_SHM_H
#define ASMTEST_TAINT_SHM_H

#include <stdint.h>

#include "asmtest_taint.h"

/* Default segment name (a POSIX shm name: leading slash, no other slashes). The workload
 * and validator take an override as argv[1]; this is the fallback both default to. */
#define AT_SHM_NAME     "/asmtest_taint_launch"
#define AT_SHM_HITS_CAP 16

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
} at_shm_channel_t;

#endif /* ASMTEST_TAINT_SHM_H */
