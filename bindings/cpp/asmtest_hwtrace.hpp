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
#include <stdexcept>
#include <string>
#include <vector>

#include "asmtest_hwtrace.h"

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
    explicit HwTrace(void *h) : handle_(h) {}

    void *handle_ = nullptr;
};

}  // namespace asmtest

#endif  // ASMTEST_HWTRACE_HPP
