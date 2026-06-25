/*
 * test_cpp.cpp — Track X example suite: the asm-test framework used from C++.
 *
 * Demonstrates that a C++ translation unit can drive the framework directly —
 * the TEST/ASSERT macros, the capture trampoline, and the emulator all work
 * through the C++-compatible headers — with the asmtest.hpp conveniences
 * (initializer-list args, RAII Emu, verdict predicates) on top. Built and run by
 * `make cpp-test`; linked against the same framework objects as the C suites.
 */
#include "asmtest.hpp"

// Routines under test (examples/{add,flags,fp,simd}.s), C symbols.
extern "C" {
long add_signed(long, long);
long set_carry(void);
long clobbers_rbx(long, long);
double fp_add(double, double);
void vec_add4f(void);  // vec128 in/out; only its address is needed
long read_fault(const long *);  // loads *p; faults if p is unmapped
double int_to_double(long);     // (double)n into xmm0 from an integer arg
}

using namespace asmtest;

TEST(cpp, capture_int_and_abi) {
    regs_t r = capture(reinterpret_cast<void *>(add_signed), {40, 2});
    ASSERT_EQ(r.ret, 42);
    ASSERT_TRUE(abi_preserved(r));
}

TEST(cpp, abi_violation_is_observable) {
    regs_t r = capture(reinterpret_cast<void *>(clobbers_rbx), {1, 2});
    // The verdict predicate reports the violation rather than aborting.
    ASSERT_FALSE(abi_preserved(r));
}

TEST(cpp, flags) {
    regs_t r = capture(reinterpret_cast<void *>(set_carry), {});
    ASSERT_TRUE(flag_set(r, ASMTEST_CF));
    ASSERT_FLAG_SET(&r, CF);  // the C macro works in C++ too
}

TEST(cpp, fp_return) {
    regs_t r = capture_fp(reinterpret_cast<void *>(fp_add), {}, {1.5, 2.25});
    ASSERT_FP_EQ(&r, 3.75);
}

// Tier-2 assert_* helpers: the pass paths succeed and the failure path throws.
TEST(cpp, tier2_assertions) {
    regs_t r = capture(reinterpret_cast<void *>(add_signed), {40, 2});
    assert_ret(r, 42);
    assert_abi_preserved(r);
    assert_flag(r, ASMTEST_CF, false);
    regs_t fr = capture_fp(reinterpret_cast<void *>(fp_add), {}, {1.5, 2.25});
    assert_fp(fr, 3.75);

    bool threw = false;
    try {
        assert_ret(r, 99); // wrong on purpose
    } catch (const asmtest::assertion_error &) {
        threw = true;
    }
    ASSERT_TRUE(threw);
}

TEST(cpp, vector_lanes) {
    regs_t r = capture_vec(reinterpret_cast<void *>(vec_add4f), {},
                           {vec_f32(1, 2, 3, 4), vec_f32(10, 20, 30, 40)});
    ASSERT_FEQ(r.vec[0].f32[0], 11.0f);
    ASSERT_FEQ(r.vec[0].f32[3], 44.0f);
}

#if defined(__x86_64__)
// The emulator's x86-64 guest runs the host-compiled routine bytes (valid input
// only on an x86-64 host, like the C `emu` suite); fault-as-data on other hosts.
TEST(cpp, emulator_raii) {
    Emu e;
    ASSERT_TRUE(static_cast<bool>(e));
    emu_result_t res = e.call(reinterpret_cast<void *>(add_signed), {40, 2});
    ASSERT_NO_FAULT(&res);
    ASSERT_EMU_REG_EQ(&res, rax, 42);
}  // e closes the handle here

// read_fault dereferences an unmapped address: the fault is data — where
// (fault_addr) and why (fault_kind) — not a crash.
TEST(cpp, emulator_fault_is_data) {
    constexpr unsigned long kFaultAddr = 0x00DEAD00UL;
    Emu e;
    long addr = static_cast<long>(kFaultAddr);
    emu_result_t res = e.call(reinterpret_cast<void *>(read_fault), {addr});
    ASSERT_TRUE(res.faulted);
    ASSERT_EQ(res.fault_addr, kFaultAddr);
    ASSERT_EQ(res.fault_kind, EMU_FAULT_READ);
}

// int_to_double lands (double)42 in xmm0, so the XMM file is readable beyond the
// GP registers; a clean run also keeps rflags live (x86 holds bit 1 set).
TEST(cpp, emulator_xmm_and_rflags) {
    Emu e;
    emu_result_t res = e.call(reinterpret_cast<void *>(int_to_double), {42});
    ASSERT_FALSE(res.faulted);
    ASSERT_EQ(res.regs.xmm[0].f64[0], 42.0);
    ASSERT_TRUE((res.regs.rflags & 0x2u) != 0);
}

#ifdef ASMTEST_ENABLE_ASM
// In-line assembler (Keystone): assemble source text and run it on the guest.
TEST(cpp, inline_assembler) {
    Emu e;
    emu_result_t res = e.call_asm("mov rax, rdi; add rax, rsi; ret", {40, 2});
    ASSERT_NO_FAULT(&res);
    ASSERT_EMU_REG_EQ(&res, rax, 42);

    // Widened shim: AT&T syntax + a third arg (rdi+rsi+rdx).
    emu_result_t att = e.call_asm(
        "mov %rdi, %rax; add %rsi, %rax; add %rdx, %rax; ret",
        {10, 20, 12}, ASM_SYNTAX_ATT);
    ASSERT_EMU_REG_EQ(&att, rax, 42);

    // Failure path: a bad string throws asm_error with the Keystone diagnostic.
    bool threw = false;
    try {
        e.call_asm("mov rax, nonsense_token");
    } catch (const asmtest::asm_error &ex) {
        threw = std::string(ex.what()).size() > sizeof("in-line assembly failed: ") - 1;
    }
    ASSERT_TRUE(threw);

    // Multi-arch assemble-to-bytes: AArch64 `ret` is C0 03 5F D6.
    std::vector<std::uint8_t> a64 = assemble("ret", ASM_ARM64);
    ASSERT_TRUE(a64.size() == 4 && a64[0] == 0xC0 && a64[3] == 0xD6);
}
#endif  // ASMTEST_ENABLE_ASM
#endif
