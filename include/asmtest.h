/*
 * asmtest.h — public API for the asm-test framework (Phase 0).
 *
 * Write assembly routines (the code under test) and C test cases that call
 * them through the real ABI. The framework provides test discovery, a runner
 * with main(), and assertion macros that report failures TAP-style.
 *
 * See DESIGN.md for the full plan.
 */
#ifndef ASMTEST_H
#define ASMTEST_H

#include <setjmp.h>

/* One registered test case. */
typedef struct asmtest_case {
    const char *suite;
    const char *name;
    void (*fn)(void);
    struct asmtest_case *next;
} asmtest_case_t;

/* Runtime support implemented in src/asmtest.c. */
void asmtest_register(asmtest_case_t *tc);
void asmtest_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((noreturn, format(printf, 3, 4)));

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

/* Assertions. On failure they report and abort the current test. */
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

#define ASSERT_EQ(a, b)                                                       \
    do {                                                                      \
        long asmtest_a_ = (long)(a);                                          \
        long asmtest_b_ = (long)(b);                                          \
        if (asmtest_a_ != asmtest_b_)                                         \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_EQ(%s, %s): %ld != %ld", #a, #b,             \
                         asmtest_a_, asmtest_b_);                             \
    } while (0)

#define ASSERT_NE(a, b)                                                       \
    do {                                                                      \
        long asmtest_a_ = (long)(a);                                          \
        long asmtest_b_ = (long)(b);                                          \
        if (asmtest_a_ == asmtest_b_)                                         \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_NE(%s, %s): both %ld", #a, #b, asmtest_a_);  \
    } while (0)

#endif /* ASMTEST_H */
