/*
 * asmtest.hpp — thin C++ convenience layer over asm-test (Track X).
 *
 * The C headers are already C++-consumable (they carry extern "C" guards), so a
 * C++ project can just #include "asmtest.h" and use the framework directly. This
 * optional header adds RAII and typed ergonomics on top: a scope-guarded
 * emulator, std::initializer_list arg passing, vector-lane helpers, and verdict
 * predicates that read naturally in a C++ assertion.
 *
 *   #include "asmtest.hpp"
 *   auto r = asmtest::capture((void*)add_signed, {40, 2});
 *   ASSERT_EQ(r.ret, 42);
 *   ASSERT_TRUE(asmtest::abi_preserved(r));
 *
 * Define ASMTEST_ENABLE_EMU (and link the emulator) before including to get the
 * asmtest::Emu wrapper.
 */
#ifndef ASMTEST_HPP
#define ASMTEST_HPP

#include "asmtest.h"
#ifdef ASMTEST_ENABLE_EMU
#include "asmtest_emu.h"
#endif
#ifdef ASMTEST_ENABLE_ASM
#include "asmtest_assemble.h"  // in-line assembler (Keystone); pulls in asmtest_emu.h
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace asmtest {

namespace detail {
inline void fill6(long (&dst)[6], std::initializer_list<long> src) {
    long *p = dst;
    long *end = dst + 6;
    for (long v : src) {
        if (p == end)
            break;
        *p++ = v;
    }
    while (p != end)
        *p++ = 0;
}
}  // namespace detail

// --- Capture tier ---------------------------------------------------------- //

/// Call `fn` through the integer ABI (up to 6 args) and return the snapshot.
inline regs_t capture(void *fn, std::initializer_list<long> args = {}) {
    regs_t r{};
    long a[6];
    detail::fill6(a, args);
    asm_call_capture(&r, fn, a);
    return r;
}

/// Call `fn` marshalling up to 8 doubles into the FP argument registers.
inline regs_t capture_fp(void *fn, std::initializer_list<long> iargs,
                         std::initializer_list<double> fargs) {
    regs_t r{};
    long ia[6];
    detail::fill6(ia, iargs);
    double fa[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    double *p = fa;
    for (double v : fargs) {
        if (p == fa + 8)
            break;
        *p++ = v;
    }
    asm_call_capture_fp(&r, fn, ia, fa);
    return r;
}

/// Pack four float32 lanes into a 128-bit vector argument.
inline vec128_t vec_f32(float a, float b, float c, float d) {
    vec128_t v{};
    v.f32[0] = a;
    v.f32[1] = b;
    v.f32[2] = c;
    v.f32[3] = d;
    return v;
}

/// Call `fn` marshalling up to 8 128-bit vectors into the vector arg registers.
inline regs_t capture_vec(void *fn, std::initializer_list<long> iargs,
                          std::initializer_list<vec128_t> vargs) {
    regs_t r{};
    long ia[6];
    detail::fill6(ia, iargs);
    std::array<vec128_t, 8> va{};
    std::size_t i = 0;
    for (const vec128_t &v : vargs) {
        if (i == va.size())
            break;
        va[i++] = v;
    }
    asm_call_capture_vec(&r, fn, ia, va.data());
    return r;
}

/// All callee-saved registers restored — via the native non-jumping verdict shim.
inline bool abi_preserved(const regs_t &r) {
    return asmtest_check_abi(&r, nullptr, 0) == 0;
}

/// True if condition flag bit(s) `mask` (ASMTEST_CF, …) are set.
inline bool flag_set(const regs_t &r, unsigned long mask) {
    return (r.flags & mask) != 0;
}

// --- Tier-2 idiomatic assertions ------------------------------------------- //
// Throw on failure with a clear message — usable from GoogleTest/Catch2/doctest
// (or any C++ test runner) where an exception is the natural failure signal.

/// Thrown by the assert_* helpers below.
struct assertion_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void assert_ret(const regs_t &r, unsigned long expected) {
    if (r.ret != expected)
        throw assertion_error("ret: got " + std::to_string(r.ret) + ", want " +
                              std::to_string(expected));
}
inline void assert_abi_preserved(const regs_t &r) {
    if (!abi_preserved(r))
        throw assertion_error("ABI not preserved: a callee-saved register was "
                              "not restored");
}
inline void assert_flag(const regs_t &r, unsigned long mask, bool set = true) {
    if (flag_set(r, mask) != set)
        throw assertion_error("condition flag mismatch");
}
inline void assert_fp(const regs_t &r, double expected) {
    if (r.fret != expected)
        throw assertion_error("FP return mismatch");
}

#ifdef ASMTEST_ENABLE_ASM
/// Thrown by the assembler helpers when a string fails to assemble.
struct asm_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
#endif  // ASMTEST_ENABLE_ASM

#ifdef ASMTEST_ENABLE_EMU
// --- Emulator tier --------------------------------------------------------- //

/// RAII guard over an emu_t handle (x86-64 guest). Move-only.
class Emu {
  public:
    Emu() : e_(emu_open()) {}
    ~Emu() {
        if (e_)
            emu_close(e_);
    }
    Emu(const Emu &) = delete;
    Emu &operator=(const Emu &) = delete;
    Emu(Emu &&o) noexcept : e_(o.e_) { o.e_ = nullptr; }
    Emu &operator=(Emu &&o) noexcept {
        if (this != &o) {
            if (e_)
                emu_close(e_);
            e_ = o.e_;
            o.e_ = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return e_ != nullptr; }
    emu_t *get() const { return e_; }

    /// Copy `code_len` bytes of `fn` and run with integer args; faults are data.
    emu_result_t call(void *fn, std::initializer_list<long> args = {},
                      std::uint64_t max_insns = 0, std::size_t code_len = 64) {
        emu_result_t res{};
        std::vector<long> a(args);
        if (a.empty())
            a.push_back(0);
        emu_call(e_, fn, code_len, a.data(),
                 static_cast<int>(args.size()), max_insns, &res);
        return res;
    }

    /// Run raw machine-code `code` (length `len`) with up to six integer args —
    /// more than the two of `call`. Faults are data.
    emu_result_t call_bytes(const void *code, std::size_t len,
                            std::initializer_list<long> args = {},
                            std::uint64_t max_insns = 0) {
        emu_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_call(e_, code, len, a, static_cast<int>(args.size()), max_insns,
                 &res);
        return res;
    }

    /// Run raw bytes marshalling doubles into the FP arg registers (xmm0..7);
    /// the scalar double return is res.regs.xmm[0].f64[0].
    emu_result_t call_fp(const void *code, std::size_t len,
                         std::initializer_list<long> iargs,
                         std::initializer_list<double> fargs,
                         std::uint64_t max_insns = 0) {
        emu_result_t res{};
        long ia[6];
        detail::fill6(ia, iargs);
        double fa[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        double *p = fa;
        for (double v : fargs) {
            if (p == fa + 8)
                break;
            *p++ = v;
        }
        emu_call_fp(e_, code, len, ia, static_cast<int>(iargs.size()), fa,
                    static_cast<int>(fargs.size()), max_insns, &res);
        return res;
    }

    /// Run raw bytes marshalling 128-bit vectors into xmm0..7; a vector return is
    /// res.regs.xmm[0].
    emu_result_t call_vec(const void *code, std::size_t len,
                          std::initializer_list<long> iargs,
                          std::initializer_list<vec128_t> vargs,
                          std::uint64_t max_insns = 0) {
        emu_result_t res{};
        long ia[6];
        detail::fill6(ia, iargs);
        std::array<emu_vec128_t, 8> va{};
        std::size_t i = 0;
        for (const vec128_t &v : vargs) {
            if (i == va.size())
                break;
            for (int l = 0; l < 4; ++l)
                va[i].f32[l] = v.f32[l];
            ++i;
        }
        emu_call_vec(e_, code, len, ia, static_cast<int>(iargs.size()),
                     va.data(), static_cast<int>(vargs.size()), max_insns, &res);
        return res;
    }

    /// Run raw bytes under the Microsoft x64 (Win64) convention — integer args in
    /// rcx, rdx, r8, r9 — so a Win64 routine can be tested on a System V host.
    emu_result_t call_win64(const void *code, std::size_t len,
                            std::initializer_list<long> args = {},
                            std::uint64_t max_insns = 0) {
        emu_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_call_win64(e_, code, len, a, static_cast<int>(args.size()),
                       max_insns, &res);
        return res;
    }

    /// Like call_bytes, but record an execution trace / coverage into `tr`.
    emu_result_t call_traced(const void *code, std::size_t len, emu_trace_t *tr,
                             std::initializer_list<long> args = {},
                             std::uint64_t max_insns = 0) {
        emu_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_call_traced(e_, code, len, a, static_cast<int>(args.size()),
                        max_insns, &res, tr);
        return res;
    }

#ifdef ASMTEST_ENABLE_ASM
    /// Assemble x86-64 `src` in `syntax` via Keystone and run it with up to six
    /// integer args, stopping after `max_insns` instructions (0 = run to `ret`).
    /// Returns the run's result; throws asm_error with the Keystone diagnostic
    /// if `src` fails to assemble.
    emu_result_t call_asm(const char *src,
                          std::initializer_list<long> args = {},
                          asm_syntax_t syntax = ASM_SYNTAX_INTEL,
                          std::uint64_t max_insns = 0) {
        emu_result_t res{};
        long a[6] = {0, 0, 0, 0, 0, 0};
        int n = 0;
        for (long v : args) {
            if (n == 6)
                break;
            a[n++] = v;
        }
        int ok = asmtest_emu_call_asm6(e_, src, static_cast<int>(syntax),
                                       a[0], a[1], a[2], a[3], a[4], a[5], n,
                                       max_insns, &res);
        if (!ok)
            throw asm_error(std::string("in-line assembly failed: ") +
                            asmtest_asm_last_error());
        return res;
    }
#endif  // ASMTEST_ENABLE_ASM

  private:
    emu_t *e_;
};

// --- Execution trace / coverage -------------------------------------------- //

/// RAII execution-trace / basic-block coverage recorder. Pass get() to
/// Emu::call_traced or a guest's call_traced, then ask which block byte-offsets
/// were entered.
class Trace {
  public:
    explicit Trace(std::size_t insns_cap = 4096, std::size_t blocks_cap = 4096)
        : insns_(insns_cap), blocks_(blocks_cap), t_{} {
        t_.insns = insns_.data();
        t_.insns_cap = insns_cap;
        t_.blocks = blocks_.data();
        t_.blocks_cap = blocks_cap;
    }
    Trace(const Trace &) = delete;
    Trace &operator=(const Trace &) = delete;

    emu_trace_t *get() { return &t_; }
    bool covered(std::uint64_t off) const { return emu_trace_covered(&t_, off); }
    std::uint64_t insns_total() const { return t_.insns_total; }
    std::size_t blocks_len() const { return t_.blocks_len; }

  private:
    std::vector<std::uint64_t> insns_, blocks_;
    emu_trace_t t_;
};

// --- Cross-arch emulator guests (raw bytes, any host) ---------------------- //
// Each runs raw machine-code bytes through its ISA's Unicorn guest and returns
// the typed per-arch result (read res.regs.x[0] / .a-regs / .r[0] directly).

/// AArch64 guest. Move-disabled RAII over emu_arm64_t.
class Arm64Emu {
  public:
    Arm64Emu() : e_(emu_arm64_open()) {}
    ~Arm64Emu() {
        if (e_)
            emu_arm64_close(e_);
    }
    Arm64Emu(const Arm64Emu &) = delete;
    Arm64Emu &operator=(const Arm64Emu &) = delete;
    explicit operator bool() const { return e_ != nullptr; }

    emu_arm64_result_t call(const void *code, std::size_t len,
                            std::initializer_list<long> args = {},
                            std::uint64_t max_insns = 0) {
        emu_arm64_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_arm64_call(e_, code, len, a, static_cast<int>(args.size()),
                       max_insns, &res);
        return res;
    }
    emu_arm64_result_t call_traced(const void *code, std::size_t len,
                                   emu_trace_t *tr,
                                   std::initializer_list<long> args = {},
                                   std::uint64_t max_insns = 0) {
        emu_arm64_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_arm64_call_traced(e_, code, len, a, static_cast<int>(args.size()),
                              max_insns, &res, tr);
        return res;
    }

  private:
    emu_arm64_t *e_;
};

/// RISC-V (RV64) guest.
class RiscvEmu {
  public:
    RiscvEmu() : e_(emu_riscv_open()) {}
    ~RiscvEmu() {
        if (e_)
            emu_riscv_close(e_);
    }
    RiscvEmu(const RiscvEmu &) = delete;
    RiscvEmu &operator=(const RiscvEmu &) = delete;
    explicit operator bool() const { return e_ != nullptr; }

    emu_riscv_result_t call(const void *code, std::size_t len,
                            std::initializer_list<long> args = {},
                            std::uint64_t max_insns = 0) {
        emu_riscv_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_riscv_call(e_, code, len, a, static_cast<int>(args.size()),
                       max_insns, &res);
        return res;
    }

  private:
    emu_riscv_t *e_;
};

/// ARM32 (A32) guest.
class ArmEmu {
  public:
    ArmEmu() : e_(emu_arm_open()) {}
    ~ArmEmu() {
        if (e_)
            emu_arm_close(e_);
    }
    ArmEmu(const ArmEmu &) = delete;
    ArmEmu &operator=(const ArmEmu &) = delete;
    explicit operator bool() const { return e_ != nullptr; }

    emu_arm_result_t call(const void *code, std::size_t len,
                          std::initializer_list<long> args = {},
                          std::uint64_t max_insns = 0) {
        emu_arm_result_t res{};
        long a[6];
        detail::fill6(a, args);
        emu_arm_call(e_, code, len, a, static_cast<int>(args.size()), max_insns,
                     &res);
        return res;
    }

  private:
    emu_arm_t *e_;
};
#endif  // ASMTEST_ENABLE_EMU

#ifdef ASMTEST_ENABLE_ASM
/// Assemble `src` for `arch`/`syntax` at load address `addr` and return the
/// machine-code bytes. Multi-arch (unlike Emu::call_asm, which runs on the
/// x86-64 guest). Throws asm_error with the Keystone diagnostic on failure.
inline std::vector<std::uint8_t> assemble(const char *src,
                                          asm_arch_t arch = ASM_X86_64,
                                          asm_syntax_t syntax = ASM_SYNTAX_INTEL,
                                          std::uint64_t addr = EMU_CODE_BASE) {
    std::vector<std::uint8_t> buf(256);
    int n = asmtest_asm_bytes(static_cast<int>(arch), static_cast<int>(syntax),
                              src, addr, buf.data(),
                              static_cast<int>(buf.size()));
    if (n == 0)
        throw asm_error(std::string("assemble failed: ") +
                        asmtest_asm_last_error());
    if (static_cast<std::size_t>(n) > buf.size()) {
        buf.resize(n);
        n = asmtest_asm_bytes(static_cast<int>(arch), static_cast<int>(syntax),
                              src, addr, buf.data(), n);
    }
    buf.resize(n);
    return buf;
}
#endif  // ASMTEST_ENABLE_ASM

}  // namespace asmtest

#endif  // ASMTEST_HPP
