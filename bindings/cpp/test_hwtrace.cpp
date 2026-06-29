/*
 * test_hwtrace.cpp — standalone live test for the single-step hardware-trace
 * backend via the C++ wrapper (asmtest_hwtrace.hpp). Mirrors the Python suite
 * bindings/python/tests/test_hwtrace.py.
 *
 * Unlike the DynamoRIO wrapper (which needs a DynamoRIO install) and the
 * PT/AMD/CoreSight backends (which need specific bare-metal hardware), the
 * SINGLESTEP backend runs on ANY x86-64 Linux — so this asserts a real, live trace
 * here and in CI/containers, self-skipping (prints "# SKIP <reason>", returns 0)
 * only off x86-64 Linux, without Capstone, or when the library is absent.
 *
 * Emits TAP-style "ok N - ..." / "not ok N - ..." lines and exits nonzero on any
 * failure. Build standalone (links only -ldl):
 *
 *   g++ -std=c++17 -I include bindings/cpp/test_hwtrace.cpp -ldl -o test_hwtrace
 *   ASMTEST_HWTRACE_LIB=$PWD/build/libasmtest_hwtrace.so ./test_hwtrace
 *
 * IMPORTANT: under SINGLESTEP, EFLAGS.TF is armed across begin..call..end — the
 * region scopes below keep that window tight (no I/O between begin and end).
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "asmtest_hwtrace.hpp"

using namespace asmtest;

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
static const std::vector<std::uint8_t> ROUTINE = {
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3};

// mov rax,0; L: add rax,rdi; dec rsi; jnz L; ret  (19 back-edges > LBR's 16)
static const std::vector<std::uint8_t> LOOP = {
    0x48, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x48,
    0x01, 0xF8, 0x48, 0xFF, 0xCE, 0x75, 0xF8, 0xC3};

static int g_test = 0;
static int g_failed = 0;

static void ok(bool cond, const char *msg) {
    ++g_test;
    if (cond) {
        std::printf("ok %d - %s\n", g_test, msg);
    } else {
        std::printf("not ok %d - %s\n", g_test, msg);
        ++g_failed;
    }
}

int main() {
    if (!HwTrace::available(SINGLESTEP)) {
        std::printf("# SKIP single-step backend unavailable: %s\n",
                    HwTrace::skip_reason(SINGLESTEP).c_str());
        return 0;
    }

    HwTrace::init(SINGLESTEP);

    // ---- ROUTINE: two-block branch fixture ----
    {
        NativeCode code = NativeCode::from_bytes(ROUTINE);
        HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/64);
        tr.register_region("add2", code);

        long result;
        {
            auto scope = tr.region("add2");
            result = code.call(20, 22);  // 42 <= 100 -> jle taken, dec skipped
        }

        ok(result == 42, "ROUTINE: code.call(20, 22) == 42");
        // Byte-for-byte the Unicorn/DynamoRIO/PT/AMD result for this fixture.
        const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xC, 0x11};
        ok(tr.insn_offsets() == expect,
           "ROUTINE: insn_offsets() == {0, 3, 6, 0xC, 0x11}");
        ok(tr.insns_total() == 5, "ROUTINE: insns_total() == 5");
        ok(tr.covered(0) && tr.covered(0x11),
           "ROUTINE: covered(0) && covered(0x11)");
        ok(tr.blocks_len() == 2, "ROUTINE: blocks_len() == 2");
        ok(!tr.truncated(), "ROUTINE: !truncated()");

        tr.free();
        code.free();
    }

    // ---- LOOP: tight loop past the LBR depth ceiling ----
    {
        NativeCode code = NativeCode::from_bytes(LOOP);
        HwTrace tr = HwTrace::create(/*blocks=*/64, /*instructions=*/256);
        tr.register_region("loop", code);

        long result;
        {
            auto scope = tr.region("loop");
            result = code.call(1, 20);
        }

        ok(result == 20, "LOOP: code.call(1, 20) == 20");
        ok(tr.insns_total() == 62,
           "LOOP: insns_total() == 62 (1 + 20*3 + 1)");
        ok(tr.covered(0) && tr.covered(0x7),
           "LOOP: covered(0) && covered(0x7)");
        ok(tr.blocks_len() == 2, "LOOP: blocks_len() == 2");
        ok(!tr.truncated(), "LOOP: !truncated()");

        tr.free();
        code.free();
    }

    HwTrace::shutdown();

    std::printf("1..%d\n", g_test);
    return g_failed == 0 ? 0 : 1;
}
