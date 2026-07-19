/*
 * pintool_shm.h — the fixed POSIX shared-memory layout for the XED-decoded Pin
 * trace tier (PIN-2). The Pintool (asmtest_pintool.cpp, runs under PinCRT inside
 * the traced process) WRITES the recorded offset trace here; an out-of-process
 * validator (examples/pin_trace_validator.c, a normal host binary run after `pin`
 * exits) READS it back and diffs against the single-step / DynamoRIO backends.
 *
 * Deliberately NOT under include/: PIN-2 ships no public header (test-lane only,
 * see licenses/README.md). Compiled with -Ipintool by the fixture/validator and
 * via the kit's -I. by the tool.
 *
 * CROSS-PROCESS CONTRACT (mirrors include/asmtest_taint_shm.h): every array here
 * holds OFFSETS from region_base, never pointers — the tool and the validator do
 * NOT share an address space, so a stored producer-space pointer would be
 * meaningless to the reader. Consumers read by offset only. Plain C99, C++-safe,
 * <stdint.h> only, so both the C fixtures and the C++ tool include it unchanged.
 */
#ifndef ASMTEST_PINTOOL_SHM_H
#define ASMTEST_PINTOOL_SHM_H

#include <stdint.h>

#define PIN_SHM_NAME       "/asmtest_pin_trace"
#define PIN_SHM_INSNS_CAP  256
#define PIN_SHM_BLOCKS_CAP 64
#define PIN_SHM_MAGIC      0x50494E54u /* "PINT": the tool checks the layout */

typedef struct asmtest_pin_channel {
    uint32_t magic;         /* PIN_SHM_MAGIC — tool validates before writing  */
    volatile uint32_t done; /* fixture sets 1 after the markers (release)     */
    uint64_t region_base;   /* fixture-space VA of the traced routine         */
    uint64_t region_len;    /* its byte length                                */
    char region_name[64];   /* NUL-terminated region label                    */
    int64_t result;         /* r1*1000 + r2 liveness check (see the workload) */

    /* Ordered instruction offsets, in execution order. Mirrors asmtest_trace_t's
     * insns[] append/truncate discipline: record while insns_len < CAP, always
     * bump insns_total, set truncated on overflow. */
    uint64_t insns[PIN_SHM_INSNS_CAP];
    uint64_t insns_len;   /* entries written to insns[] (<= INSNS_CAP)        */
    uint64_t insns_total; /* instructions executed (counts past the cap)      */

    /* DISTINCT block-start offsets, de-duplicated, first-entry order. Mirrors
     * trace_append_block: linear-scan dedup, record if new, always bump total. */
    uint64_t blocks[PIN_SHM_BLOCKS_CAP];
    uint64_t blocks_len;   /* distinct blocks recorded (<= BLOCKS_CAP)        */
    uint64_t blocks_total; /* block entries; a loop counts each pass          */

    uint32_t truncated; /* a buffer filled and >= 1 entry was dropped         */
} asmtest_pin_channel_t;

#endif /* ASMTEST_PINTOOL_SHM_H */
