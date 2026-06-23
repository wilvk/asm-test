/*
 * asmtest.h — public API for the asm-test framework (Phase 2).
 *
 * Write assembly routines (the code under test) and C test cases that call
 * them through the real ABI. The framework provides test discovery, a runner
 * with main(), per-suite setup/teardown, assertions, register/flags capture,
 * ABI-preservation checks, guard-page buffers, and crash-to-failure handling.
 *
 * See DESIGN.md for the full plan.
 */
#ifndef ASMTEST_H
#define ASMTEST_H

#include <setjmp.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Test / hook registration                                            */
/* ------------------------------------------------------------------ */

typedef struct asmtest_case {
    const char *suite;
    const char *name;
    void (*fn)(void);
    struct asmtest_case *next;
} asmtest_case_t;

/* A per-suite setup/teardown hook. kind: 0 = setup, 1 = teardown. */
typedef struct asmtest_hook {
    const char *suite;
    void (*fn)(void);
    int kind;
    struct asmtest_hook *next;
} asmtest_hook_t;

/* ------------------------------------------------------------------ */
/* Captured CPU state (filled by asm_call_capture in capture.s)        */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long rax;    /* offset 0  — return value          */
    unsigned long rdx;    /* offset 8  — second return register */
    unsigned long rbx;    /* offset 16 — callee-saved          */
    unsigned long rbp;    /* offset 24 — callee-saved          */
    unsigned long r12;    /* offset 32 — callee-saved          */
    unsigned long r13;    /* offset 40 — callee-saved          */
    unsigned long r14;    /* offset 48 — callee-saved          */
    unsigned long r15;    /* offset 56 — callee-saved          */
    unsigned long rflags; /* offset 64 — flags as fn left them */
} regs_t;

/* Sentinels the trampoline seeds into callee-saved regs before the call.
 * MUST match the movabsq immediates in src/capture.s. */
#define ASMTEST_SENTINEL_RBX 0x1111111111111111UL
#define ASMTEST_SENTINEL_RBP 0x2222222222222222UL
#define ASMTEST_SENTINEL_R12 0x3333333333333333UL
#define ASMTEST_SENTINEL_R13 0x4444444444444444UL
#define ASMTEST_SENTINEL_R14 0x5555555555555555UL
#define ASMTEST_SENTINEL_R15 0x6666666666666666UL

/* RFLAGS bit masks (use the short name with ASSERT_FLAG_*). */
#define ASMTEST_CF (1UL << 0)  /* carry    */
#define ASMTEST_PF (1UL << 2)  /* parity   */
#define ASMTEST_ZF (1UL << 6)  /* zero     */
#define ASMTEST_SF (1UL << 7)  /* sign     */
#define ASMTEST_OF (1UL << 11) /* overflow */

/* Call fn (in capture.s), capturing CPU state into *out. args has 6 slots. */
void asm_call_capture(regs_t *out, void *fn, const long *args);

/* ------------------------------------------------------------------ */
/* Runtime support (src/asmtest.c)                                     */
/* ------------------------------------------------------------------ */

void asmtest_register(asmtest_case_t *tc);
void asmtest_register_hook(asmtest_hook_t *h);
void asmtest_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));
void asmtest_skip(const char *reason) __attribute__((noreturn));
void asmtest_assert_streq(const char *file, int line, const char *aexpr,
                          const char *bexpr, const char *a, const char *b);
void asmtest_assert_mem_eq(const char *file, int line, const char *aexpr,
                           const char *bexpr, const void *a, const void *b,
                           size_t n);
void asmtest_assert_abi(const char *file, int line, const regs_t *r);
void asmtest_assert_flag(const char *file, int line, const regs_t *r,
                         unsigned long mask, int want_set, const char *name);

/* Allocate n writable bytes followed by an inaccessible guard page, so a
 * one-past-the-end access faults (and is reported as a test failure). Free
 * with the same n that was passed to alloc. */
void *asmtest_guarded_alloc(size_t n);
void asmtest_guarded_free(void *p, size_t n);

extern sigjmp_buf asmtest_jmp; /* assertions/crashes jump here */

/* ------------------------------------------------------------------ */
/* Test / fixture definition                                           */
/* ------------------------------------------------------------------ */

#define TEST(suite_, name_)                                                   \
    static void asmtest_fn_##suite_##_##name_(void);                          \
    static asmtest_case_t asmtest_tc_##suite_##_##name_ = {                   \
        #suite_, #name_, asmtest_fn_##suite_##_##name_, 0};                   \
    __attribute__((constructor)) static void                                  \
    asmtest_reg_##suite_##_##name_(void) {                                    \
        asmtest_register(&asmtest_tc_##suite_##_##name_);                     \
    }                                                                         \
    static void asmtest_fn_##suite_##_##name_(void)

#define SETUP(suite_)                                                         \
    static void asmtest_setup_##suite_(void);                                 \
    static asmtest_hook_t asmtest_setuphook_##suite_ = {                      \
        #suite_, asmtest_setup_##suite_, 0, 0};                               \
    __attribute__((constructor)) static void                                  \
    asmtest_regsetup_##suite_(void) {                                         \
        asmtest_register_hook(&asmtest_setuphook_##suite_);                   \
    }                                                                         \
    static void asmtest_setup_##suite_(void)

#define TEARDOWN(suite_)                                                      \
    static void asmtest_teardown_##suite_(void);                             \
    static asmtest_hook_t asmtest_teardownhook_##suite_ = {                   \
        #suite_, asmtest_teardown_##suite_, 1, 0};                            \
    __attribute__((constructor)) static void                                  \
    asmtest_regteardown_##suite_(void) {                                      \
        asmtest_register_hook(&asmtest_teardownhook_##suite_);                \
    }                                                                         \
    static void asmtest_teardown_##suite_(void)

#define SKIP(reason) asmtest_skip(reason)

/* ------------------------------------------------------------------ */
/* Capture-call convenience: ASM_CALLn(out, fn, args...)               */
/* ------------------------------------------------------------------ */

#define ASM_CALL0(out, fn)                                                    \
    asm_call_capture((out), (void *)(fn), (long[6]){0, 0, 0, 0, 0, 0})
#define ASM_CALL1(out, fn, a)                                                 \
    asm_call_capture((out), (void *)(fn),                                     \
                     (long[6]){(long)(a), 0, 0, 0, 0, 0})
#define ASM_CALL2(out, fn, a, b)                                              \
    asm_call_capture((out), (void *)(fn),                                     \
                     (long[6]){(long)(a), (long)(b), 0, 0, 0, 0})
#define ASM_CALL3(out, fn, a, b, c)                                           \
    asm_call_capture((out), (void *)(fn),                                     \
                     (long[6]){(long)(a), (long)(b), (long)(c), 0, 0, 0})
#define ASM_CALL4(out, fn, a, b, c, d)                                        \
    asm_call_capture((out), (void *)(fn),                                     \
                     (long[6]){(long)(a), (long)(b), (long)(c), (long)(d), 0, \
                               0})
#define ASM_CALL5(out, fn, a, b, c, d, e)                                     \
    asm_call_capture((out), (void *)(fn),                                     \
                     (long[6]){(long)(a), (long)(b), (long)(c), (long)(d),    \
                               (long)(e), 0})
#define ASM_CALL6(out, fn, a, b, c, d, e, f)                                  \
    asm_call_capture((out), (void *)(fn),                                     \
                     (long[6]){(long)(a), (long)(b), (long)(c), (long)(d),    \
                               (long)(e), (long)(f)})

/* ------------------------------------------------------------------ */
/* Assertions                                                          */
/* ------------------------------------------------------------------ */

#define ASSERT_TRUE(x)                                                        \
    do {                                                                      \
        if (!(x))                                                             \
            asmtest_fail(__FILE__, __LINE__, "ASSERT_TRUE(%s)", #x);          \
    } while (0)

#define ASSERT_FALSE(x)                                                       \
    do {                                                                      \
        if (x)                                                                \
            asmtest_fail(__FILE__, __LINE__, "ASSERT_FALSE(%s)", #x);         \
    } while (0)

#define ASMTEST_CMP_(a, op, b, opname)                                        \
    do {                                                                      \
        long asmtest_a_ = (long)(a);                                          \
        long asmtest_b_ = (long)(b);                                          \
        if (!(asmtest_a_ op asmtest_b_))                                      \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_" opname "(%s, %s): %ld vs %ld", #a, #b,     \
                         asmtest_a_, asmtest_b_);                             \
    } while (0)

#define ASSERT_EQ(a, b) ASMTEST_CMP_(a, ==, b, "EQ")
#define ASSERT_NE(a, b) ASMTEST_CMP_(a, !=, b, "NE")
#define ASSERT_LT(a, b) ASMTEST_CMP_(a, <, b, "LT")
#define ASSERT_LE(a, b) ASMTEST_CMP_(a, <=, b, "LE")
#define ASSERT_GT(a, b) ASMTEST_CMP_(a, >, b, "GT")
#define ASSERT_GE(a, b) ASMTEST_CMP_(a, >=, b, "GE")

#define ASSERT_STREQ(a, b)                                                    \
    asmtest_assert_streq(__FILE__, __LINE__, #a, #b, (a), (b))

#define ASSERT_MEM_EQ(ptr, expect, len)                                       \
    asmtest_assert_mem_eq(__FILE__, __LINE__, #ptr, #expect, (ptr), (expect), \
                          (len))

/* Verify the routine restored all callee-saved registers (ABI compliance). */
#define ASSERT_ABI_PRESERVED(r) asmtest_assert_abi(__FILE__, __LINE__, (r))

/* Flag assertions take a short name: ASSERT_FLAG_SET(&r, CF). */
#define ASSERT_FLAG_SET(r, FLG)                                               \
    asmtest_assert_flag(__FILE__, __LINE__, (r), ASMTEST_##FLG, 1, #FLG)
#define ASSERT_FLAG_CLEAR(r, FLG)                                             \
    asmtest_assert_flag(__FILE__, __LINE__, (r), ASMTEST_##FLG, 0, #FLG)

#endif /* ASMTEST_H */
