/*
 * asmtest_drtrace.hpp — header-only C++ wrapper for the optional in-process
 * DynamoRIO native-trace tier (see include/asmtest_drtrace.h and
 * docs/native-tracing.md). The companion to asmtest.hpp's RAII conveniences,
 * mirroring the Python wrapper (bindings/python/asmtest/drtrace.py).
 *
 * Where the emulator tier (asmtest::Trace) traces isolated guest bytes, this
 * tier traces host-native code as it runs **inside this process**: initialize
 * DynamoRIO once, materialize host-native machine code, mark a region, call into
 * it, and read back basic-block coverage / the instruction stream.
 *
 *   #include "asmtest_drtrace.hpp"
 *   using namespace asmtest;
 *   if (!NativeTrace::available()) return 0;          // self-skip cleanly
 *   NativeTrace::initialize();                        // dr_init + dr_start
 *   NativeCode code = NativeCode::from_bytes(bytes);
 *   NativeTrace tr = NativeTrace::create(64);         // 64 block slots
 *   tr.register_region("add", code);
 *   { auto scope = tr.region("add"); long r = code.call(20, 22); }
 *   bool hit = tr.covered(0);
 *   NativeTrace::shutdown();
 *
 * Unlike asmtest.hpp, this header links NOTHING at build time: it dlopen()s
 * libasmtest_drapp.so at runtime (resolved via $ASMTEST_DRAPP_LIB, else
 * <repo>/build/libasmtest_drapp.so) and dlsym()s the C API, because the drapp
 * library needs DynamoRIO and may be absent. Link only -ldl. When the library
 * (or libdynamorio behind it) is unavailable, NativeTrace::available() returns
 * false and callers self-skip; nothing here crashes or throws on a missing lib.
 *
 * Advanced, Linux-x86-64-only, and opt-in.
 */
#ifndef ASMTEST_DRTRACE_HPP
#define ASMTEST_DRTRACE_HPP

#include <dlfcn.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "asmtest_drtrace.h"

namespace asmtest {

namespace detail {

/* The C symbols of libasmtest_drapp, resolved once via dlsym. Mirrors the
 * _declare() table in the Python wrapper. */
struct DrApi {
    void *handle = nullptr;

    int (*dr_available)(void) = nullptr;
    int (*dr_init)(const asmtest_drtrace_options_t *) = nullptr;
    int (*dr_start)(void) = nullptr;
    int (*dr_stop)(void) = nullptr;
    void (*dr_shutdown)(void) = nullptr;
    int (*dr_register_region)(const char *, void *, size_t,
                              asmtest_trace_t *) = nullptr;
    int (*dr_unregister_region)(const char *) = nullptr;
    void (*trace_begin)(const char *) = nullptr;
    void (*trace_end)(const char *) = nullptr;
    int (*dr_marker_error)(void) = nullptr;
    int (*exec_alloc)(const uint8_t *, size_t, asmtest_exec_code_t *) = nullptr;
    void (*exec_free)(asmtest_exec_code_t *) = nullptr;
    asmtest_trace_t *(*trace_new)(size_t, size_t) = nullptr;
    void (*trace_free)(asmtest_trace_t *) = nullptr;
    int (*trace_covered)(asmtest_trace_t *, uint64_t) = nullptr;
    unsigned long long (*trace_blocks_len)(asmtest_trace_t *) = nullptr;
    unsigned long long (*trace_insns_total)(asmtest_trace_t *) = nullptr;

    /* True if the library loaded and every symbol resolved. */
    bool loaded() const { return handle != nullptr; }
};

template <typename Fn>
inline bool dlsym_into(void *h, const char *name, Fn &slot) {
    /* The cast through void* avoids the ISO C++ object<->function pointer
     * warning; POSIX guarantees dlsym returns a usable function address. */
    void *sym = ::dlsym(h, name);
    slot = reinterpret_cast<Fn>(reinterpret_cast<std::size_t>(sym));
    return sym != nullptr;
}

inline std::string lib_path() {
    if (const char *env = ::getenv("ASMTEST_DRAPP_LIB"))
        if (*env)
            return std::string(env);
    /* Else fall back to <repo>/build/libasmtest_drapp.so. The Python wrapper
     * derives <repo> from __file__; a header has no such anchor at runtime, so
     * we keep it simple and rely on $ASMTEST_DRAPP_LIB (set by the make lanes)
     * or the default loader search path for the bare soname. */
    return std::string("build/libasmtest_drapp.so");
}

/* Process-wide, lazily loaded handle table. Never throws: on any failure it
 * returns a table with handle==nullptr so available() reports false. */
inline DrApi &api() {
    static DrApi a = [] {
        DrApi t;
        void *h = ::dlopen(lib_path().c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            h = ::dlopen("libasmtest_drapp.so", RTLD_NOW | RTLD_GLOBAL);
        if (!h)
            return t;  // handle stays null -> available() == false
        bool ok = true;
        ok &= dlsym_into(h, "asmtest_dr_available", t.dr_available);
        ok &= dlsym_into(h, "asmtest_dr_init", t.dr_init);
        ok &= dlsym_into(h, "asmtest_dr_start", t.dr_start);
        ok &= dlsym_into(h, "asmtest_dr_stop", t.dr_stop);
        ok &= dlsym_into(h, "asmtest_dr_shutdown", t.dr_shutdown);
        ok &= dlsym_into(h, "asmtest_dr_register_region", t.dr_register_region);
        ok &= dlsym_into(h, "asmtest_dr_unregister_region",
                         t.dr_unregister_region);
        ok &= dlsym_into(h, "asmtest_trace_begin", t.trace_begin);
        ok &= dlsym_into(h, "asmtest_trace_end", t.trace_end);
        ok &= dlsym_into(h, "asmtest_dr_marker_error", t.dr_marker_error);
        ok &= dlsym_into(h, "asmtest_exec_alloc", t.exec_alloc);
        ok &= dlsym_into(h, "asmtest_exec_free", t.exec_free);
        ok &= dlsym_into(h, "asmtest_trace_new", t.trace_new);
        ok &= dlsym_into(h, "asmtest_trace_free", t.trace_free);
        ok &= dlsym_into(h, "asmtest_trace_covered", t.trace_covered);
        ok &= dlsym_into(h, "asmtest_emu_trace_blocks_len", t.trace_blocks_len);
        ok &= dlsym_into(h, "asmtest_emu_trace_insns_total",
                         t.trace_insns_total);
        if (!ok) {
            ::dlclose(h);
            return DrApi{};  // a fresh, empty table -> available() == false
        }
        t.handle = h;
        return t;
    }();
    return a;
}

/* nullptr for an empty string -> the C side falls back to its env defaults
 * (ASMTEST_DRCLIENT etc.), matching the Python `(s or "").encode() or None`. */
inline const char *or_null(const std::string &s) {
    return s.empty() ? nullptr : s.c_str();
}

}  // namespace detail

/// Host-native machine code in real executable (W^X) memory. Move-only; frees
/// its mapping in the destructor.
class NativeCode {
  public:
    NativeCode() = default;
    NativeCode(const NativeCode &) = delete;
    NativeCode &operator=(const NativeCode &) = delete;
    NativeCode(NativeCode &&o) noexcept : code_(o.code_), freed_(o.freed_) {
        o.code_ = asmtest_exec_code_t{};
        o.freed_ = true;
    }
    NativeCode &operator=(NativeCode &&o) noexcept {
        if (this != &o) {
            free();
            code_ = o.code_;
            freed_ = o.freed_;
            o.code_ = asmtest_exec_code_t{};
            o.freed_ = true;
        }
        return *this;
    }
    ~NativeCode() { free(); }

    /// Map executable memory and copy `len` bytes of host-native machine code
    /// into it. Throws std::runtime_error on failure.
    static NativeCode from_bytes(const uint8_t *bytes, std::size_t len) {
        detail::DrApi &a = detail::api();
        asmtest_exec_code_t ec{};
        int rc = a.exec_alloc(bytes, len, &ec);
        if (rc != ASMTEST_DR_OK)
            throw std::runtime_error("asmtest_exec_alloc failed: " +
                                     std::to_string(rc));
        return NativeCode(ec);
    }
    static NativeCode from_bytes(const std::vector<uint8_t> &bytes) {
        return from_bytes(bytes.data(), bytes.size());
    }

    void *base() const { return code_.base; }
    std::size_t length() const { return code_.len; }

    /// Invoke the code through a function pointer under the SysV integer ABI
    /// (each arg a long, the result a long). The default `Ret(Args...)` is
    /// `long(long...)`; pass explicit template args for another prototype.
    template <typename Ret = long, typename... Args>
    Ret call(Args... args) const {
        using Fn = Ret (*)(Args...);
        Fn fn = reinterpret_cast<Fn>(code_.base);
        return fn(args...);
    }

    void free() {
        if (!freed_) {
            detail::api().exec_free(&code_);
            freed_ = true;
            code_ = asmtest_exec_code_t{};
        }
    }

  private:
    explicit NativeCode(asmtest_exec_code_t ec) : code_(ec), freed_(false) {}

    asmtest_exec_code_t code_{};
    bool freed_ = true;
};

/// Scoped begin/end marker pair for a named region. The constructor opens
/// recording (asmtest_trace_begin), the destructor closes it (asmtest_trace_end)
/// — so a region is always balanced even if the call between them throws.
/// Move-only; obtained from NativeTrace::region(name).
class RegionScope {
  public:
    RegionScope(const RegionScope &) = delete;
    RegionScope &operator=(const RegionScope &) = delete;
    RegionScope(RegionScope &&o) noexcept : name_(std::move(o.name_)),
                                            active_(o.active_) {
        o.active_ = false;
    }
    RegionScope &operator=(RegionScope &&) = delete;
    ~RegionScope() {
        if (active_)
            detail::api().trace_end(name_.c_str());
    }

  private:
    friend class NativeTrace;
    explicit RegionScope(std::string name)
        : name_(std::move(name)), active_(true) {
        detail::api().trace_begin(name_.c_str());
    }

    std::string name_;
    bool active_;
};

/// An app-owned coverage recorder for a registered native region. Process-wide
/// lifecycle (initialize/shutdown) is static; per-trace state is the instance.
/// Move-only; frees its handle in the destructor.
class NativeTrace {
  public:
    NativeTrace(const NativeTrace &) = delete;
    NativeTrace &operator=(const NativeTrace &) = delete;
    NativeTrace(NativeTrace &&o) noexcept : handle_(o.handle_) {
        o.handle_ = nullptr;
    }
    NativeTrace &operator=(NativeTrace &&o) noexcept {
        if (this != &o) {
            free();
            handle_ = o.handle_;
            o.handle_ = nullptr;
        }
        return *this;
    }
    ~NativeTrace() { free(); }

    // ---- process-wide lifecycle ----

    /// True if the tier can run: the drapp library loaded AND DynamoRIO is
    /// resolvable. Never crashes or throws — a missing library reports false.
    static bool available() {
        detail::DrApi &a = detail::api();
        if (!a.loaded())
            return false;
        return a.dr_available() != 0;
    }

    /// Bring DynamoRIO up in-process and take over (dr_init then dr_start).
    /// `client` is the path to libasmtest_drclient.so (empty -> the C side
    /// falls back to $ASMTEST_DRCLIENT); `dynamorio_home` lets the C side find
    /// libdynamorio (empty -> $ASMTEST_DR_LIB / rpath). Throws std::runtime_error
    /// on a nonzero return code.
    static void initialize(const std::string &client = "",
                           const std::string &dynamorio_home = "",
                           const std::string &client_options = "",
                           int mode = 0) {
        detail::DrApi &a = detail::api();
        asmtest_drtrace_options_t opts{};
        opts.client_path = detail::or_null(client);
        opts.dynamorio_home = detail::or_null(dynamorio_home);
        opts.client_options = detail::or_null(client_options);
        opts.mode = static_cast<asmtest_drtrace_mode_t>(mode);
        int rc = a.dr_init(&opts);
        if (rc != ASMTEST_DR_OK)
            throw std::runtime_error("asmtest_dr_init failed: " +
                                     std::to_string(rc));
        rc = a.dr_start();
        if (rc != ASMTEST_DR_OK)
            throw std::runtime_error("asmtest_dr_start failed: " +
                                     std::to_string(rc));
    }

    static void shutdown() { detail::api().dr_shutdown(); }

    /// Count of illegal marker operations since init (0 = every marker balanced).
    static int marker_error() { return detail::api().dr_marker_error(); }

    // ---- per-trace ----

    /// Allocate a trace: `blocks` block slots and `instructions` instruction
    /// slots (instruction recording active when instructions > 0). Note the C
    /// ABI takes insns first, blocks second. Throws on allocation failure.
    /// (`new` is reserved, so this is `create`.)
    static NativeTrace create(std::size_t blocks = 64,
                              std::size_t instructions = 0) {
        asmtest_trace_t *h = detail::api().trace_new(instructions, blocks);
        if (!h)
            throw std::runtime_error("asmtest_trace_new failed");
        return NativeTrace(h);
    }

    /// Register a non-overlapping native code range under `name`, recording
    /// coverage into this trace. Throws std::runtime_error on failure.
    void register_region(const std::string &name, const NativeCode &code) {
        int rc = detail::api().dr_register_region(
            name.c_str(), code.base(), code.length(), handle_);
        if (rc != ASMTEST_DR_OK)
            throw std::runtime_error("register_region(" + name +
                                     ") failed: " + std::to_string(rc));
    }

    /// Drop the named region (the client forgets its cached translation). Must
    /// happen before freeing the backing NativeCode.
    void unregister(const std::string &name) {
        detail::api().dr_unregister_region(name.c_str());
    }

    /// A scoped begin/end marker for `name`. Use it in a block:
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

    void free() {
        if (handle_) {
            detail::api().trace_free(handle_);
            handle_ = nullptr;
        }
    }

  private:
    explicit NativeTrace(asmtest_trace_t *h) : handle_(h) {}

    asmtest_trace_t *handle_ = nullptr;
};

}  // namespace asmtest

#endif  // ASMTEST_DRTRACE_HPP
