/*
 * asmtest.h — public API for the asm-test framework (Phase 1).
 *
 * Write assembly routines (the code under test) and C test cases that call
 * them through the real ABI. The framework provides test discovery, a runner
 * with main(), per-suite setup/teardown, and assertion macros that report
 * failures TAP-style.
 *
 * See DESIGN.md for the full plan.
 */
#ifndef ASMTEST_H
#define ASMTEST_H

#include <setjmp.h>
#include <stddef.h>

/* One registered test case. */
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

/* Runtime support implemented in src/asmtest.c. */
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

extern jmp_buf asmtest_jmp; /* assertions longjmp here to abort a test */

/*
 * TEST(suite, name) { ... } defines a test body and auto-registers it via a
 * constructor that runs before main(). suite/name must be valid C identifiers.
 */
#define TEST(suite_, name_)                                                   \
    static void asmtest_fn_##suite_##_##name_(void);                          \
    static asmtest_case_t asmtest_tc_##suite_##_##name_ = {                   \
        #suite_, #name_, asmtest_fn_##suite_##_##name_, 0};                   \
    __attribute__((constructor)) static void                                  \
    asmtest_reg_##suite_##_##name_(void) {                                    \
        asmtest_register(&asmtest_tc_##suite_##_##name_);                     \
    }                                                                         \
    static void asmtest_fn_##suite_##_##name_(void)

/* SETUP(suite) { ... } runs before each test in the suite. */
#define SETUP(suite_)                                                         \
    static void asmtest_setup_##suite_(void);                                 \
    static asmtest_hook_t asmtest_setuphook_##suite_ = {                      \
        #suite_, asmtest_setup_##suite_, 0, 0};                               \
    __attribute__((constructor)) static void                                  \
    asmtest_regsetup_##suite_(void) {                                         \
        asmtest_register_hook(&asmtest_setuphook_##suite_);                   \
    }                                                                         \
    static void asmtest_setup_##suite_(void)

/* TEARDOWN(suite) { ... } runs after each test whose setup succeeded. */
#define TEARDOWN(suite_)                                                      \
    static void asmtest_teardown_##suite_(void);                             \
    static asmtest_hook_t asmtest_teardownhook_##suite_ = {                   \
        #suite_, asmtest_teardown_##suite_, 1, 0};                            \
    __attribute__((constructor)) static void                                  \
    asmtest_regteardown_##suite_(void) {                                      \
        asmtest_register_hook(&asmtest_teardownhook_##suite_);                \
    }                                                                         \
    static void asmtest_teardown_##suite_(void)

/* Skip the current test with a reason. */
#define SKIP(reason) asmtest_skip(reason)

/* --- Boolean assertions --- */
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

/* --- Signed integer comparisons --- */
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

/* --- String / memory assertions --- */
#define ASSERT_STREQ(a, b)                                                    \
    asmtest_assert_streq(__FILE__, __LINE__, #a, #b, (a), (b))

#define ASSERT_MEM_EQ(ptr, expect, len)                                       \
    asmtest_assert_mem_eq(__FILE__, __LINE__, #ptr, #expect, (ptr), (expect), \
                          (len))

#endif /* ASMTEST_H */
