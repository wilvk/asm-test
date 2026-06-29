/*
 * test_drtrace.cpp — standalone smoke test for the in-process DynamoRIO
 * native-trace wrapper (asmtest_drtrace.hpp). Mirrors the Python suite
 * bindings/python/tests/test_drtrace.py.
 *
 * Self-skips (prints "SKIP: <reason>", returns 0) unless the tier is built AND
 * DynamoRIO is resolvable — i.e. unless ASMTEST_DRAPP_LIB / ASMTEST_DRCLIENT (and
 * ASMTEST_DR_LIB or DYNAMORIO_HOME) point at a built libasmtest_drapp + client on
 * a DynamoRIO-capable Linux x86-64 host. Build standalone (links only -ldl):
 *
 *   g++ -std=c++17 -I include bindings/cpp/test_drtrace.cpp -ldl -o test_drtrace
 *   ASMTEST_DRAPP_LIB=$PWD/build/libasmtest_drapp.so ./test_drtrace
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "asmtest_drtrace.hpp"

using namespace asmtest;

// mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two basic blocks)
static const std::vector<std::uint8_t> ROUTINE = {
    0x48, 0x89, 0xF8, 0x48, 0x01, 0xF0, 0x48, 0x3D, 0x64, 0x00,
    0x00, 0x00, 0x7E, 0x03, 0x48, 0xFF, 0xC8, 0xC3};

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL: %s\n", (msg));                         \
            return 1;                                                          \
        }                                                                      \
    } while (0)

int main() {
    if (!NativeTrace::available()) {
        std::printf("SKIP: DynamoRIO native-trace tier unavailable "
                    "(self-skip)\n");
        return 0;
    }
    if (!std::getenv("ASMTEST_DRCLIENT")) {
        std::printf("SKIP: ASMTEST_DRCLIENT not set (build the DR client)\n");
        return 0;
    }

    try {
        NativeTrace::initialize();
    } catch (const std::exception &e) {
        std::printf("SKIP: dr_init/start failed: %s\n", e.what());
        return 0;
    }

    // ---- block coverage + accumulation ----
    {
        NativeCode code = NativeCode::from_bytes(ROUTINE);
        NativeTrace tr = NativeTrace::create(/*blocks=*/64);
        tr.register_region("add2", code);

        {
            auto scope = tr.region("add2");
            long r = code.call(20, 22);
            CHECK(r == 42, "code.call(20, 22) == 42");
        }
        CHECK(tr.covered(0), "tr.covered(0) (entry block)");

        std::uint64_t before = tr.blocks_len();
        {
            auto scope = tr.region("add2");
            long r2 = code.call(60, 60);  // 120 > 100 -> dec -> 119
            CHECK(r2 == 119, "code.call(60, 60) == 119");
        }
        CHECK(tr.blocks_len() >= before, "tr.blocks_len() >= before");
        CHECK(NativeTrace::marker_error() == 0, "marker_error() == 0");

        tr.unregister("add2");
        code.free();
        tr.free();
    }

    // ---- instruction mode ----
    {
        NativeTrace tr2 = NativeTrace::create(/*blocks=*/64, /*instructions=*/64);
        NativeCode code2 = NativeCode::from_bytes(ROUTINE);
        tr2.register_region("add2i", code2);
        {
            auto scope = tr2.region("add2i");
            long r = code2.call(1, 2);
            CHECK(r == 3, "code2.call(1, 2) == 3");
        }
        CHECK(tr2.insns_total() >= 4, "tr2.insns_total() >= 4");
        // offset-list accessors: 1+2 <= 100 -> jle taken, dec (0xe) skipped:
        // exact stream mov(0) add(3) cmp(6) jle(0xc) ret(0x11); blocks {0, 0x11}.
        const std::vector<std::uint64_t> expect{0x0, 0x3, 0x6, 0xc, 0x11};
        CHECK(tr2.insn_offsets() == expect, "tr2.insn_offsets() exact sequence");
        auto bo = tr2.block_offsets();
        bool has0 = std::find(bo.begin(), bo.end(), 0u) != bo.end();
        CHECK(has0, "tr2.block_offsets() contains entry block 0");

        tr2.unregister("add2i");
        code2.free();
        tr2.free();
    }

    // ---- symbol mode ----
    // Traces a named exported function by name with no begin/end markers:
    // recording is always-on over the resolved range, so the fixture is called
    // WITHOUT any region scope and coverage still lands.
    {
        NativeTrace str = NativeTrace::create(/*blocks=*/64);
        str.register_symbol("asmtest_symbol_demo", 256);
        long sr = NativeTrace::symbol_demo(3, 4);  // no region scope on purpose
        CHECK(sr == 10, "symbol_demo(3, 4) == 10");
        CHECK(str.covered(0), "symbol mode records coverage with no manual region");
        CHECK(NativeTrace::marker_error() == 0, "symbol mode uses no markers");

        str.unregister("asmtest_symbol_demo");
        str.free();
    }

    NativeTrace::shutdown();
    std::printf("PASS\n");
    return 0;
}
