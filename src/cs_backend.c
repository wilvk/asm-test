/*
 * cs_backend.c — ARM CoreSight decode backend for the hardware-trace tier
 * (OpenCSD). See asmtest_hwtrace.h and docs/native-tracing.md.
 *
 * CoreSight ETM/ETE waypoint trace is captured (perf cs_etm AUX, by hwtrace.c)
 * and deformatted + decoded by OpenCSD against asm-test's registered code bytes,
 * yielding the same instruction-range stream the Intel PT backend produces, which
 * is then normalized into Unicorn/DynamoRIO-equivalent basic blocks exactly as in
 * pt_backend.c.
 *
 * STATUS: scaffold. The PT backend is fully implemented; CoreSight decode is
 * structured but reports the decoder absent until completed on a real AArch64
 * CoreSight board (the plan gates this tier on board access). Because
 * asmtest_cs_decoder_present() returns 0, asmtest_hwtrace_available(CORESIGHT)
 * self-skips on every host — including AArch64 — until the OpenCSD decode tree
 * below is finished and this returns 1.
 *
 * Completing it (OpenCSD C API, opencsd/c_api/opencsd_c_api.h):
 *   1. ocsd_create_dcd_tree(OCSD_TRC_SRC_FRAME_FORMATTED, ...) for the formatted
 *      cs_etm AUX stream;
 *   2. create an ETMv4 (ETE) instruction decoder on the tree with the trace ID
 *      from perf's AUXTRACE_INFO, registering a custom memory accessor that
 *      serves [base, base+len) from the registered bytes;
 *   3. register an element-decode callback that, on OCSD_GEN_TRC_ELEM_INSTR_RANGE,
 *      walks each instruction offset, recording insns[] and marking a block start
 *      after every branch (identical normalization to pt_backend.c);
 *   4. ocsd_dt_process_data() over the AUX bytes, then ocsd_destroy_dcd_tree().
 */
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#define ASMTEST_HW_OK 0
#define ASMTEST_HW_ENOSYS (-5)

/* Returns 0 until the OpenCSD decode tree above is implemented and validated on
 * a CoreSight board; this keeps asmtest_hwtrace_available(CORESIGHT) self-skipping
 * rather than advertising a backend that cannot yet decode. */
int asmtest_cs_decoder_present(void) { return 0; }

int asmtest_cs_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace) {
    (void)aux;
    (void)aux_len;
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}
