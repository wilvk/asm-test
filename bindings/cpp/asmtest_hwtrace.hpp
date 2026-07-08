/*
 * asmtest_hwtrace.hpp — header-only C++ wrapper for the optional hardware-tier
 * native-trace backend (see include/asmtest_hwtrace.h and docs/native-tracing.md).
 * The companion to asmtest_drtrace.hpp, mirroring the Python wrapper
 * (bindings/python/asmtest/hwtrace.py).
 *
 * Where the DynamoRIO tier (asmtest::NativeTrace in asmtest_drtrace.hpp) needs a
 * DynamoRIO install, this tier observes the **real CPU**. Four backends share one
 * API, selected by enum; the portable default is SINGLESTEP — EFLAGS.TF single-step
 * (#DB -> SIGTRAP), which is exact and complete on ANY x86-64 Linux (Intel, any-Zen
 * AMD, VM, CI, container) with no PMU, no perf_event, no privilege, and no decoder
 * library. The PT/CoreSight/AMD backends self-skip off the bare-metal hardware they
 * need. It records the same asmtest_trace_t offsets as the emulator and DynamoRIO
 * tiers.
 *
 *   #include "asmtest_hwtrace.hpp"
 *   using namespace asmtest;
 *   if (!HwTrace::available()) return 0;               // self-skip cleanly
 *   HwTrace::init();                                   // SINGLESTEP by default
 *   NativeCode code = NativeCode::from_bytes(bytes);
 *   HwTrace tr = HwTrace::create(64, 64);              // 64 block + 64 insn slots
 *   tr.register_region("add", code);
 *   { auto scope = tr.region("add"); long r = code.call(20, 22); }
 *   bool hit = tr.covered(0);
 *   HwTrace::shutdown();
 *
 * Like asmtest_drtrace.hpp this header links NOTHING at build time: it dlopen()s
 * libasmtest_hwtrace.so at runtime (resolved via $ASMTEST_HWTRACE_LIB, else the bare
 * soname on the loader path) and dlsym()s the C API, because the library may be
 * absent. Link only -ldl. When the library is unavailable, HwTrace::available()
 * returns false and callers self-skip; nothing here crashes or throws on a missing
 * lib.
 *
 * IMPORTANT: the single-step backend arms EFLAGS.TF across the begin..call..end
 * window — keep that bracket tight (no I/O between begin and end).
 *
 * Advanced, Linux-x86-64-only, and opt-in.
 */
#ifndef ASMTEST_HWTRACE_HPP
#define ASMTEST_HWTRACE_HPP

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "asmtest_hwtrace.h"
#include "asmtest_trace_auto.h"

namespace asmtest {

/* asmtest_trace_backend_t values, surfaced for callers selecting a backend. */
enum Backend {
    INTEL_PT = ASMTEST_HWTRACE_INTEL_PT,
    CORESIGHT = ASMTEST_HWTRACE_CORESIGHT,
    AMD_LBR = ASMTEST_HWTRACE_AMD_LBR,
    SINGLESTEP = ASMTEST_HWTRACE_SINGLESTEP,
};

/* asmtest_hwtrace_policy_t values for HwTrace::resolve()/auto_select(). BEST is
 * the most faithful available backend; CEILING_FREE additionally skips the one
 * fixed-window backend (AMD LBR) and is what you re-resolve under after a trace
 * comes back truncated. */
enum Policy {
    BEST = ASMTEST_HWTRACE_BEST,
    CEILING_FREE = ASMTEST_HWTRACE_CEILING_FREE,
};

/* asmtest_trace_auto.h — the CROSS-TIER orchestrator over all three trace tiers
 * (hardware + DynamoRIO + emulator), not just the hardware backends above. */

/* asmtest_trace_tier_t values: the trace tiers, most-faithful to least. */
enum Tier {
    TIER_HWTRACE = ASMTEST_TIER_HWTRACE,     // HW branch trace / single-step (real CPU)
    TIER_DYNAMORIO = ASMTEST_TIER_DYNAMORIO, // in-process software DBI (real CPU)
    TIER_EMULATOR = ASMTEST_TIER_EMULATOR,   // Unicorn virtual CPU (isolated guest)
};

/* asmtest_trace_fidelity_t values: NATIVE runs the real bytes on the real CPU
 * in-process; VIRTUAL runs an isolated guest on an emulated CPU. */
enum Fidelity {
    FIDELITY_NATIVE = ASMTEST_FIDELITY_NATIVE,
    FIDELITY_VIRTUAL = ASMTEST_FIDELITY_VIRTUAL,
};

/* Cross-tier policy bitmask for HwTrace::resolveTiers()/autoTier(). TRACE_BEST is
 * the most-faithful available choice (emulator floor allowed); TRACE_CEILING_FREE
 * drops the one fixed-window backend (AMD LBR); TRACE_NATIVE_ONLY forbids the
 * native->emulator fidelity crossing (drops the emulator floor). */
enum TracePolicy {
    TRACE_BEST = ASMTEST_TRACE_BEST,
    TRACE_CEILING_FREE = ASMTEST_TRACE_CEILING_FREE,
    TRACE_NATIVE_ONLY = ASMTEST_TRACE_NATIVE_ONLY,
};

/* asmtest_ptrace.h — out-of-process / foreign-process tracing status codes.
 * Defined here (not via the C header) so this wrapper keeps depending only on the
 * hwtrace headers, exactly as the Python wrapper redeclares them. */
enum PtraceStatus {
    ASMTEST_PTRACE_OK = 0,
    ASMTEST_PTRACE_ENOENT = -7,  // region / symbol / method not found
};

/* asmtest_descent_level_t — call-descent policy (see the Descent class). Redeclared
 * here (not via asmtest_ptrace.h) so this wrapper keeps depending only on the hwtrace
 * headers, exactly as PtraceStatus above and the Python wrapper's DESCENT_* do.
 *
 * NOTE: this binding is allow-set-only. dlopen/dlsym cannot host a GC-safe capturing
 * upcall, so it deliberately does NOT wrap the descent resolver/denylist setters (see
 * scripts/bindings-parity-allow.txt). DESCEND_KNOWN still descends via allow_region();
 * DESCEND_ALL / resolver-gated descent is unavailable here. */
enum DescentLevel {
    DESCENT_OFF = 0,            // step over, record nothing (default)
    DESCENT_RECORD_EDGES = 1,   // record (call-site -> callee) edges, still step over
    DESCENT_DESCEND_KNOWN = 2,  // step INTO calls resolvable via the allow-set
    DESCENT_DESCEND_ALL = 3,    // step INTO everything (needs a resolver/denylist: N/A here)
};

/* asmtest_codeimage.h — time-aware code-image recorder status codes. Redeclared
 * here (not via the C header) so this wrapper keeps depending only on the hwtrace
 * headers, exactly as PtraceStatus above and the Python wrapper do. */
enum CodeImageStatus {
    ASMTEST_CI_OK = 0,
    ASMTEST_CI_EINVAL = -1,    // bad argument
    ASMTEST_CI_EUNAVAIL = -3,  // no PAGEMAP_SCAN / soft-dirty / BPF privilege
    ASMTEST_CI_ENOSYS = -5,    // built without the needed support (e.g. no libbpf)
    ASMTEST_CI_ENOENT = -7,    // address never tracked / no version at-or-before when
    ASMTEST_CI_ELOAD = -8,     // libbpf load/attach failure (Phase C)
};

/* How a code-emission event was observed (the event's `kind` field). Mirrors the
 * ASMTEST_CI_KIND_* macros in asmtest_codeimage.h. */
enum CodeImageKind {
    ASMTEST_CI_KIND_MPROTECT = 1,  // mprotect(...PROT_EXEC...) — the common JIT edge
    ASMTEST_CI_KIND_MMAP = 2,      // mmap(...PROT_EXEC...); addr is the real base
    ASMTEST_CI_KIND_MEMFD = 3,     // memfd_create — staging hint; correlate via fd
};

/// A resolved cross-tier trace option: which `tier` to use, which hardware
/// `backend` within it (meaningful only when tier == TIER_HWTRACE; otherwise
/// 0/ignore), and the `fidelity` class (FIDELITY_NATIVE vs FIDELITY_VIRTUAL) so a
/// caller can see at a glance whether a choice crosses the native->emulator line.
/// Mirrors asmtest_trace_choice_t (three int-sized fields, no padding).
struct TierChoice {
    int tier;
    int backend;
    int fidelity;
};

namespace detail {

/* The C symbols of libasmtest_hwtrace, resolved once via dlsym. Mirrors the
 * _declare() table in the Python wrapper. The trace handle is an opaque void*
 * (matching the Python wrapper's c_void_p), so this header pulls in only the
 * hwtrace C header for constants and the options struct. */
struct HwApi {
    void *handle = nullptr;

    int (*available)(int) = nullptr;
    void (*skip_reason)(int, char *, size_t) = nullptr;
    size_t (*resolve)(int, asmtest_trace_backend_t *, size_t) = nullptr;
    int (*hwauto)(int) = nullptr;
    size_t (*trace_resolve)(unsigned, asmtest_trace_choice_t *, size_t) = nullptr;
    int (*trace_auto)(unsigned, asmtest_trace_choice_t *) = nullptr;
    int (*init)(const asmtest_hwtrace_options_t *) = nullptr;
    int (*register_region)(const char *, void *, size_t, void *) = nullptr;
    void (*begin)(const char *) = nullptr;
    void (*end)(const char *) = nullptr;
    /* Scoped-tracing shared core (§0/§1): error-returning begin, render-on-close,
     * arming-thread accessor. Non-gated (an older lib without them still loads). */
    int (*try_begin)(const char *) = nullptr;
    int (*render)(const char *, char *, size_t) = nullptr;
    int (*arm_tid)() = nullptr;
    /* §1 registry-free lazy-arm call + handle-keyed render (the call_scoped path).
     * render_scope takes the 8-byte asmtest_hwtrace_scope_t handle BY VALUE (native
     * POD from the #included header — no packing, unlike the Fiddle/JNI bindings). */
    int (*call_scoped_ex)(void *, size_t, void *, void *, const long *, int, long *,
                          asmtest_hwtrace_scope_t *) = nullptr;
    int (*render_scope)(asmtest_hwtrace_scope_t, char *, size_t) = nullptr;
    void (*shutdown)(void) = nullptr;
    int (*exec_alloc)(const void *, size_t, void **, size_t *) = nullptr;
    void (*exec_free)(void *, size_t) = nullptr;
    void *(*trace_new)(size_t, size_t) = nullptr;
    void (*trace_free)(void *) = nullptr;
    int (*trace_covered)(void *, uint64_t) = nullptr;
    uint64_t (*trace_blocks_len)(void *) = nullptr;
    uint64_t (*trace_insns_total)(void *) = nullptr;
    uint64_t (*trace_insns_len)(void *) = nullptr;
    int (*trace_truncated)(void *) = nullptr;
    uint64_t (*trace_block_at)(void *, size_t) = nullptr;
    uint64_t (*trace_insn_at)(void *, size_t) = nullptr;

    /* asmtest_ptrace.h — out-of-process / foreign-process tracing toolkit. pid_t
     * is `int` on Linux x86-64; the trace handle is the same opaque void* as
     * above; the jitdump entry is four consecutive uint64 (see PtraceJitEntry). */
    int (*ptrace_available)() = nullptr;
    void (*ptrace_skip_reason)(char *, size_t) = nullptr;
    int (*ptrace_trace_call)(const void *, size_t, const long *, int, long *,
                             void *) = nullptr;
    // BTF block-step tier: same shapes as the per-instruction trio above.
    int (*ptrace_blockstep_available)() = nullptr;
    int (*ptrace_trace_call_blockstep)(const void *, size_t, const long *, int,
                                       long *, void *) = nullptr;
    int (*ptrace_trace_attached_blockstep)(int, const void *, size_t, long *,
                                           void *) = nullptr;
    int (*ptrace_trace_attached)(int, const void *, size_t, long *,
                                 void *) = nullptr;
    int (*ptrace_run_to)(int, const void *) = nullptr;
    int (*proc_region_by_addr)(int, const void *, void **, size_t *) = nullptr;
    int (*proc_perfmap_symbol)(int, const char *, void **, size_t *) = nullptr;
    int (*jitdump_find)(const char *, int, const char *, void *, uint8_t *,
                        size_t, size_t *) = nullptr;

    /* asmtest_codeimage.h — the time-aware code-image recorder (a userspace
     * PERF_RECORD_TEXT_POKE). The timeline handle is the same opaque void* as
     * everything else here; the event is the 40-byte mirror below. The
     * versioned-decode bridge into the ptrace tracer lives here too. */
    int (*ci_available)() = nullptr;
    void (*ci_skip_reason)(char *, size_t) = nullptr;
    void *(*ci_new)(int) = nullptr;
    void (*ci_free)(void *) = nullptr;
    int (*ci_track)(void *, const void *, size_t) = nullptr;
    int (*ci_refresh)(void *) = nullptr;
    uint64_t (*ci_now)(const void *) = nullptr;
    int (*ci_bytes_at)(const void *, const void *, uint64_t, const uint8_t **,
                       size_t *) = nullptr;
    int (*ci_bpf_available)() = nullptr;
    void (*ci_bpf_skip_reason)(char *, size_t) = nullptr;
    int (*ci_watch_bpf)(void *) = nullptr;
    int (*ci_poll_bpf)(void *, int) = nullptr;
    int (*ci_next)(void *, void *) = nullptr;
    int (*ptrace_trace_attached_versioned)(int, const void *, size_t, void *,
                                           uint64_t, long *, void *) = nullptr;

    /* asmtest_ptrace.h — call descent (asmtest_descent_t): edges + nested frames.
     * The descent handle is the same opaque void* idiom as the trace handle. This
     * table wraps the 25 allow-set-visible descent symbols; the two callback
     * installers (set_resolver / set_denylist) are intentionally NOT resolved here
     * because a dlopen FFI cannot host a GC-safe capturing upcall (see the enum note
     * above and scripts/bindings-parity-allow.txt). */
    void *(*descent_new)(int) = nullptr;
    void (*descent_free)(void *) = nullptr;
    void (*descent_set_max_depth)(void *, uint32_t) = nullptr;
    void (*descent_set_insn_budget)(void *, uint64_t) = nullptr;
    void (*descent_set_watchdog_ms)(void *, uint32_t) = nullptr;
    void (*descent_use_default_denylist)(void *) = nullptr;
    int (*descent_allow_region)(void *, const void *, size_t) = nullptr;
    int (*descent_deny_region)(void *, const void *, size_t) = nullptr;
    size_t (*descent_edges_len)(void *) = nullptr;
    uint64_t (*descent_edge_site)(void *, size_t) = nullptr;
    uint64_t (*descent_edge_target)(void *, size_t) = nullptr;
    uint32_t (*descent_edge_depth)(void *, size_t) = nullptr;
    size_t (*descent_frames_len)(void *) = nullptr;
    uint64_t (*descent_frame_base)(void *, size_t) = nullptr;
    uint64_t (*descent_frame_len)(void *, size_t) = nullptr;
    uint32_t (*descent_frame_depth)(void *, size_t) = nullptr;
    int32_t (*descent_frame_parent)(void *, size_t) = nullptr;
    size_t (*descent_frame_insn_count)(void *, size_t) = nullptr;
    uint64_t (*descent_frame_insn_at)(void *, size_t, size_t) = nullptr;
    size_t (*descent_frame_block_count)(void *, size_t) = nullptr;
    uint64_t (*descent_frame_block_at)(void *, size_t, size_t) = nullptr;
    int (*descent_truncated)(void *) = nullptr;
    int (*descent_depth_capped)(void *) = nullptr;
    int (*ptrace_trace_call_ex)(const void *, size_t, const long *, int, long *,
                                void *, void *) = nullptr;
    int (*ptrace_trace_attached_ex)(int, const void *, size_t, long *, void *,
                                    void *) = nullptr;
    int (*ptrace_trace_attached_versioned_ex)(int, const void *, size_t, void *,
                                              uint64_t, long *, void *,
                                              void *) = nullptr;

    /* True if the library loaded and every symbol resolved. */
    bool loaded() const { return handle != nullptr; }
};

template <typename Fn>
inline bool dlsym_into(void *h, const char *name, Fn &slot) {
    /* The cast through size_t avoids the ISO C++ object<->function pointer
     * warning; POSIX guarantees dlsym returns a usable function address. */
    void *sym = ::dlsym(h, name);
    slot = reinterpret_cast<Fn>(reinterpret_cast<std::size_t>(sym));
    return sym != nullptr;
}

inline std::string lib_name() {
    return std::string("libasmtest_hwtrace.so");
}

inline std::string lib_path() {
    if (const char *env = ::getenv("ASMTEST_HWTRACE_LIB"))
        if (*env)
            return std::string(env);
    /* Else fall back to the bare soname. The Python wrapper derives <repo> from
     * __file__; a header has no such anchor at runtime, so we rely on
     * $ASMTEST_HWTRACE_LIB (set by the make lanes) or the default loader search
     * path for the bare soname. */
    return lib_name();
}

/* Process-wide, lazily loaded handle table. Never throws: on any failure it
 * returns a table with handle==nullptr so available() reports false. */
inline HwApi &api() {
    static HwApi a = [] {
        HwApi t;
        void *h = ::dlopen(lib_path().c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            h = ::dlopen(lib_name().c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            return t;  // handle stays null -> available() == false
        bool ok = true;
        ok &= dlsym_into(h, "asmtest_hwtrace_available", t.available);
        ok &= dlsym_into(h, "asmtest_hwtrace_skip_reason", t.skip_reason);
        ok &= dlsym_into(h, "asmtest_hwtrace_resolve", t.resolve);
        ok &= dlsym_into(h, "asmtest_hwtrace_auto", t.hwauto);
        ok &= dlsym_into(h, "asmtest_trace_resolve", t.trace_resolve);
        ok &= dlsym_into(h, "asmtest_trace_auto", t.trace_auto);
        ok &= dlsym_into(h, "asmtest_hwtrace_init", t.init);
        ok &= dlsym_into(h, "asmtest_hwtrace_register_region",
                         t.register_region);
        ok &= dlsym_into(h, "asmtest_hwtrace_begin", t.begin);
        ok &= dlsym_into(h, "asmtest_hwtrace_end", t.end);
        /* Non-gated: the scope construct falls back to begin/end when absent. */
        dlsym_into(h, "asmtest_hwtrace_try_begin", t.try_begin);
        dlsym_into(h, "asmtest_hwtrace_render", t.render);
        dlsym_into(h, "asmtest_hwtrace_arm_tid", t.arm_tid);
        dlsym_into(h, "asmtest_hwtrace_call_scoped_ex", t.call_scoped_ex);
        dlsym_into(h, "asmtest_hwtrace_render_scope", t.render_scope);
        ok &= dlsym_into(h, "asmtest_hwtrace_shutdown", t.shutdown);
        ok &= dlsym_into(h, "asmtest_hwtrace_exec_alloc", t.exec_alloc);
        ok &= dlsym_into(h, "asmtest_hwtrace_exec_free", t.exec_free);
        ok &= dlsym_into(h, "asmtest_trace_new", t.trace_new);
        ok &= dlsym_into(h, "asmtest_trace_free", t.trace_free);
        ok &= dlsym_into(h, "asmtest_trace_covered", t.trace_covered);
        ok &= dlsym_into(h, "asmtest_emu_trace_blocks_len", t.trace_blocks_len);
        ok &= dlsym_into(h, "asmtest_emu_trace_insns_total",
                         t.trace_insns_total);
        ok &= dlsym_into(h, "asmtest_emu_trace_insns_len", t.trace_insns_len);
        ok &= dlsym_into(h, "asmtest_emu_trace_truncated", t.trace_truncated);
        ok &= dlsym_into(h, "asmtest_emu_trace_block_at", t.trace_block_at);
        ok &= dlsym_into(h, "asmtest_emu_trace_insn_at", t.trace_insn_at);
        ok &= dlsym_into(h, "asmtest_ptrace_available", t.ptrace_available);
        ok &= dlsym_into(h, "asmtest_ptrace_skip_reason", t.ptrace_skip_reason);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_call", t.ptrace_trace_call);
        ok &= dlsym_into(h, "asmtest_ptrace_blockstep_available",
                         t.ptrace_blockstep_available);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_call_blockstep",
                         t.ptrace_trace_call_blockstep);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_attached_blockstep",
                         t.ptrace_trace_attached_blockstep);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_attached",
                         t.ptrace_trace_attached);
        ok &= dlsym_into(h, "asmtest_ptrace_run_to", t.ptrace_run_to);
        ok &= dlsym_into(h, "asmtest_proc_region_by_addr",
                         t.proc_region_by_addr);
        ok &= dlsym_into(h, "asmtest_proc_perfmap_symbol",
                         t.proc_perfmap_symbol);
        ok &= dlsym_into(h, "asmtest_jitdump_find", t.jitdump_find);
        ok &= dlsym_into(h, "asmtest_codeimage_available", t.ci_available);
        ok &= dlsym_into(h, "asmtest_codeimage_skip_reason",
                         t.ci_skip_reason);
        ok &= dlsym_into(h, "asmtest_codeimage_new", t.ci_new);
        ok &= dlsym_into(h, "asmtest_codeimage_free", t.ci_free);
        ok &= dlsym_into(h, "asmtest_codeimage_track", t.ci_track);
        ok &= dlsym_into(h, "asmtest_codeimage_refresh", t.ci_refresh);
        ok &= dlsym_into(h, "asmtest_codeimage_now", t.ci_now);
        ok &= dlsym_into(h, "asmtest_codeimage_bytes_at", t.ci_bytes_at);
        ok &= dlsym_into(h, "asmtest_codeimage_bpf_available",
                         t.ci_bpf_available);
        ok &= dlsym_into(h, "asmtest_codeimage_bpf_skip_reason",
                         t.ci_bpf_skip_reason);
        ok &= dlsym_into(h, "asmtest_codeimage_watch_bpf", t.ci_watch_bpf);
        ok &= dlsym_into(h, "asmtest_codeimage_poll_bpf", t.ci_poll_bpf);
        ok &= dlsym_into(h, "asmtest_codeimage_next", t.ci_next);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_attached_versioned",
                         t.ptrace_trace_attached_versioned);
        // Call descent (25 allow-set-visible symbols). set_resolver / set_denylist
        // are intentionally not resolved (allow-set-only binding; see the enum note).
        ok &= dlsym_into(h, "asmtest_descent_new", t.descent_new);
        ok &= dlsym_into(h, "asmtest_descent_free", t.descent_free);
        ok &= dlsym_into(h, "asmtest_descent_set_max_depth",
                         t.descent_set_max_depth);
        ok &= dlsym_into(h, "asmtest_descent_set_insn_budget",
                         t.descent_set_insn_budget);
        ok &= dlsym_into(h, "asmtest_descent_set_watchdog_ms",
                         t.descent_set_watchdog_ms);
        ok &= dlsym_into(h, "asmtest_descent_use_default_denylist",
                         t.descent_use_default_denylist);
        ok &= dlsym_into(h, "asmtest_descent_allow_region",
                         t.descent_allow_region);
        ok &= dlsym_into(h, "asmtest_descent_deny_region",
                         t.descent_deny_region);
        ok &= dlsym_into(h, "asmtest_descent_edges_len", t.descent_edges_len);
        ok &= dlsym_into(h, "asmtest_descent_edge_site", t.descent_edge_site);
        ok &= dlsym_into(h, "asmtest_descent_edge_target",
                         t.descent_edge_target);
        ok &= dlsym_into(h, "asmtest_descent_edge_depth", t.descent_edge_depth);
        ok &= dlsym_into(h, "asmtest_descent_frames_len", t.descent_frames_len);
        ok &= dlsym_into(h, "asmtest_descent_frame_base", t.descent_frame_base);
        ok &= dlsym_into(h, "asmtest_descent_frame_len", t.descent_frame_len);
        ok &= dlsym_into(h, "asmtest_descent_frame_depth",
                         t.descent_frame_depth);
        ok &= dlsym_into(h, "asmtest_descent_frame_parent",
                         t.descent_frame_parent);
        ok &= dlsym_into(h, "asmtest_descent_frame_insn_count",
                         t.descent_frame_insn_count);
        ok &= dlsym_into(h, "asmtest_descent_frame_insn_at",
                         t.descent_frame_insn_at);
        ok &= dlsym_into(h, "asmtest_descent_frame_block_count",
                         t.descent_frame_block_count);
        ok &= dlsym_into(h, "asmtest_descent_frame_block_at",
                         t.descent_frame_block_at);
        ok &= dlsym_into(h, "asmtest_descent_truncated", t.descent_truncated);
        ok &= dlsym_into(h, "asmtest_descent_depth_capped",
                         t.descent_depth_capped);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_call_ex",
                         t.ptrace_trace_call_ex);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_attached_ex",
                         t.ptrace_trace_attached_ex);
        ok &= dlsym_into(h, "asmtest_ptrace_trace_attached_versioned_ex",
                         t.ptrace_trace_attached_versioned_ex);
        if (!ok) {
            ::dlclose(h);
            return HwApi{};  // a fresh, empty table -> available() == false
        }
        t.handle = h;
        return t;
    }();
    return a;
}

/* Like api() but enforces the load contract for entry points that dereference a
 * resolved symbol: throws std::runtime_error (matching skip_reason's guidance)
 * instead of calling through a null function pointer when the optional library is
 * absent. available()/skip_reason()/resolve/auto keep using api() so they can
 * report the missing-lib case without throwing. */
inline HwApi &require() {
    HwApi &a = api();
    if (!a.loaded())
        throw std::runtime_error(
            "libasmtest_hwtrace not found; build it with `make shared-hwtrace` "
            "or set ASMTEST_HWTRACE_LIB");
    return a;
}

}  // namespace detail

/// Host-native machine code in real executable (W^X) memory. Move-only; frees
/// its mapping in the destructor. Backed by asmtest_hwtrace_exec_alloc, which is
/// self-contained (no DynamoRIO-tier dependency).
class NativeCode {
  public:
    NativeCode() = default;
    NativeCode(const NativeCode &) = delete;
    NativeCode &operator=(const NativeCode &) = delete;
    NativeCode(NativeCode &&o) noexcept
        : base_(o.base_), len_(o.len_), freed_(o.freed_) {
        o.base_ = nullptr;
        o.len_ = 0;
        o.freed_ = true;
    }
    NativeCode &operator=(NativeCode &&o) noexcept {
        if (this != &o) {
            free();
            base_ = o.base_;
            len_ = o.len_;
            freed_ = o.freed_;
            o.base_ = nullptr;
            o.len_ = 0;
            o.freed_ = true;
        }
        return *this;
    }
    ~NativeCode() { free(); }

    /// Map executable memory and copy `len` bytes of host-native machine code
    /// into it. Throws std::runtime_error on failure.
    static NativeCode from_bytes(const uint8_t *bytes, std::size_t len) {
        detail::HwApi &a = detail::require();
        void *base = nullptr;
        std::size_t out_len = 0;
        int rc = a.exec_alloc(bytes, len, &base, &out_len);
        if (rc != ASMTEST_HW_OK)
            throw std::runtime_error("asmtest_hwtrace_exec_alloc failed: " +
                                     std::to_string(rc));
        return NativeCode(base, out_len);
    }
    static NativeCode from_bytes(const std::vector<uint8_t> &bytes) {
        return from_bytes(bytes.data(), bytes.size());
    }

    void *base() const { return base_; }
    std::size_t length() const { return len_; }

    /// Invoke the code through a function pointer under the SysV integer ABI
    /// (each arg a long, the result a long). The default `Ret(Args...)` is
    /// `long(long...)`; pass explicit template args for another prototype.
    template <typename Ret = long, typename... Args>
    Ret call(Args... args) const {
        using Fn = Ret (*)(Args...);
        Fn fn = reinterpret_cast<Fn>(reinterpret_cast<std::size_t>(base_));
        return fn(args...);
    }

    void free() {
        if (!freed_) {
            detail::api().exec_free(base_, len_);
            freed_ = true;
            base_ = nullptr;
            len_ = 0;
        }
    }

  private:
    NativeCode(void *base, std::size_t len)
        : base_(base), len_(len), freed_(false) {}

    void *base_ = nullptr;
    std::size_t len_ = 0;
    bool freed_ = true;
};

/// Scoped begin/end marker pair for a named region. The constructor opens
/// recording (asmtest_hwtrace_begin), the destructor closes it
/// (asmtest_hwtrace_end) — so a region is always balanced even if the call
/// between them throws. Under the SINGLESTEP backend this is the window in which
/// EFLAGS.TF is armed, so keep the body tight (no I/O). Move-only; obtained from
/// HwTrace::region(name).
class RegionScope {
  public:
    RegionScope(const RegionScope &) = delete;
    RegionScope &operator=(const RegionScope &) = delete;
    RegionScope(RegionScope &&o) noexcept
        : name_(std::move(o.name_)), active_(o.active_) {
        o.active_ = false;
    }
    RegionScope &operator=(RegionScope &&) = delete;
    ~RegionScope() {
        if (active_)
            detail::api().end(name_.c_str());
    }

  private:
    friend class HwTrace;
    explicit RegionScope(std::string name)
        : name_(std::move(name)), active_(true) {
        detail::require().begin(name_.c_str());
    }

    std::string name_;
    bool active_;
};

namespace detail {
/// Build a call-site region name `basename:line` (basename dodges the 64-char C
/// name ceiling and full-path aliasing under Core §0.4's by-name registry).
inline std::string scope_name(const char *file, int line) {
    const char *base = file ? file : "?";
    if (const char *slash = std::strrchr(base, '/'))
        base = slash + 1;
    std::string n(base);
    n += ':';
    n += std::to_string(line);
    if (n.size() > 63)
        n = n.substr(n.size() - 63); /* keep the distinguishing tail */
    return n;
}
} // namespace detail

/// RAII scope over the register-then-begin/end pair with the shared-core
/// render-on-close. It auto-names from the CALL SITE via the
/// __builtin_FUNCTION/FILE/LINE default arguments (evaluated where the object is
/// constructed — the C++17 floor; `std::source_location` needs C++20), brackets
/// `try_begin`/`end` (a nonzero try_begin is a clean self-skip), and — in a
/// **noexcept** destructor (never throw / abort from a dtor) — renders the executed
/// assembly into `path()` and, when `emit`, writes it to stdout. The C core flags the
/// trace `truncated` on a cross-thread close (§0.2/§1), surfaced via `truncated()`.
class ScopedTrace {
  public:
    /// `out` (optional) receives the rendered listing on close, so it is readable
    /// after the RAII scope ends (the render happens in the destructor); `emit`
    /// additionally writes it to stdout.
    explicit ScopedTrace(const NativeCode &code, std::string *out = nullptr,
                         bool emit = true, const char *file = __builtin_FILE(),
                         int line = __builtin_LINE())
        : name_(detail::scope_name(file, line)), out_(out), emit_(emit) {
        detail::HwApi &a = detail::require();
        handle_ = a.trace_new(256, 64);
        if (handle_ != nullptr)
            a.register_region(name_.c_str(), code.base(), code.length(), handle_);
        if (a.try_begin != nullptr)
            active_ = (a.try_begin(name_.c_str()) == 0);
        else {
            a.begin(name_.c_str());
            active_ = true;
        }
    }
    ScopedTrace(const ScopedTrace &) = delete;
    ScopedTrace &operator=(const ScopedTrace &) = delete;

    ~ScopedTrace() noexcept {
        detail::HwApi &a = detail::api();
        if (!a.loaded()) {
            if (handle_ != nullptr && a.trace_free)
                a.trace_free(handle_);
            return;
        }
        a.end(name_.c_str());
        if (a.render != nullptr) {
            int need = a.render(name_.c_str(), nullptr, 0);
            if (need > 0) {
                path_.resize(static_cast<std::size_t>(need));
                a.render(name_.c_str(), &path_[0],
                         static_cast<std::size_t>(need) + 1);
            }
        }
        if (a.trace_truncated != nullptr && handle_ != nullptr)
            truncated_ = a.trace_truncated(handle_) != 0;
        if (out_ != nullptr)
            *out_ = path_;
        if (emit_ && !path_.empty())
            std::fputs(path_.c_str(), stdout);
        if (handle_ != nullptr && a.trace_free != nullptr)
            a.trace_free(handle_);
    }

    /// The rendered assembly listing (populated on close; empty if unavailable).
    const std::string &path() const { return path_; }
    /// True if the scope was armed (a backend was available and try_begin succeeded).
    bool armed() const { return active_; }
    /// True if the close hopped OS threads / the capture overflowed (Core §0.2/§1).
    bool truncated() const { return truncated_; }
    /// The auto-generated (or explicit) region name.
    const std::string &name() const { return name_; }

  private:
    std::string name_;
    std::string path_;
    void *handle_ = nullptr;
    std::string *out_;
    bool emit_;
    bool active_ = false;
    bool truncated_ = false;
};

/// The outcome of `HwTrace::callScoped`: the traced call's return value, the
/// executed body's disassembly (`path`, empty when no decoder is present), and
/// the honesty bits. Mirrors the Python/Ruby `CallScopedResult`.
struct CallScopedResult {
    long result = 0;
    std::string path;
    bool truncated = false;
    int rc = 0;
    /// True when the scope armed and the call ran (rc == ASMTEST_HW_OK).
    bool ok() const { return rc == ASMTEST_HW_OK; }
};

/// A coverage recorder for a registered native region, via the hardware tier.
/// Process-wide lifecycle (available/init/shutdown) is static; per-trace state is
/// the instance. Move-only; frees its handle in the destructor.
class HwTrace {
  public:
    HwTrace(const HwTrace &) = delete;
    HwTrace &operator=(const HwTrace &) = delete;
    HwTrace(HwTrace &&o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    HwTrace &operator=(HwTrace &&o) noexcept {
        if (this != &o) {
            free();
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }
    ~HwTrace() { free(); }

    // ---- process-wide lifecycle ----

    /// True if the chosen backend can run on this host (self-skip otherwise).
    /// Never crashes or throws — a missing library reports false. SINGLESTEP is
    /// the portable default that runs on any x86-64 Linux.
    static bool available(int backend = SINGLESTEP) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return false;
        return a.available(backend) != 0;
    }

    /// Human-readable reason available() is false (or "available"). Returns a
    /// fixed string when the library itself is missing.
    static std::string skip_reason(int backend = SINGLESTEP) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return "libasmtest_hwtrace not found (set ASMTEST_HWTRACE_LIB)";
        char buf[256] = {0};
        a.skip_reason(backend, buf, sizeof(buf));
        return std::string(buf);
    }

    /// This host's hardware-trace fallback cascade: the available backends,
    /// most-faithful first (INTEL_PT > AMD_LBR > SINGLESTEP > CORESIGHT), honoring
    /// `policy`. Empty only off x86-64 Linux (single-step is the floor there) or
    /// when the library is missing. CEILING_FREE drops the ceiling-bounded backend
    /// (AMD LBR).
    static std::vector<int> resolve(int policy = BEST) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return {};
        asmtest_trace_backend_t out[4];
        std::size_t n = a.resolve(policy, out, 4);
        std::vector<int> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = static_cast<int>(out[i]);
        return v;
    }

    /// The single most-preferred available backend under `policy` (a backend enum
    /// >= 0, ready to init()), or ASMTEST_HW_EUNAVAIL (< 0) when no hardware-trace
    /// backend is available on this host. (`auto` is a keyword, so this is
    /// `auto_select`.)
    static int auto_select(int policy = BEST) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return ASMTEST_HW_EUNAVAIL;
        return a.hwauto(policy);
    }

    /// This host's full CROSS-TIER cascade (asmtest_trace_resolve), most-faithful
    /// first: Intel PT -> AMD LBR -> DynamoRIO -> single-step -> CoreSight ->
    /// emulator, each included only if its tier is available. Empty when the
    /// library is missing, or off a native host under TRACE_NATIVE_ONLY.
    /// TRACE_NATIVE_ONLY drops the emulator floor (no native->emulator fidelity
    /// crossing); TRACE_CEILING_FREE drops AMD LBR.
    static std::vector<TierChoice> resolveTiers(unsigned policy = TRACE_BEST) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return {};
        asmtest_trace_choice_t out[8];
        std::size_t n = a.trace_resolve(policy, out, 8);
        std::vector<TierChoice> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = TierChoice{static_cast<int>(out[i].tier),
                              static_cast<int>(out[i].backend),
                              static_cast<int>(out[i].fidelity)};
        return v;
    }

    /// The single most-preferred available cross-tier choice under `policy`
    /// (asmtest_trace_auto), or std::nullopt when the cascade is empty (the library
    /// is missing, or off a native host under TRACE_NATIVE_ONLY). (`auto` is a
    /// keyword, so this is `autoTier`.)
    static std::optional<TierChoice> autoTier(unsigned policy = TRACE_BEST) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return std::nullopt;
        asmtest_trace_choice_t out{};
        if (a.trace_auto(policy, &out) != ASMTEST_HW_OK)
            return std::nullopt;
        return TierChoice{static_cast<int>(out.tier),
                          static_cast<int>(out.backend),
                          static_cast<int>(out.fidelity)};
    }

    /// Select a backend and initialize the tier. SINGLESTEP is the portable
    /// default that runs on any x86-64 Linux. Throws std::runtime_error on a
    /// nonzero return code.
    static void init(int backend = SINGLESTEP) {
        asmtest_hwtrace_options_t opts{};
        opts.backend = static_cast<asmtest_trace_backend_t>(backend);
        int rc = detail::require().init(&opts);
        if (rc != ASMTEST_HW_OK)
            throw std::runtime_error("asmtest_hwtrace_init failed: " +
                                     std::to_string(rc));
    }

    // Safe no-op when the library never loaded, so an unconditional teardown path
    // after a self-skip cannot dereference a null pointer.
    static void shutdown() {
        detail::HwApi &a = detail::api();
        if (a.loaded())
            a.shutdown();
    }

    // ---- per-trace ----

    /// Allocate a trace: `blocks` block slots and `instructions` instruction
    /// slots (instruction recording active when instructions > 0). Note the C
    /// ABI takes insns first, blocks second. Throws on allocation failure.
    /// (`new` is reserved, so this is `create`.)
    static HwTrace create(std::size_t blocks = 64,
                          std::size_t instructions = 64) {
        void *h = detail::require().trace_new(instructions, blocks);
        if (!h)
            throw std::runtime_error("asmtest_trace_new returned NULL");
        return HwTrace(h);
    }

    /// Register a non-overlapping native code range under `name`, recording
    /// coverage into this trace. Throws std::runtime_error on failure.
    void register_region(const std::string &name, const NativeCode &code) {
        int rc = detail::require().register_region(name.c_str(), code.base(),
                                                   code.length(), handle_);
        if (rc != ASMTEST_HW_OK)
            throw std::runtime_error("register_region(" + name +
                                     ") failed: " + std::to_string(rc));
    }

    /// A scoped begin/end marker for `name`. Use it in a tight block:
    /// `{ auto s = tr.region("add"); code.call(...); }`.
    RegionScope region(const std::string &name) { return RegionScope(name); }

    /// Trace ONE native call the managed-safe way: arm the single-step window,
    /// call `code(args...)` through the SysV integer ABI, and disarm — all in
    /// native code (`asmtest_hwtrace_call_scoped_ex`), a tighter window than the
    /// `region` form. REGISTRY-FREE (consumes no MAX_REGIONS slot), so it is safe
    /// in a tight loop. Owns its own trace handle. Integer args (0-6). Self-skips
    /// (`.rc` negative, `.result` 0) where no single-step backend is available.
    /// The tier must already be up (`HwTrace::init(SINGLESTEP)`).
    template <typename... Args>
    static CallScopedResult callScoped(const NativeCode &code, Args... args) {
        detail::HwApi &a = detail::require();
        if (!a.call_scoped_ex || !a.render_scope)
            return {0, "", false, ASMTEST_HW_EUNAVAIL};
        void *handle = a.trace_new(256, 64); // insns=256, blocks=64
        if (!handle)
            throw std::runtime_error("asmtest_trace_new returned NULL");
        // Free the trace on EVERY exit path — including a throwing path.resize below.
        // This is the Python reference's try/finally (hwtrace.py) in RAII form.
        struct FreeTrace {
            detail::HwApi &a;
            void *h;
            ~FreeTrace() {
                if (h)
                    a.trace_free(h);
            }
        } freer{a, handle};
        long arr[] = {static_cast<long>(args)..., 0L}; // +1 avoids a zero-size array
        const int n = static_cast<int>(sizeof...(Args));
        long result = 0;
        asmtest_hwtrace_scope_t scope{};
        int rc = a.call_scoped_ex(code.base(), code.length(), handle, code.base(),
                                  (n ? arr : nullptr), n, &result, &scope);
        if (rc != ASMTEST_HW_OK)
            return {0, "", false, rc};
        std::string path;
        int need = a.render_scope(scope, nullptr, 0);
        if (need > 0) {
            path.resize(static_cast<std::size_t>(need));
            a.render_scope(scope, &path[0], static_cast<std::size_t>(need) + 1);
        }
        bool trunc = a.trace_truncated(handle) != 0;
        return {result, path, trunc, rc};
    }

    /// True if basic-block offset `off` is in this trace's distinct block set.
    bool covered(uint64_t off) const {
        return detail::api().trace_covered(handle_, off) != 0;
    }

    /// Distinct basic blocks recorded so far.
    std::uint64_t blocks_len() const {
        return detail::api().trace_blocks_len(handle_);
    }

    /// Total instructions executed (counts past the instruction buffer cap).
    std::uint64_t insns_total() const {
        return detail::api().trace_insns_total(handle_);
    }

    /// Instruction offsets actually stored (<= insns_total, capped by capacity).
    std::uint64_t insns_len() const {
        return detail::api().trace_insns_len(handle_);
    }

    /// True if the instruction buffer overflowed its capacity.
    bool truncated() const {
        return detail::api().trace_truncated(handle_) != 0;
    }

    /// The distinct basic-block start offsets recorded, in first-seen order.
    std::vector<std::uint64_t> block_offsets() const {
        const auto &a = detail::api();
        std::size_t n = static_cast<std::size_t>(a.trace_blocks_len(handle_));
        std::vector<std::uint64_t> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = a.trace_block_at(handle_, i);
        return v;
    }

    /// The ordered instruction-offset stream actually stored (insns_len entries,
    /// in execution order — not the possibly-larger insns_total).
    std::vector<std::uint64_t> insn_offsets() const {
        const auto &a = detail::api();
        std::size_t n = static_cast<std::size_t>(a.trace_insns_len(handle_));
        std::vector<std::uint64_t> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = a.trace_insn_at(handle_, i);
        return v;
    }

    void free() {
        if (handle_) {
            detail::api().trace_free(handle_);
            handle_ = nullptr;
        }
    }

  private:
    friend class Ptrace;
    /// The opaque asmtest_trace_t* handle, as the out-of-process tracer needs it
    /// (mirrors the Python wrapper reading trace._handle).
    void *raw() const { return handle_; }

    explicit HwTrace(void *h) : handle_(h) {}

    void *handle_ = nullptr;
};

/// Mirrors asmtest_jitdump_entry_t: four consecutive uint64, no padding. The
/// `code` bytes are carried alongside in JitMethod, not in this layout struct.
struct PtraceJitEntry {
    std::uint64_t code_addr;
    std::uint64_t code_size;
    std::uint64_t timestamp;
    std::uint64_t code_index;
};

/// A JIT method resolved from a jitdump: load address, size, the JIT's
/// timestamp/index, and (optionally) the recorded native code bytes. Mirrors the
/// Python wrapper's JitMethod.
struct JitMethod {
    std::uint64_t code_addr = 0;
    std::uint64_t code_size = 0;
    std::uint64_t timestamp = 0;
    std::uint64_t code_index = 0;
    std::vector<std::uint8_t> code;  // the recorded bytes (empty unless requested)
};

/// Forward declaration: Descent::frame() hands out a FrameView, defined just after
/// Descent so its accessors can call Descent's public readers.
class FrameView;

/// Call descent (asmtest_descent_t): configure how the ptrace stepper handles the
/// call-outs it would otherwise step over, and read back the recorded edges + nested
/// frames. Four levels (see DescentLevel): OFF, RECORD_EDGES, DESCEND_KNOWN,
/// DESCEND_ALL. Pass to Ptrace::traceCallEx and friends. Frame 0 is the root region
/// (a superset of the flat trace); descended callees are frames 1..N.
///
/// Move-only; owns the handle with an idempotent, NULL-safe free() (the native side
/// also NULLs internally, so a double free is a no-op — the trace-handle discipline
/// this header uses everywhere). This binding is ALLOW-SET-ONLY: it offers
/// allow_region()/deny_region() but no set_resolver/set_denylist callback API, because
/// a dlopen FFI cannot host a GC-safe capturing upcall.
class Descent {
  public:
    /// A single stepped-over call-out (level >= 1): the call-site offset within its
    /// frame, the ABSOLUTE callee target address, and the caller's descent depth.
    struct Edge {
        std::uint64_t site;
        std::uint64_t target;
        std::uint32_t depth;
    };

    /// Allocate a descent handle at `level` (conservative depth/budget/watchdog
    /// defaults; empty allow-set). Throws std::runtime_error if the library is
    /// missing or allocation fails.
    explicit Descent(int level = DESCENT_OFF) {
        handle_ = detail::require().descent_new(level);
        if (!handle_)
            throw std::runtime_error("asmtest_descent_new failed");
    }

    Descent(const Descent &) = delete;
    Descent &operator=(const Descent &) = delete;
    Descent(Descent &&o) noexcept : handle_(o.handle_) { o.handle_ = nullptr; }
    Descent &operator=(Descent &&o) noexcept {
        if (this != &o) {
            free();
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }
    ~Descent() { free(); }

    // ---- configuration (in) ----

    /// Ceiling on nested descent depth (frame 0 is depth 0). 0 restores the default.
    void set_max_depth(std::uint32_t d) {
        detail::api().descent_set_max_depth(handle_, d);
    }
    /// Total single-step instruction budget across all descended frames; 0 = default.
    void set_insn_budget(std::uint64_t b) {
        detail::api().descent_set_insn_budget(handle_, b);
    }
    /// Real-time watchdog (ms) for a descended run; 0 = default.
    void set_watchdog_ms(std::uint32_t ms) {
        detail::api().descent_set_watchdog_ms(handle_, ms);
    }
    /// Arm the built-in L3 default denylist (PLT resolver / vdso / GC-JIT
    /// modules; plus blocking-libc entry points on the fork path).
    void use_default_denylist() {
        detail::api().descent_use_default_denylist(handle_);
    }
    /// Add [base, base+len) to the level-2 allow-set (descend into calls landing
    /// inside). Returns 0 on success, negative on OOM.
    int allow_region(const void *base, std::size_t len) {
        return detail::api().descent_allow_region(handle_, base, len);
    }
    /// Add [base, base+len) to the level-3 deny-set (never descend into it).
    int deny_region(const void *base, std::size_t len) {
        return detail::api().descent_deny_region(handle_, base, len);
    }

    // ---- results (out) ----

    /// Every stepped-over call-out (level >= 1), in record order.
    std::vector<Edge> edges() const {
        const auto &a = detail::api();
        std::size_t n = a.descent_edges_len(handle_);
        std::vector<Edge> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = Edge{a.descent_edge_site(handle_, i),
                        a.descent_edge_target(handle_, i),
                        a.descent_edge_depth(handle_, i)};
        return v;
    }

    /// Number of recorded frames (>= 1 once a call is traced: frame 0 is the root).
    std::size_t frames_len() const {
        return detail::api().descent_frames_len(handle_);
    }
    /// ABSOLUTE base address of frame `f`.
    std::uint64_t frame_base(std::size_t f) const {
        return detail::api().descent_frame_base(handle_, f);
    }
    /// Byte length of frame `f`'s region.
    std::uint64_t frame_len(std::size_t f) const {
        return detail::api().descent_frame_len(handle_, f);
    }
    /// Descent depth of frame `f` (0 = frame 0 / root).
    std::uint32_t frame_depth(std::size_t f) const {
        return detail::api().descent_frame_depth(handle_, f);
    }
    /// Parent frame index of frame `f` (-1 = root).
    std::int32_t frame_parent(std::size_t f) const {
        return detail::api().descent_frame_parent(handle_, f);
    }
    /// Frame `f`'s instruction offsets (relative to the frame base), execution order.
    std::vector<std::uint64_t> frame_insns(std::size_t f) const {
        const auto &a = detail::api();
        std::size_t n = a.descent_frame_insn_count(handle_, f);
        std::vector<std::uint64_t> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = a.descent_frame_insn_at(handle_, f, i);
        return v;
    }
    /// Frame `f`'s distinct basic-block start offsets (relative to the frame base).
    std::vector<std::uint64_t> frame_blocks(std::size_t f) const {
        const auto &a = detail::api();
        std::size_t n = a.descent_frame_block_count(handle_, f);
        std::vector<std::uint64_t> v(n);
        for (std::size_t i = 0; i < n; ++i)
            v[i] = a.descent_frame_block_at(handle_, f, i);
        return v;
    }
    /// True if a pool overflowed / a byte failed to decode (record incomplete).
    bool truncated() const {
        return detail::api().descent_truncated(handle_) != 0;
    }
    /// True if descent stopped at a policy limit (max_depth / budget / recursion
    /// cap) — distinct from a pool overflow.
    bool depth_capped() const {
        return detail::api().descent_depth_capped(handle_) != 0;
    }

    /// A NON-OWNING view of descended frame `f` (see FrameView). The view borrows
    /// this Descent and never frees the handle; keep the Descent alive while it is used.
    FrameView frame(std::size_t f) const;

    void free() {
        if (handle_) {
            detail::api().descent_free(handle_);
            handle_ = nullptr;
        }
    }

  private:
    friend class Ptrace;
    /// The opaque asmtest_descent_t* handle, as the ptrace tracer needs it.
    void *raw() const { return handle_; }

    void *handle_ = nullptr;
};

/// A non-owning window onto one descended frame of a Descent. It holds a reference
/// to the owning Descent and reads through its accessors; it NEVER calls
/// descent_free (the Descent owns the handle). Cheap to copy; must not outlive the
/// Descent it views.
class FrameView {
  public:
    /// This frame's index within the Descent (0 = root).
    std::size_t index() const { return f_; }
    /// ABSOLUTE base address of the frame.
    std::uint64_t base() const { return d_.frame_base(f_); }
    /// Byte length of the frame's region.
    std::uint64_t length() const { return d_.frame_len(f_); }
    /// Descent depth (0 = root).
    std::uint32_t depth() const { return d_.frame_depth(f_); }
    /// Parent frame index (-1 = root).
    std::int32_t parent() const { return d_.frame_parent(f_); }
    /// Instruction offsets (relative to base), in execution order.
    std::vector<std::uint64_t> insns() const { return d_.frame_insns(f_); }
    /// Distinct basic-block start offsets (relative to base).
    std::vector<std::uint64_t> blocks() const { return d_.frame_blocks(f_); }

  private:
    friend class Descent;
    FrameView(const Descent &d, std::size_t f) : d_(d), f_(f) {}

    const Descent &d_;  // borrowed — never freed here
    std::size_t f_;
};

inline FrameView Descent::frame(std::size_t f) const {
    return FrameView(*this, f);
}

/// Forward declaration: traceAttachedVersioned() decodes against a CodeImage,
/// which is defined below (it in turn does not depend on Ptrace).
class CodeImage;

/// Out-of-process / foreign-process tracing (asmtest_ptrace.h): single-step a
/// forked or externally-attached target out of band, and resolve the code region
/// to trace from the OS — /proc/<pid>/maps, a JIT perf-map, or a binary jitdump.
/// The managed-runtime path (JVM/.NET/Node on AMD, where Intel PT is unavailable
/// and in-process DynamoRIO cannot seize the runtime's threads). Linux x86-64.
/// All-static, mirroring HwTrace's process-wide lifecycle methods; the trace
/// handle and exec-allocated code are the same HwTrace / NativeCode types.
class Ptrace {
  public:
    /// True if the out-of-process single-step tracer can run on this host
    /// (Linux x86-64). Never crashes or throws — a missing library reports false.
    static bool available() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return false;
        return a.ptrace_available() != 0;
    }

    /// Human-readable reason available() is false (or "available"). Returns a
    /// fixed string when the library itself is missing.
    static std::string skipReason() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return "libasmtest_hwtrace not found (set ASMTEST_HWTRACE_LIB)";
        char buf[256] = {0};
        a.ptrace_skip_reason(buf, sizeof(buf));
        return std::string(buf);
    }

    /// Fork a tracee that calls `code` (`len` bytes already executable at `code`)
    /// with up to six integer `args`, single-step it out of process, and fill
    /// `trace`; returns the routine's return value (the child's RAX at the ret).
    /// Throws std::runtime_error on a nonzero status.
    static long traceCall(const void *code, std::size_t len,
                          const std::vector<long> &args, const HwTrace &trace) {
        long result = 0;
        int rc = detail::require().ptrace_trace_call(
            code, len, args.data(), static_cast<int>(args.size()), &result,
            trace.raw());
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error("asmtest_ptrace_trace_call failed: " +
                                     std::to_string(rc));
        return result;
    }

    /// Same, taking a NativeCode (uses its base/length).
    static long traceCall(const NativeCode &code, const std::vector<long> &args,
                          const HwTrace &trace) {
        return traceCall(code.base(), code.length(), args, trace);
    }

    /// True if the BTF block-step variant (PTRACE_SINGLEBLOCK — one #DB per TAKEN
    /// branch instead of one per instruction) can run here: x86-64 Linux with a
    /// functional PTRACE_SINGLEBLOCK and Capstone for the intra-block
    /// reconstruction. Hang-proof, cached probe; callers self-skip on false.
    static bool blockstepAvailable() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return false;
        return a.ptrace_blockstep_available() != 0;
    }

    /// Block-step variant of traceCall: drives PTRACE_SINGLEBLOCK (DEBUGCTL.BTF),
    /// stopping once per TAKEN branch and reconstructing the intra-block
    /// instructions with Capstone — the same insns/blocks stream as traceCall at a
    /// fraction of the stops. Probe first with blockstepAvailable(). Complete at
    /// moderate overhead, NOT cheap: each block still costs a full ptrace
    /// round-trip. Throws std::runtime_error on a nonzero status.
    static long traceCallBlockstep(const void *code, std::size_t len,
                                   const std::vector<long> &args,
                                   const HwTrace &trace) {
        long result = 0;
        int rc = detail::require().ptrace_trace_call_blockstep(
            code, len, args.data(), static_cast<int>(args.size()), &result,
            trace.raw());
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error(
                "asmtest_ptrace_trace_call_blockstep failed: " +
                std::to_string(rc));
        return result;
    }

    /// Same, taking a NativeCode (uses its base/length).
    static long traceCallBlockstep(const NativeCode &code,
                                   const std::vector<long> &args,
                                   const HwTrace &trace) {
        return traceCallBlockstep(code.base(), code.length(), args, trace);
    }

    /// Descending trace_call: thread a Descent handle through the single-step loop so
    /// call-outs are recorded as edges and (at level >= DESCEND_KNOWN) descended as
    /// nested frames. `trace` (the flat frame-0 view) and `descent` are each optional
    /// (pass nullptr to skip one). `len` is the traced region's byte length — pass the
    /// callee-excluding region, NOT the whole allocation, when a call target is an
    /// in-blob sibling that must stay OUTSIDE the traced region (else it mis-records as
    /// recursion). Throws std::runtime_error on a nonzero status.
    static long traceCallEx(const void *code, std::size_t len,
                            const std::vector<long> &args, HwTrace *trace,
                            Descent *descent) {
        long result = 0;
        int rc = detail::require().ptrace_trace_call_ex(
            code, len, args.data(), static_cast<int>(args.size()), &result,
            trace ? trace->raw() : nullptr,
            descent ? descent->raw() : nullptr);
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error("asmtest_ptrace_trace_call_ex failed: " +
                                     std::to_string(rc));
        return result;
    }

    /// Same, taking a NativeCode. `region` is the traced region's byte length;
    /// when 0 (the default) it falls back to the whole allocation (code.length()).
    /// Pass an explicit region (e.g. 0xc) to keep an in-blob sibling out of it.
    static long traceCallEx(const NativeCode &code,
                            const std::vector<long> &args, HwTrace *trace,
                            Descent *descent, std::size_t region = 0) {
        return traceCallEx(code.base(), region ? region : code.length(), args,
                           trace, descent);
    }

    /// Trace a region in a SEPARATE, already-ptrace-stopped process (the caller
    /// owns PTRACE_ATTACH/DETACH). Reads the target's bytes via process_vm_readv.
    /// Returns the target's RAX at the region exit. Throws on a nonzero status.
    static long traceAttached(int pid, const void *base, std::size_t len,
                              const HwTrace &trace) {
        long result = 0;
        int rc = detail::require().ptrace_trace_attached(pid, base, len, &result,
                                                         trace.raw());
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error("asmtest_ptrace_trace_attached failed: " +
                                     std::to_string(rc));
        return result;
    }

    /// Block-step variant of traceAttached: one #DB per TAKEN branch (intra-block
    /// instructions reconstructed with Capstone), same contract otherwise — the
    /// rootless managed-runtime completeness fallback at a fraction of the stops.
    /// Probe first with blockstepAvailable(). Throws on a nonzero status.
    static long traceAttachedBlockstep(int pid, const void *base, std::size_t len,
                                       const HwTrace &trace) {
        long result = 0;
        int rc = detail::require().ptrace_trace_attached_blockstep(
            pid, base, len, &result, trace.raw());
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error(
                "asmtest_ptrace_trace_attached_blockstep failed: " +
                std::to_string(rc));
        return result;
    }

    /// Descending variant of traceAttached for an externally-attached process:
    /// threads a Descent handle through the loop. `trace` and `descent` are each
    /// optional (nullptr to skip one). Throws on a nonzero status.
    static long traceAttachedEx(int pid, const void *base, std::size_t len,
                                HwTrace *trace, Descent *descent) {
        long result = 0;
        int rc = detail::require().ptrace_trace_attached_ex(
            pid, base, len, &result, trace ? trace->raw() : nullptr,
            descent ? descent->raw() : nullptr);
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error(
                "asmtest_ptrace_trace_attached_ex failed: " +
                std::to_string(rc));
        return result;
    }

    /// Descending variant of traceAttachedVersioned (code-image-versioned bytes +
    /// a Descent handle). Defined out-of-line below, after CodeImage.
    static long traceAttachedVersionedEx(int pid, const void *base,
                                         std::size_t len, const CodeImage &img,
                                         std::uint64_t when, HwTrace *trace,
                                         Descent *descent);

    /// Like traceAttached, but decode the target's instructions against the bytes
    /// the CodeImage recorder captured at capture sequence `when` (when == 0 =>
    /// the latest version) instead of a live process_vm_readv snapshot — so a JIT
    /// method whose address was patched or reused mid-trace decodes against the
    /// bytes that were live then, not whatever happens to be mapped at read time.
    /// Returns the target's RAX at the region exit. Throws on a nonzero status.
    /// (Defined out-of-line below, after CodeImage.)
    static long traceAttachedVersioned(int pid, const void *base,
                                       std::size_t len, const CodeImage &img,
                                       std::uint64_t when, const HwTrace &trace);

    /// Run an already-attached, ptrace-stopped target forward until it reaches
    /// `addr` (a software breakpoint that fires when the program itself next calls
    /// in), leaving it stopped there ready for traceAttached. Returns the status:
    /// ASMTEST_PTRACE_OK, or ASMTEST_PTRACE_ENOENT if the target exited first. The
    /// caller owns PTRACE_ATTACH/DETACH.
    static int runTo(int pid, const void *addr) {
        return detail::require().ptrace_run_to(pid, addr);
    }

    /// The executable mapping in /proc/<pid>/maps containing `addr`, as
    /// (base, len), or std::nullopt if no executable mapping contains it.
    static std::optional<std::pair<void *, std::size_t>>
    regionByAddr(int pid, const void *addr) {
        void *base = nullptr;
        std::size_t len = 0;
        int rc = detail::require().proc_region_by_addr(pid, addr, &base, &len);
        if (rc != ASMTEST_PTRACE_OK)
            return std::nullopt;
        return std::make_pair(base, len);
    }

    /// A JIT method by `name` in /tmp/perf-<pid>.map, as (base, len), or nullopt.
    static std::optional<std::pair<void *, std::size_t>>
    perfmapSymbol(int pid, const std::string &name) {
        void *base = nullptr;
        std::size_t len = 0;
        int rc = detail::require().proc_perfmap_symbol(pid, name.c_str(), &base,
                                                       &len);
        if (rc != ASMTEST_PTRACE_OK)
            return std::nullopt;
        return std::make_pair(base, len);
    }

    /// A JIT method by `name` from a jitdump (`path`, or /tmp/jit-<pid>.dump when
    /// `path` is empty) as a JitMethod carrying up to `wantBytes` of recorded
    /// code, or std::nullopt. The latest re-JIT body (highest timestamp) wins.
    static std::optional<JitMethod> jitdumpFind(const std::string &path,
                                                const std::string &name,
                                                int pid = 0,
                                                std::size_t wantBytes = 0) {
        PtraceJitEntry e{};
        std::vector<std::uint8_t> buf(wantBytes);
        std::size_t blen = 0;
        const char *p = path.empty() ? nullptr : path.c_str();
        int rc = detail::require().jitdump_find(
            p, pid, name.c_str(), &e, wantBytes ? buf.data() : nullptr,
            wantBytes, wantBytes ? &blen : nullptr);
        if (rc != ASMTEST_PTRACE_OK)
            return std::nullopt;
        JitMethod m;
        m.code_addr = e.code_addr;
        m.code_size = e.code_size;
        m.timestamp = e.timestamp;
        m.code_index = e.code_index;
        if (wantBytes)
            m.code.assign(buf.begin(), buf.begin() + blen);
        return m;
    }
};

/// A code-emission event from the optional eBPF detector. Byte-compatible mirror
/// of asmtest_codeimage_event_t (40 bytes: three uint64, three uint32, one int32 —
/// no trailing padding on x86-64): when and where new executable code appeared for
/// the watched pid, never the instruction stream itself.
struct CodeImageEvent {
    std::uint64_t addr = 0;       // published base address (0 for a memfd hint)
    std::uint64_t len = 0;        // byte length (0 for a memfd hint)
    std::uint64_t timestamp = 0;  // bpf_ktime_get_ns() at emission
    std::uint32_t pid = 0;        // tgid that published
    std::uint32_t tid = 0;        // thread that published
    std::uint32_t kind = 0;       // ASMTEST_CI_KIND_*
    std::int32_t fd = -1;         // memfd fd, or -1
};

/// The time-aware code-image recorder (asmtest_codeimage.h): a userspace
/// PERF_RECORD_TEXT_POKE. track() snapshots a region's bytes (version 0) and arms
/// write-protect-async on its pages; refresh() re-snapshots only the pages written
/// since the last arm as new versions stamped with a monotonic sequence; and
/// bytes_at(addr, when) answers "what bytes were live at addr as of sequence
/// `when`" — the query a branch-trace decoder or the W2 block-normalizer needs to
/// reconstruct a JIT method whose address was reused. Change detection is pure
/// userspace and works on a FOREIGN process (pid 0 records this process). The
/// optional eBPF emission detector self-skips cleanly without libbpf / CAP_BPF.
/// Move-only; the ctor opens a timeline (asmtest_codeimage_new), the dtor frees it.
class CodeImage {
  public:
    /// Open a timeline recording `pid`'s memory (pid == 0 => this process). Throws
    /// std::runtime_error if the recorder is unavailable or allocation fails (call
    /// available() first to self-skip). Like the C API, the caller owns any ptrace
    /// attach policy for a foreign pid; the recorder only reads memory.
    explicit CodeImage(int pid = 0) {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            throw std::runtime_error(
                "libasmtest_hwtrace not found (set ASMTEST_HWTRACE_LIB)");
        img_ = a.ci_new(pid);
        if (!img_)
            throw std::runtime_error("asmtest_codeimage_new returned NULL");
    }

    CodeImage(const CodeImage &) = delete;
    CodeImage &operator=(const CodeImage &) = delete;
    CodeImage(CodeImage &&o) noexcept : img_(o.img_) { o.img_ = nullptr; }
    CodeImage &operator=(CodeImage &&o) noexcept {
        if (this != &o) {
            free();
            img_ = o.img_;
            o.img_ = nullptr;
        }
        return *this;
    }
    ~CodeImage() { free(); }

    // ---- userspace recorder (always available on a supporting host) ----

    /// True if the userspace recorder can detect page changes on this host
    /// (PAGEMAP_SCAN, or the soft-dirty fallback). Never crashes or throws — a
    /// missing library reports false. Like HwTrace::available().
    static bool available() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return false;
        return a.ci_available() != 0;
    }

    /// Human-readable reason available() is false. Returns a fixed string when the
    /// library itself is missing.
    static std::string skip_reason() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return "libasmtest_hwtrace not found (set ASMTEST_HWTRACE_LIB)";
        char buf[256] = {0};
        a.ci_skip_reason(buf, sizeof(buf));
        return std::string(buf);
    }

    /// Begin tracking [base, base+len): snapshot version 0 now and arm
    /// write-protect-async on its pages. May be called for several disjoint
    /// regions. Returns ASMTEST_CI_OK or a negative CodeImageStatus.
    int track(const void *base, std::size_t len) {
        return detail::api().ci_track(img_, base, len);
    }

    /// Re-snapshot pages changed since the last arm as new versions and re-arm.
    /// Returns the number of new versions recorded (>= 0), or a negative status.
    int refresh() { return detail::api().ci_refresh(img_); }

    /// The current capture sequence — a monotonic logical timestamp. Advances by
    /// one for every version recorded (track + each refresh change). 0 before
    /// anything is tracked.
    std::uint64_t now() const { return detail::api().ci_now(img_); }

    /// The bytes live at `addr` as of capture sequence `when` (when == 0 => the
    /// latest version), as a copy. Empty when `addr` is not in any tracked region
    /// or there is no version at/before `when` (ASMTEST_CI_ENOENT). On a different
    /// negative status this throws.
    std::vector<std::uint8_t> bytes_at(const void *addr,
                                       std::uint64_t when = 0) const {
        const std::uint8_t *out = nullptr;
        std::size_t out_len = 0;
        int rc = detail::api().ci_bytes_at(img_, addr, when, &out, &out_len);
        if (rc == ASMTEST_CI_ENOENT)
            return {};
        if (rc != ASMTEST_CI_OK)
            throw std::runtime_error("asmtest_codeimage_bytes_at failed: " +
                                     std::to_string(rc));
        return std::vector<std::uint8_t>(out, out + out_len);
    }

    // ---- optional eBPF emission detector (self-skips without libbpf) ----

    /// True if the eBPF emission detector can load and attach on this host (built
    /// with libbpf, kernel BTF present, sufficient privilege). Never throws.
    static bool bpf_available() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return false;
        return a.ci_bpf_available() != 0;
    }

    /// Human-readable reason bpf_available() is false. Fixed string when the
    /// library itself is missing.
    static std::string bpf_skip_reason() {
        detail::HwApi &a = detail::api();
        if (!a.loaded())
            return "libasmtest_hwtrace not found (set ASMTEST_HWTRACE_LIB)";
        char buf[256] = {0};
        a.ci_bpf_skip_reason(buf, sizeof(buf));
        return std::string(buf);
    }

    /// Load the CO-RE program, filter it to this timeline's pid, and attach it.
    /// Returns ASMTEST_CI_OK, ASMTEST_CI_ENOSYS, ASMTEST_CI_EUNAVAIL, or
    /// ASMTEST_CI_ELOAD.
    int watch_bpf() { return detail::api().ci_watch_bpf(img_); }

    /// Drain ready emission events into the internal queue. timeout_ms == 0 is a
    /// non-blocking drain. Returns the number of events queued (>= 0) or a negative
    /// status.
    int poll_bpf(int timeout_ms) {
        return detail::api().ci_poll_bpf(img_, timeout_ms);
    }

    /// Pop one queued emission event. Returns the event if one was available, else
    /// std::nullopt (queue empty). Throws on a negative status.
    std::optional<CodeImageEvent> next_event() {
        /* The C struct is the 40-byte mirror of CodeImageEvent; fill it directly
         * (no separate POD type needed since the layout matches field-for-field). */
        CodeImageEvent ev{};
        int rc = detail::api().ci_next(img_, &ev);
        if (rc < 0)
            throw std::runtime_error("asmtest_codeimage_next failed: " +
                                     std::to_string(rc));
        if (rc == 0)
            return std::nullopt;
        return ev;
    }

    void free() {
        if (img_) {
            detail::api().ci_free(img_);
            img_ = nullptr;
        }
    }

  private:
    friend class Ptrace;
    /// The opaque asmtest_codeimage* handle, as the versioned tracer needs it.
    void *raw() const { return img_; }

    void *img_ = nullptr;
};

/// Out-of-line because it references CodeImage, which is defined above only after
/// Ptrace. Mirrors traceAttached but passes the recorder + `when` so the decoder
/// reads versioned bytes instead of a live snapshot.
inline long Ptrace::traceAttachedVersioned(int pid, const void *base,
                                           std::size_t len,
                                           const CodeImage &img,
                                           std::uint64_t when,
                                           const HwTrace &trace) {
    long result = 0;
    int rc = detail::require().ptrace_trace_attached_versioned(
        pid, base, len, img.raw(), when, &result, trace.raw());
    if (rc != ASMTEST_PTRACE_OK)
        throw std::runtime_error(
            "asmtest_ptrace_trace_attached_versioned failed: " +
            std::to_string(rc));
    return result;
}

/// Out-of-line because it references CodeImage (defined above only after Ptrace)
/// and threads a Descent handle. Mirrors traceAttachedVersioned but records edges +
/// nested frames into `descent` (either handle may be nullptr).
inline long Ptrace::traceAttachedVersionedEx(int pid, const void *base,
                                             std::size_t len,
                                             const CodeImage &img,
                                             std::uint64_t when, HwTrace *trace,
                                             Descent *descent) {
    long result = 0;
    int rc = detail::require().ptrace_trace_attached_versioned_ex(
        pid, base, len, img.raw(), when, &result,
        trace ? trace->raw() : nullptr, descent ? descent->raw() : nullptr);
    if (rc != ASMTEST_PTRACE_OK)
        throw std::runtime_error(
            "asmtest_ptrace_trace_attached_versioned_ex failed: " +
            std::to_string(rc));
    return result;
}

}  // namespace asmtest

#endif  // ASMTEST_HWTRACE_HPP
