/*
 * consumer_cpp.cpp — public-header portability fixture (compile-only).
 *
 * Backs asmtest.hpp's claim that "a C++ project can just #include \"asmtest.h\"
 * and use the framework directly": it includes the C header (NOT asmtest.hpp)
 * and expands the ASM_CALL* / ASM_FCALL* / ASM_VCALL* capture macros from a C++
 * translation unit. This regressed when each macro built its argument array with
 * a C99 compound literal — g++ rejects taking the address of a compound-literal
 * temporary array ("taking address of temporary array"), so the whole ergonomic
 * surface failed to compile under g++.
 *
 * Built by `make check-header-portability` with -fsyntax-only (never linked), so
 * the routine below is only declared, not defined.
 */
#include "asmtest.h"

extern "C" long some_routine(long, long);

void asmtest_cpp_smoke() {
    regs_t r;
    vec128_t v{};
    ASM_CALL0(&r, some_routine);
    ASM_CALL2(&r, some_routine, 1, 2);
    ASM_CALL6(&r, some_routine, 1, 2, 3, 4, 5, 6);
    ASM_CALLN(&r, some_routine, 1, 2, 3);
    ASM_MIXCALL(&r, some_routine, (1, 2), (0.5));
    ASM_FCALL1(&r, some_routine, 1.5);
    ASM_FCALL3(&r, some_routine, 1.0, 2.0, 3.0);
    ASM_FCALLN(&r, some_routine, 1.0, 2.0);
    ASM_VCALL1(&r, some_routine, v);
    ASM_VCALLN(&r, some_routine, v, v);
    ASSERT_EQ(1 + 1, 2);
    ASSERT_UEQ(2u, 2u);
    (void)r;
    (void)v;
}
