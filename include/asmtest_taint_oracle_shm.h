/*
 * asmtest_taint_oracle_shm.h — the cross-address-space results channel for the libdft64
 * DIFFERENTIAL ORACLE lane (pin-libdft-taint-oracle.md). Under `pin -t oracle.so --
 * pin_taint <mode>`, the fixture driver (examples/pin_taint.c) and the libdft64 Pintool
 * (pintools/libdft_oracle/oracle.cpp) share one address space, but the DIFF validator
 * (examples/taint_oracle_diff.c) that drains + oracle-diffs the sink set against the DR
 * client is a SEPARATE process — so the sink report is backed by a POSIX shared-memory
 * segment all three map. This header is the fixed layout they agree on.
 *
 * CROSS-PROCESS RULE (mirrors asmtest_taint_shm.h): the segment is mapped at DIFFERENT
 * virtual addresses in the three processes, so the stored POINTER at_taint_report_t.hits
 * is a producer-space (Pintool) address and is MEANINGLESS to a consumer. The consumer
 * reads ONLY by OFFSET: the fixed hits[] array + the scalar counters (hits_len /
 * hits_total / truncated), never the .hits pointer. The Pintool still fills via the
 * pointer (valid in its own space), which is the same physical memory read by offset.
 *
 * libdft produces no value trace, so — unlike asmtest_taint_shm.h — no embedded
 * at_drval_t / steps[] ride the segment; only the sink report does. The channel is
 * created + zeroed by whoever runs first (the diff orchestrator, or pin_taint standalone);
 * the Pintool ATTACHES lazily (never creates), so it is robust to the tool's main()
 * running before the app creates the segment.
 *
 * <stdint.h>-only (via asmtest_taint.h) so the C driver/validator and the C++ Pintool
 * agree on the layout byte-for-byte.
 */
#ifndef ASMTEST_TAINT_ORACLE_SHM_H
#define ASMTEST_TAINT_ORACLE_SHM_H

#include <stdint.h>

#include "asmtest_taint.h"

/* Default segment name (a POSIX shm name: leading slash, no other slashes). The driver,
 * tool, and validator all take an override (pin_taint argv / the tool's -shm knob); this
 * is the fallback all three default to. */
#define AT_ORACLE_SHM_NAME "/asmtest_taint_oracle"
#define AT_ORACLE_HITS_CAP 16

typedef struct at_oracle_shm {
    volatile uint32_t
        done; /* 0 until the fixture returns; then 1 (release/acquire) */
    uint32_t pad0;
    int64_t
        result; /* the fixture's return value (a liveness sanity)          */
    uint64_t
        region_base; /* fixture region base, so the tool reports region offsets */
    uint64_t
        region_len; /* fixture region length, so the tool bounds in-region     */
    /* The Pintool appends hits + bumps the counters here. report.hits points at hits[]
     * below in PRODUCER (Pintool) space; the consumer ignores that pointer and reads
     * hits[] + the counters by offset, with the same append/truncate discipline as
     * at_taint_report_t (fixed cap, `truncated` on overflow, hits_total keeps counting). */
    at_taint_report_t report;
    at_taint_hit_t hits[AT_ORACLE_HITS_CAP];
} at_oracle_shm_t;

#endif /* ASMTEST_TAINT_ORACLE_SHM_H */
