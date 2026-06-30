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
    int (*ptrace_trace_attached)(int, const void *, size_t, long *,
                                 void *) = nullptr;
    int (*ptrace_run_to)(int, const void *) = nullptr;
    int (*proc_region_by_addr)(int, const void *, void **, size_t *) = nullptr;
    int (*proc_perfmap_symbol)(int, const char *, void **, size_t *) = nullptr;
    int (*jitdump_find)(const char *, int, const char *, void *, uint8_t *,
                        size_t, size_t *) = nullptr;

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
        ok &= dlsym_into(h, "asmtest_ptrace_trace_attached",
                         t.ptrace_trace_attached);
        ok &= dlsym_into(h, "asmtest_ptrace_run_to", t.ptrace_run_to);
        ok &= dlsym_into(h, "asmtest_proc_region_by_addr",
                         t.proc_region_by_addr);
        ok &= dlsym_into(h, "asmtest_proc_perfmap_symbol",
                         t.proc_perfmap_symbol);
        ok &= dlsym_into(h, "asmtest_jitdump_find", t.jitdump_find);
        if (!ok) {
            ::dlclose(h);
            return HwApi{};  // a fresh, empty table -> available() == false
        }
        t.handle = h;
        return t;
    }();
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
        detail::HwApi &a = detail::api();
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
        detail::api().begin(name_.c_str());
    }

    std::string name_;
    bool active_;
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
    /// when the library is missing. CEILING_FREE drops the depth-bounded backend
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
        int rc = detail::api().init(&opts);
        if (rc != ASMTEST_HW_OK)
            throw std::runtime_error("asmtest_hwtrace_init failed: " +
                                     std::to_string(rc));
    }

    static void shutdown() { detail::api().shutdown(); }

    // ---- per-trace ----

    /// Allocate a trace: `blocks` block slots and `instructions` instruction
    /// slots (instruction recording active when instructions > 0). Note the C
    /// ABI takes insns first, blocks second. Throws on allocation failure.
    /// (`new` is reserved, so this is `create`.)
    static HwTrace create(std::size_t blocks = 64,
                          std::size_t instructions = 64) {
        void *h = detail::api().trace_new(instructions, blocks);
        if (!h)
            throw std::runtime_error("asmtest_trace_new returned NULL");
        return HwTrace(h);
    }

    /// Register a non-overlapping native code range under `name`, recording
    /// coverage into this trace. Throws std::runtime_error on failure.
    void register_region(const std::string &name, const NativeCode &code) {
        int rc = detail::api().register_region(name.c_str(), code.base(),
                                               code.length(), handle_);
        if (rc != ASMTEST_HW_OK)
            throw std::runtime_error("register_region(" + name +
                                     ") failed: " + std::to_string(rc));
    }

    /// A scoped begin/end marker for `name`. Use it in a tight block:
    /// `{ auto s = tr.region("add"); code.call(...); }`.
    RegionScope region(const std::string &name) { return RegionScope(name); }

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
        int rc = detail::api().ptrace_trace_call(
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

    /// Trace a region in a SEPARATE, already-ptrace-stopped process (the caller
    /// owns PTRACE_ATTACH/DETACH). Reads the target's bytes via process_vm_readv.
    /// Returns the target's RAX at the region exit. Throws on a nonzero status.
    static long traceAttached(int pid, const void *base, std::size_t len,
                              const HwTrace &trace) {
        long result = 0;
        int rc = detail::api().ptrace_trace_attached(pid, base, len, &result,
                                                     trace.raw());
        if (rc != ASMTEST_PTRACE_OK)
            throw std::runtime_error("asmtest_ptrace_trace_attached failed: " +
                                     std::to_string(rc));
        return result;
    }

    /// Run an already-attached, ptrace-stopped target forward until it reaches
    /// `addr` (a software breakpoint that fires when the program itself next calls
    /// in), leaving it stopped there ready for traceAttached. Returns the status:
    /// ASMTEST_PTRACE_OK, or ASMTEST_PTRACE_ENOENT if the target exited first. The
    /// caller owns PTRACE_ATTACH/DETACH.
    static int runTo(int pid, const void *addr) {
        return detail::api().ptrace_run_to(pid, addr);
    }

    /// The executable mapping in /proc/<pid>/maps containing `addr`, as
    /// (base, len), or std::nullopt if no executable mapping contains it.
    static std::optional<std::pair<void *, std::size_t>>
    regionByAddr(int pid, const void *addr) {
        void *base = nullptr;
        std::size_t len = 0;
        int rc = detail::api().proc_region_by_addr(pid, addr, &base, &len);
        if (rc != ASMTEST_PTRACE_OK)
            return std::nullopt;
        return std::make_pair(base, len);
    }

    /// A JIT method by `name` in /tmp/perf-<pid>.map, as (base, len), or nullopt.
    static std::optional<std::pair<void *, std::size_t>>
    perfmapSymbol(int pid, const std::string &name) {
        void *base = nullptr;
        std::size_t len = 0;
        int rc = detail::api().proc_perfmap_symbol(pid, name.c_str(), &base,
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
        int rc = detail::api().jitdump_find(
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

}  // namespace asmtest

#endif  // ASMTEST_HWTRACE_HPP
