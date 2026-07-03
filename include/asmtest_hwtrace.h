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
 * The hardware-trace backends are mostly-bare-metal-Linux and even-more-optional
 * than the DynamoRIO tier: Intel PT is Intel-x86-64-only and absent on AMD, ARM,
 * and almost all cloud/VM/CI guests; CoreSight self-hosted trace needs specific
 * AArch64 boards. The SINGLESTEP backend is the universal fallback: it drives the
 * x86 EFLAGS.TF single-step debug exception (#DB -> SIGTRAP) to record every
 * executed RIP, so it produces the SAME exact/complete asmtest_trace_t offsets on
 * ANY x86-64 Linux host (Intel, AMD of any Zen, VM, CI, container) with no PMU, no
 * perf_event, no privilege, and no decoder library (only the existing Capstone
 * length-decoder, for block normalization). asmtest_hwtrace_available() encodes
 * the full detect-and-skip chain per backend so callers self-skip cleanly. The
 * PT/CoreSight backends build only when libipt/OpenCSD are present; the AMD and
 * single-step backends need no extra library. Kept out of the core libasmtest and
 * the libasmtest_emu superset. No DynamoRIO or drwrap dependency (libipt and
 * OpenCSD are BSD).
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
#define ASMTEST_HW_OK       0
#define ASMTEST_HW_EINVAL   (-1)
#define ASMTEST_HW_ESTATE   (-2)
#define ASMTEST_HW_EUNAVAIL (-3) /* backend/PMU/privilege unavailable */
#define ASMTEST_HW_ENOSYS   (-5) /* decoder library not compiled in    */
#define ASMTEST_HW_EFULL    (-6)
#define ASMTEST_HW_EDECODE  (-8) /* capture/decode failure             */

typedef enum {
    ASMTEST_HWTRACE_INTEL_PT = 0,
    ASMTEST_HWTRACE_CORESIGHT = 1,
    ASMTEST_HWTRACE_AMD_LBR = 2, /* AMD Zen 3 BRS / Zen 4 LbrExtV2 (16-deep) */
    ASMTEST_HWTRACE_SINGLESTEP =
        3, /* EFLAGS.TF #DB single-step: any x86-64 Linux,
                                       no PMU/perf/privilege/decoder; exact + complete */
} asmtest_trace_backend_t;

typedef struct {
    asmtest_trace_backend_t backend;
    size_t
        aux_size; /* AUX (trace) ring bytes; rounded up to 2^n pages (0=64KB) */
    size_t data_size; /* base perf ring bytes; 2^n pages (0=8KB; the AMD backend
                         floors it at 64KB and it bounds the Tier-B stitched run) */
    int snapshot; /* nonzero: circular snapshot ring; 0: linear (drain)       */
    const char
        *object_hint; /* optional object-file path for hw address filters  */
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

/* ------------------------------------------------------------------ */
/* Backend auto-selection (the hardware-tier fallback cascade)         */
/*                                                                     */
/* Every backend above fills the same asmtest_trace_t (one offset basis, */
/* one block partition) and self-skips cleanly via asmtest_hwtrace_     */
/* available(), so the most-faithful AVAILABLE one can be chosen without */
/* the caller hard-coding an enum. These resolve that choice; the caller */
/* still brackets its own routine with init/register/begin/end.         */
/* ------------------------------------------------------------------ */

/* Selection policy. BEST returns the most faithful available backend.
 * CEILING_FREE additionally skips the ceiling-bounded backend (AMD LBR: Tier-B
 * stitching decodes past its 16-deep stack, but capture is still bounded by the
 * data ring and PMI throttling, either of which sets trace.truncated) — so the
 * backend it returns has no such completeness ceiling. CEILING_FREE is the policy
 * to re-resolve under after a trace comes back truncated. */
typedef enum {
    ASMTEST_HWTRACE_BEST = 0,
    ASMTEST_HWTRACE_CEILING_FREE = 1,
} asmtest_hwtrace_policy_t;

/* Resolve this host's hardware-trace fallback cascade: write the AVAILABLE
 * backends, most-faithful first (Intel PT > AMD LBR > single-step > CoreSight),
 * into out[0..cap), honoring `policy`; return how many were written (0 = none
 * available — only off x86-64 Linux / a non-CoreSight host, since the single-step
 * backend covers every x86-64 Linux host). Each entry satisfies
 * asmtest_hwtrace_available(). This is the static capability cascade restricted to
 * the hardware tier: the DynamoRIO and emulator tiers are separate libraries, and a
 * fall to them stays an explicit, fidelity-aware caller decision (native->emulator
 * crosses real-CPU vs. virtual-guest execution — see docs/native-tracing.md). */
size_t asmtest_hwtrace_resolve(asmtest_hwtrace_policy_t policy,
                               asmtest_trace_backend_t *out, size_t cap);

/* The single most-preferred AVAILABLE backend under `policy`, as an int that is a
 * valid asmtest_trace_backend_t when >= 0 (init it directly) or a negative
 * ASMTEST_HW_* status (ASMTEST_HW_EUNAVAIL) when no hardware-trace backend is
 * available on this host. Dynamic-fallback idiom: run under asmtest_hwtrace_auto
 * (ASMTEST_HWTRACE_BEST); if trace.truncated afterward, re-resolve with
 * asmtest_hwtrace_auto(ASMTEST_HWTRACE_CEILING_FREE) and, when it differs, re-run. */
int asmtest_hwtrace_auto(asmtest_hwtrace_policy_t policy);

int asmtest_hwtrace_init(const asmtest_hwtrace_options_t *opts);
int asmtest_hwtrace_register_region(const char *name, void *base, size_t len,
                                    asmtest_trace_t *trace);
void asmtest_hwtrace_shutdown(void);

/* Materialize raw host-native machine code into W^X-correct executable memory
 * (mmap PROT_NONE -> mprotect RW to copy bytes in -> mprotect RX, icache flushed),
 * so a caller — notably a language binding — can register and call into it under
 * the single-step backend without rolling its own mmap/mprotect dance. On success
 * *base_out is the executable address (cast to a function pointer to call it) and
 * *len_out is its length; free with asmtest_hwtrace_exec_free. Self-contained: no
 * dependency on the DynamoRIO tier's asmtest_exec_alloc. Returns ASMTEST_HW_OK or
 * a negative status. */
int asmtest_hwtrace_exec_alloc(const void *bytes, size_t len, void **base_out,
                               size_t *len_out);
void asmtest_hwtrace_exec_free(void *base, size_t len);

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
