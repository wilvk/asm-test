/*
 * consumer_c11.c — public-header portability fixture (compile-only).
 *
 * Stands in for a downstream consumer that pins the strict ISO C standard
 * (`cc -std=c11`, the advertised `make CSTD=c11` flow). It must be able to
 * #include "asmtest.h" and expand the assertion + capture macros. This regressed
 * when the header declared `extern sigjmp_buf asmtest_jmp;` — sigjmp_buf is
 * POSIX, hidden under strict -std=c11, so the header would not parse at all.
 *
 * Built by `make check-header-portability` with -fsyntax-only (never linked), so
 * the routine below is only declared, not defined.
 */
#include "asmtest.h"

extern long some_routine(long, long);

void asmtest_c11_smoke(void) {
    regs_t r;
    ASM_CALL0(&r, some_routine);
    ASM_CALL2(&r, some_routine, 1, 2);
    ASM_CALLN(&r, some_routine, 1, 2, 3);
    ASM_MIXCALL(&r, some_routine, (1, 2), (0.5));
    ASSERT_EQ(1 + 1, 2);
    ASSERT_UEQ(2u, 2u);
    (void)r;
}
