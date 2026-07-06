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

/* Forward declaration (defined in asmtest_codeimage.h); the §1 version-aware render
 * takes a code-image timeline. Identical typedef, so both headers may be included. */
typedef struct asmtest_codeimage asmtest_codeimage_t;

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
 * Threading (scoped-tracing-core-plan §1): the SINGLE-STEP backend is now
 * per-thread and nesting-safe — nested begin/end pairs on one thread compose
 * (innermost included in the outer) and concurrent scopes on different threads are
 * independent, via a per-thread TLS range stack. The PT / AMD LBR / CoreSight
 * backends still use a single process-global capture slot (only ONE region active at
 * a time; a begin while another is active is ignored) — the per-thread AUX-ring
 * migration is forward-look, gated on PT hardware to validate. */
void asmtest_hwtrace_begin(const char *name);
void asmtest_hwtrace_end(const char *name);

/* ------------------------------------------------------------------ */
/* Scoped-tracing shared-core primitives (scoped-tracing-core-plan §0) */
/* ------------------------------------------------------------------ */

/* §0.1 — error-returning begin. Same effect as asmtest_hwtrace_begin(name) but
 * returns 0 (ASMTEST_HW_OK) on success and a negative ASMTEST_HW_* otherwise, so a
 * scope shim gets a signal instead of a silent no-op: ASMTEST_HW_ESTATE when the
 * single process-global capture slot is already busy (the MVP no-nesting guard),
 * ASMTEST_HW_EINVAL when `name` is not a registered region, and the backend's own
 * negative status when the perf/single-step arm itself fails. asmtest_hwtrace_begin
 * is a thin wrapper that discards this result — no ABI change for the shipped shims. */
int asmtest_hwtrace_try_begin(const char *name);

/* §0.2 — the OS thread id (SYS_gettid) that armed the currently-active capture, or
 * -1 when none is active. asmtest_hwtrace_end already flags trace.truncated when it
 * runs on a different thread than begin; this accessor lets a shim assert the same
 * "single region, single thread" contract in its own idiom before it closes. */
int asmtest_hwtrace_arm_tid(void);

/* §0.3 — render a closed region's recorded instruction offsets to disassembly text.
 * Walks the named region's asmtest_trace_t insn offsets and disassembles the live
 * bytes at [base, base+len) with Capstone. snprintf semantics: writes up to
 * buflen-1 bytes plus a NUL into buf and returns the non-negative total length that
 * WOULD be written — so a caller sizes with buf=NULL, buflen=0, allocates, then
 * calls again. Returns a negative ASMTEST_HW_* code (distinct from the non-negative
 * length): ASMTEST_HW_EINVAL on a name miss or a region with no trace buffer,
 * ASMTEST_HW_ENOSYS when Capstone is not compiled in. Region-scoped and
 * version-blind: it renders whatever bytes are live at [base, len) now. A truncated
 * or over-capacity trace is rendered as a labelled PREFIX, never as complete. */
int asmtest_hwtrace_render(const char *name, char *buf, size_t buflen);

/* ------------------------------------------------------------------ */
/* §1 — per-thread handle-keyed scope API                              */
/*                                                                     */
/* try_begin/end/render (above) are name-keyed for the single-owner    */
/* path the bindings slice ships against. The handle-keyed trio here is */
/* purely ADDITIVE: it keys close/render on a per-scope handle so two   */
/* threads entering the same auto-named site concurrently each get      */
/* their own slice with no "which thread's slice" ambiguity. Backed by  */
/* the single-step per-thread range stack (host-testable); the PT/AMD   */
/* per-thread AUX path is forward-look (needs PT hardware).            */
/* ------------------------------------------------------------------ */

/* Opaque per-scope capture handle: an index into the calling thread's TLS range
 * stack, tagged with a generation counter so a stale/closed handle is rejected. */
typedef struct {
    uint32_t idx;
    uint32_t gen;
} asmtest_hwtrace_scope_t;

/* Handle-producing begin: register-then-begin under `name` (§0.4 idempotent), push a
 * range-stack frame on the calling thread, and return its handle in *out. Same
 * negative ASMTEST_HW_* returns as try_begin, plus ASMTEST_HW_EFULL when this
 * thread's range stack is full. On a non-single-step backend it falls back to the
 * name-keyed try_begin and leaves *out a sentinel. */
int asmtest_hwtrace_begin_scope(const char *name, asmtest_hwtrace_scope_t *out);

/* Handle-keyed render — the calling thread's slice for `handle`, version-blind (live
 * bytes at [base, len)). snprintf-style size-then-allocate; negative ASMTEST_HW_* on
 * a stale/unknown handle or unavailable decoder. */
int asmtest_hwtrace_render_scope(asmtest_hwtrace_scope_t handle, char *buf,
                                 size_t buflen);

/* Version-aware render — disassemble `trace`'s recorded ABSOLUTE addresses against
 * code-image `img` as of `when` (asmtest_codeimage_now), for the §D3/§D4 tiered/moved
 * managed bytes where §0.3's version-blind render would show stale text. Same
 * snprintf / negative-code convention. */
int asmtest_hwtrace_render_versioned(asmtest_codeimage_t *img, uint64_t when,
                                     const asmtest_trace_t *trace, char *buf,
                                     size_t buflen);

/* ------------------------------------------------------------------ */
/* §Z0/§Z1 — the region-free (empty-ctor) whole-window scope surface    */
/*                                                                     */
/* The aspirational `using (new AsmTrace())` form: bracket a region     */
/* with NO NativeCode and NO [base,len) and capture whatever the        */
/* calling thread executes inside it. Unlike register_region + begin,   */
/* begin_window arms with no registered region; the trace's insns[]     */
/* then hold ABSOLUTE addresses (not base-relative offsets).            */
/*                                                                     */
/* Backend reality (the honest, self-skipping envelope): the WEAK       */
/* single-step tier records every executed instruction and is           */
/* CI-runnable on any x86-64 Linux for a NATIVE LEAF (pointing          */
/* single-step at live managed code is forbidden — it fights the        */
/* runtime's SIGTRAP/JIT). The STRONG whole-window PT / CEILING AMD LBR  */
/* tiers are forward-look (bare-metal Intel PT / Zen 3+) and            */
/* begin_window self-skips to ASMTEST_HW_EUNAVAIL on them here. The      */
/* whole facility is Linux-only.                                        */
/* ------------------------------------------------------------------ */

/* Arm a region-free whole-window capture on the calling thread, recording into
 * `trace` (caller-owned; insns[] will hold ABSOLUTE addresses). Returns the scope
 * handle in *out and ASMTEST_HW_OK, or a negative status that is a clean self-skip:
 * ASMTEST_HW_EUNAVAIL on a non-single-step backend (whole-window HW trace is
 * forward-look), ASMTEST_HW_ESTATE if the tier is not up, ASMTEST_HW_EFULL if this
 * thread's range stack is full, ASMTEST_HW_EINVAL on a NULL trace. */
int asmtest_hwtrace_begin_window(asmtest_trace_t *trace,
                                 asmtest_hwtrace_scope_t *out);

/* Close a region-free scope opened by begin_window. On the ARMING thread the frame
 * resolves and is closed + normalized; on any OTHER thread (the traced work hopped
 * and Dispose ran elsewhere) the frame is invisible, so `trace` is flagged
 * `truncated` — the §Z4 thread-scope honesty default (never present a thread window
 * as a complete logical-operation trace). Returns ASMTEST_HW_OK. */
int asmtest_hwtrace_end_window(asmtest_hwtrace_scope_t handle,
                               asmtest_trace_t *trace);

/* Render a region-free scope's recorded ABSOLUTE addresses to disassembly text by
 * decoding the LIVE self-memory bytes at each address (valid for non-moving native
 * code — the WEAK tier). snprintf-style size-then-allocate; negative ASMTEST_HW_* on
 * a stale/unknown handle or unavailable decoder. Moving/managed bytes (a JIT that
 * tiers/relocates) must use asmtest_hwtrace_render_versioned against a code-image. */
int asmtest_hwtrace_render_window(asmtest_hwtrace_scope_t handle, char *buf,
                                  size_t buflen);

/* ------------------------------------------------------------------ */
/* §D4 — async-hop stitching: the shared logical-operation merge core   */
/*                                                                     */
/* When a scope follows a logical operation across await/continuation   */
/* thread hops (the opt-in stitched mode), each per-thread window is     */
/* captured as a slice, ALREADY decoded against the code-image version   */
/* live in its own window at disable time. asmtest_hwtrace_stitch is a   */
/* pure ordered CONCATENATION by `seq` — it does not decode — so it is   */
/* host-testable from synthetic pre-decoded slices with no PT hardware   */
/* and no real threads. The per-slice (tid, version) annotations ride    */
/* the companion `bounds` array (asmtest_trace_t has no field for them). */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t scope_id;     /* logical-operation id                          */
    uint32_t seq;          /* hop order within the scope                    */
    int tid;               /* thread the slice ran on                       */
    uint64_t version;      /* code-image version live in this window        */
    asmtest_trace_t trace; /* offsets ALREADY decoded against `version`     */
} asmtest_hwtrace_slice_t;

typedef struct {
    size_t insn_off; /* offset into out->insns where this slice begins */
    uint64_t scope_id;
    uint32_t seq;
    int tid;
    uint64_t version;
} asmtest_hwtrace_slice_bound_t;

/* Order `slices[0..n)` by `seq` and concatenate their instruction (and block)
 * offsets into `out` in that order, appending. Writes one `bounds` entry per slice
 * (when bounds != NULL) recording where in `out->insns` that slice begins and its
 * (scope_id, seq, tid, version); *nbounds gets the count. Returns ASMTEST_HW_OK, or
 * ASMTEST_HW_EINVAL on a NULL slices/out or n over the merge cap. A pure merge — no
 * decode — so the synchronous single-slice case is byte-identical to a plain trace. */
int asmtest_hwtrace_stitch(const asmtest_hwtrace_slice_t *slices, size_t n,
                           asmtest_trace_t *out,
                           asmtest_hwtrace_slice_bound_t *bounds, size_t *nbounds);

/* ------------------------------------------------------------------ */
/* §D3 — concealed out-of-process ptrace-stealth stepper               */
/*                                                                     */
/* The hardware-free path, hidden behind the scope façade, for hosts    */
/* with no PT/LBR (Zen 2, Docker-on-Mac). It spawns a small helper       */
/* CHILD that reverse-attaches to THIS process (prctl(PR_SET_PTRACER)    */
/* so a Yama ptrace_scope=1 host permits it, then PTRACE_SEIZE), drives  */
/* the caller to the region entry (run_to), and single-steps the region  */
/* while the caller runs it — exact, state-safe, timing-intrusive (~100- */
/* 1000x on the stepped thread while siblings run native). CI-runnable   */
/* on any ptrace-capable Linux (not hardware-gated).                    */
/* ------------------------------------------------------------------ */

/* Trace [base, base+len) — host-native code the caller has ALREADY mapped executable
 * — via a reverse-attached helper child, running the region through `run_region(arg)`
 * (the callback the caller uses to invoke it). On return `*trace` holds the recorded
 * offsets and `*result_out` (may be NULL) the region's return value. Returns
 * ASMTEST_HW_OK, ASMTEST_HW_EINVAL on a bad argument, or ASMTEST_HW_EUNAVAIL when the
 * reverse-attach is not permitted (Yama / no ptrace) — a clean self-skip. The trace
 * buffers may be ordinary heap (the helper fills a shared shadow copied back here).
 * Linux x86-64 / AArch64 only. */
int asmtest_hwtrace_stealth_trace(const void *base, size_t len,
                                  asmtest_trace_t *trace, long *result_out,
                                  void (*run_region)(void *), void *arg);

/* ------------------------------------------------------------------ */
/* AMD-P0 — deterministic software-event LBR snapshot (src/branchsnap.c) */
/* ------------------------------------------------------------------ */
/* Capture [base, base+len) via a DETERMINISTIC boundary LBR snapshot instead of the
 * sample_period=1 flood + richest-window heuristic: enable the LBR, plant a hardware
 * execution breakpoint at base+exit_off, run run_fn(arg), and when the boundary is hit a
 * BPF program calls bpf_get_branch_snapshot() to read the frozen 16-entry stack. The raw
 * branch array is decoded by the shared amd_decode (in-region-filtered). Its win over the
 * sampled path: a tiny single-shot routine — too fast to be sampled in-region — is
 * captured, not truncated. `exit_off` is the offset of the region's exit instruction (the
 * final ret / tail branch). Fills `trace`; sets trace->truncated if the boundary was
 * never reached. Returns ASMTEST_HW_OK, ASMTEST_HW_EUNAVAIL where the substrate/privilege
 * is absent (see asmtest_amd_snapshot_available), or ASMTEST_HW_ENOSYS built without the
 * BPF toolchain. Needs CAP_BPF + CAP_PERFMON + AMD LbrExtV2 + Linux >= 6.10. */
int asmtest_amd_snapshot_trace(const void *base, size_t len, size_t exit_off,
                               void (*run_fn)(void *), void *arg,
                               asmtest_trace_t *trace);

/* ------------------------------------------------------------------ */
/* §3.1(c) — whole-window noise attribution: address→name reverse       */
/* resolver + IP bucketer.                                              */
/*                                                                     */
/* The whole-window (empty-scope) PT mode captures the runtime too — the */
/* JIT compiling the method, GC, BCL plumbing. This labels every decoded */
/* IP against /proc/<pid>/maps and the perf-map so that noise is         */
/* ATTRIBUTED ("31k insns in RyuJIT, 7k in HotPath") rather than         */
/* silently mixed. The two shipped helpers return extents, not names     */
/* (asmtest_proc_region_by_addr discards the maps pathname; the perf-map  */
/* helper is a forward name→region lookup), so this is the new address→   */
/* name REVERSE resolver. Host-testable: the bucketer takes an IP list,   */
/* not a live PT capture. */
typedef struct {
    char label[128]; /* region pathname / JIT symbol (or "[anon]"/"[unknown]") */
    uint64_t count;  /* IPs bucketed to this label                             */
} asmtest_hwtrace_bucket_t;

/* Resolve the mapped-file pathname (or a "[...]" pseudo-name) + extent containing
 * `addr` in process `pid` (0 = self), keeping the maps pathname field. Returns 1 on
 * a hit (fills name/start/end; name always NUL-terminated), 0 on a miss. */
int asmtest_hwtrace_region_name(int pid, uint64_t addr, char *name, size_t namelen,
                                uint64_t *start, uint64_t *end);

/* Bucket ips[0..n) by containing perf-map JIT symbol (preferred) or mapped-file
 * region into buckets[0..cap), returning the number of distinct buckets written
 * (<= cap). Unresolved IPs bucket under "[unknown]"; if more than `cap` distinct
 * labels appear the surplus IPs are dropped (a caller wanting exact totals sizes
 * `cap` to the expected label count). */
size_t asmtest_hwtrace_symbolize_bucket(int pid, const uint64_t *ips, size_t n,
                                        asmtest_hwtrace_bucket_t *buckets,
                                        size_t cap);

/* A caller-known code region, for attributing a whole-window trace: any captured
 * address in [base, base+len) is labelled `name`. Lets several native leaves (each a
 * distinct exec_alloc'd blob that maps resolution would collapse into one "[anon]")
 * come back as separate, named buckets. `name` is truncated to fit. */
typedef struct {
    char name[64];
    uint64_t base;
    uint64_t len;
} asmtest_hwtrace_named_region_t;

/* Attribute the whole-window scope `handle`'s captured ABSOLUTE addresses into
 * labelled `buckets[0..cap)`: each address is matched against the caller's `regions`
 * first (exact, symbol-free — this is how multiple leaves separate), then falls back
 * to the perf-map JIT symbol / mapped-file region for the runtime remainder. Writes
 * *nbuckets (distinct labels, <= cap; surplus dropped). Returns ASMTEST_HW_OK, or
 * ASMTEST_HW_EINVAL on a stale/unknown handle, a REGION-scope handle (its insns are
 * relative offsets, not the absolute addresses this needs), or bad args;
 * ASMTEST_HW_ENOSYS where the whole-window path is unavailable (it is Linux/x86-64 —
 * a subset of the single-step tier, which also covers macOS x86-64). Host-testable. */
int asmtest_hwtrace_attribute_window(asmtest_hwtrace_scope_t handle,
                                     const asmtest_hwtrace_named_region_t *regions,
                                     size_t nregions,
                                     asmtest_hwtrace_bucket_t *buckets, size_t cap,
                                     size_t *nbuckets);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_HWTRACE_H */
