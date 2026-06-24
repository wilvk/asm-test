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

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
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

  private:
    emu_t *e_;
};
#endif  // ASMTEST_ENABLE_EMU

}  // namespace asmtest

#endif  // ASMTEST_HPP
