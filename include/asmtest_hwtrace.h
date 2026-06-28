/*
 * asmtest_hwtrace.h — optional hardware-assisted native tracing: Intel PT on
 * bare-metal x86-64 and ARM CoreSight on bare-metal AArch64.
 *
 * Like the DynamoRIO tier (asmtest_drtrace.h) this records native execution as
 * `asmtest_trace_t` offsets and reuses the begin/end region markers, but with
 * near-zero capture overhead: the CPU emits a compressed branch-trace packet
 * stream (Intel PT TNT/TIP/PSB, or ARM ETM/ETE waypoints) into a kernel AUX ring
 * via perf_event_open; after the region runs, a decoder (libipt for PT, OpenCSD
 * for CoreSight) replays asm-test's own registered code bytes between the branch
 * waypoints to reconstruct the ordered instruction stream and the basic blocks.
 *
 * This tier is the recommended backend for JIT/GC-heavy managed runtimes (JVM,
 * .NET, Node), where in-process DynamoRIO collides with the runtime's signal and
 * code-cache machinery — PT/ETM observe out-of-band without intercepting signals
 * or perturbing code.
 *
 * It is mostly-bare-metal-Linux and even-more-optional than the DynamoRIO tier:
 * Intel PT is Intel-x86-64-only and absent on AMD, ARM, and almost all
 * cloud/VM/CI guests; CoreSight self-hosted trace needs specific AArch64 boards.
 * asmtest_hwtrace_available() encodes the full detect-and-skip chain so callers
 * self-skip cleanly (the common case). Built only when libipt/OpenCSD are present;
 * kept out of the core libasmtest and the libasmtest_emu superset. No DynamoRIO
 * or drwrap dependency (libipt and OpenCSD are BSD).
 */
#ifndef ASMTEST_HWTRACE_H
#define ASMTEST_HWTRACE_H

#include <stddef.h>
#include <stdint.h>

#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes (shared spirit with asmtest_drtrace.h). */
#define ASMTEST_HW_OK 0
#define ASMTEST_HW_EINVAL (-1)
#define ASMTEST_HW_ESTATE (-2)
#define ASMTEST_HW_EUNAVAIL (-3) /* backend/PMU/privilege unavailable */
#define ASMTEST_HW_ENOSYS (-5)   /* decoder library not compiled in    */
#define ASMTEST_HW_EFULL (-6)
#define ASMTEST_HW_EDECODE (-8)  /* capture/decode failure             */

typedef enum {
    ASMTEST_HWTRACE_INTEL_PT = 0,
    ASMTEST_HWTRACE_CORESIGHT = 1,
    ASMTEST_HWTRACE_AMD_LBR = 2, /* AMD Zen 3 BRS / Zen 4 LbrExtV2 (16-deep) */
} asmtest_trace_backend_t;

typedef struct {
    asmtest_trace_backend_t backend;
    size_t aux_size;  /* AUX (trace) ring bytes; rounded up to 2^n pages (0=64KB) */
    size_t data_size; /* base perf ring bytes; rounded up to 2^n pages (0=8KB)    */
    int snapshot;     /* nonzero: circular snapshot ring; 0: linear (drain)       */
    const char *object_hint; /* optional object-file path for hw address filters  */
} asmtest_hwtrace_options_t;

/* The full gating chain, as a single detect-and-skip predicate: returns 1 only
 * if (a) this build links the backend's decoder, (b) the right CPU/ISA/vendor is
 * present, (c) the kernel exposes the backend's PMU (intel_pt / cs_etm), and
 * (d) perf_event capture is permitted for this process. Returns 0 (self-skip)
 * otherwise — the common case off bare metal. */
int asmtest_hwtrace_available(asmtest_trace_backend_t backend);

/* A human-readable reason asmtest_hwtrace_available() returned 0, into buf
 * (always NUL-terminated). Useful for the self-skip message. */
void asmtest_hwtrace_skip_reason(asmtest_trace_backend_t backend, char *buf,
                                 size_t buflen);

int asmtest_hwtrace_init(const asmtest_hwtrace_options_t *opts);
int asmtest_hwtrace_register_region(const char *name, void *base, size_t len,
                                    asmtest_trace_t *trace);
void asmtest_hwtrace_shutdown(void);

/* Region markers, reused from the native-trace model. begin(name) enables the
 * hardware AUX capture for the named region; end(name) disables it and decodes
 * the captured packets into the registered trace. Backend-neutral by name (no
 * drwrap needed for this tier).
 *
 * MVP limitation: capture state is a single process-global slot — only ONE region
 * may be active at a time (a begin while another is active is ignored), so these
 * are NOT yet per-thread. Bracket one registered routine per begin/end pair. */
void asmtest_hwtrace_begin(const char *name);
void asmtest_hwtrace_end(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_HWTRACE_H */
