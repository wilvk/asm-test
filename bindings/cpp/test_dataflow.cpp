// test_dataflow.cpp — C++ binding smoke for the data-flow analysis tier (Phase 6).
// Mirrors the Python binding's semantics through the typed C++ wrappers.
#include "asmtest_dataflow.hpp"

#include <cstdio>
#include <vector>

static int g_n = 0;
static int g_fail = 0;

static void check(bool cond, const char* desc) {
    ++g_n;
    if (cond)
        std::printf("ok %d - %s\n", g_n, desc);
    else {
        std::printf("not ok %d - %s\n", g_n, desc);
        g_fail = 1;
    }
}

int main() {
    using namespace asmtest::dataflow;

    // --- GC-move canonicalizer (forward-to-final) --- //
    check(gcmove_canon({}, 0, 0x1234) == 0x1234, "gcmove: empty move set is identity");
    std::vector<GcMove> mv = {{0x1000, 0x2000, 0x100, 5}};
    check(gcmove_canon(mv, 3, 0x1010) == 0x2010, "gcmove: pre-move addr forwards to final");
    check(gcmove_canon(mv, 3, 0x1000) == 0x2000, "gcmove: object base forwards");
    check(gcmove_canon(mv, 3, 0x10FF) == 0x20FF, "gcmove: last byte of half-open window forwards");
    check(gcmove_canon(mv, 3, 0x1100) == 0x1100, "gcmove: one past the window does not forward");
    check(gcmove_canon(mv, 5, 0x1010) == 0x1010, "gcmove: at-move-step observation not forwarded");
    check(gcmove_canon(mv, 3, 0x3000) == 0x3000, "gcmove: out-of-range addr unchanged");
    std::vector<GcMove> mv2 = {{0x1000, 0x2000, 0x100, 3}, {0x2000, 0x3000, 0x100, 6}};
    check(gcmove_canon(mv2, 1, 0x1010) == 0x3010, "gcmove: two compactions compose to final");

    // --- method resolver (tiered re-JIT aware) --- //
    std::vector<Method> ms = {
        {0x1000, 0x40, "Foo", 3}, {0x2000, 0x20, "Bar", 1}, {0x3000, 0, "Baz", 2}};
    check(method_resolve_pc(ms, 0x1000) == 0, "method: Foo range start");
    check(method_resolve_pc(ms, 0x103F) == 0, "method: Foo last byte (half-open)");
    check(method_resolve_pc(ms, 0x1040) == -1, "method: one past Foo -> none");
    check(method_resolve_pc(ms, 0x2010) == 1, "method: Bar range");
    check(method_resolve_pc(ms, 0x3000) == 2, "method: Baz point match");
    check(method_resolve_pc(ms, 0x3001) == -1, "method: Baz is point-only");
    std::vector<Method> rj = {{0x1000, 0x40, "Foo", 1}, {0x1000, 0x40, "Foo", 5}};
    check(method_resolve_pc(rj, 0x1010) == 1, "method: tiered re-JIT newest version wins");
    check(method_resolve_pc({}, 0x1000) == -1, "method: empty map -> -1");

    std::printf("1..%d\n", g_n);
    return g_fail;
}
