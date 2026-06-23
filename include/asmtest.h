/*
 * asmtest.h — public API for the asm-test framework.
 *
 * Write assembly routines (the code under test) and C test cases that call
 * them through the real ABI. The framework provides test discovery, a runner
 * with main(), per-suite setup/teardown, assertions, register/flags capture,
 * ABI-preservation checks, guard-page buffers, and crash-to-failure handling.
 *
 * Supported targets: x86-64 (System V AMD64 ABI) and AArch64 (AAPCS64), on
 * Linux and macOS. See DESIGN.md for the full plan.
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
/*                                                                     */
/* `ret` (return value) and `flags` are present on every target so     */
/* tests stay portable; the named callee-saved fields differ by arch.  */
/* Field offsets MUST match the stores in src/capture.s.               */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)

typedef struct {
    unsigned long ret;   /* 0  = rax (return value)   */
    unsigned long rdx;   /* 8  = second return reg    */
    unsigned long rbx;   /* 16 callee-saved           */
    unsigned long rbp;   /* 24 callee-saved           */
    unsigned long r12;   /* 32 callee-saved           */
    unsigned long r13;   /* 40 callee-saved           */
    unsigned long r14;   /* 48 callee-saved           */
    unsigned long r15;   /* 56 callee-saved           */
    unsigned long flags; /* 64 = RFLAGS               */
    double fret;         /* 72 = xmm0 (FP return); valid after asm_call_capture_fp */
} regs_t;

#define ASMTEST_SENTINEL_RBX 0x1111111111111111UL
#define ASMTEST_SENTINEL_RBP 0x2222222222222222UL
#define ASMTEST_SENTINEL_R12 0x3333333333333333UL
#define ASMTEST_SENTINEL_R13 0x4444444444444444UL
#define ASMTEST_SENTINEL_R14 0x5555555555555555UL
#define ASMTEST_SENTINEL_R15 0x6666666666666666UL

/* RFLAGS bit masks. */
#define ASMTEST_CF (1UL << 0)  /* carry    */
#define ASMTEST_PF (1UL << 2)  /* parity   */
#define ASMTEST_ZF (1UL << 6)  /* zero     */
#define ASMTEST_SF (1UL << 7)  /* sign     */
#define ASMTEST_OF (1UL << 11) /* overflow */

#elif defined(__aarch64__)

typedef struct {
    unsigned long ret;   /* 0  = x0 (return value)    */
    unsigned long x19;   /* 8  callee-saved           */
    unsigned long x20;   /* 16 */
    unsigned long x21;   /* 24 */
    unsigned long x22;   /* 32 */
    unsigned long x23;   /* 40 */
    unsigned long x24;   /* 48 */
    unsigned long x25;   /* 56 */
    unsigned long x26;   /* 64 */
    unsigned long x27;   /* 72 */
    unsigned long x28;   /* 80 */
    unsigned long x29;   /* 88 frame pointer          */
    unsigned long flags; /* 96 = NZCV                 */
    double fret;         /* 104 = d0 (FP return); valid after asm_call_capture_fp */
} regs_t;

#define ASMTEST_SENTINEL_X19 0x1111111111111111UL
#define ASMTEST_SENTINEL_X20 0x2222222222222222UL
#define ASMTEST_SENTINEL_X21 0x3333333333333333UL
#define ASMTEST_SENTINEL_X22 0x4444444444444444UL
#define ASMTEST_SENTINEL_X23 0x5555555555555555UL
#define ASMTEST_SENTINEL_X24 0x6666666666666666UL
#define ASMTEST_SENTINEL_X25 0x7777777777777777UL
#define ASMTEST_SENTINEL_X26 0x8888888888888888UL
#define ASMTEST_SENTINEL_X27 0x9999999999999999UL
#define ASMTEST_SENTINEL_X28 0xAAAAAAAAAAAAAAAAUL
#define ASMTEST_SENTINEL_X29 0xBBBBBBBBBBBBBBBBUL

/* NZCV condition-flag bit masks (CF/ZF share names with x86). */
#define ASMTEST_VF (1UL << 28) /* overflow */
#define ASMTEST_CF (1UL << 29) /* carry    */
#define ASMTEST_ZF (1UL << 30) /* zero     */
#define ASMTEST_NF (1UL << 31) /* negative */

#else
#error "asm-test supports x86-64 and AArch64 only"
#endif

/* Call fn (in capture.s), capturing CPU state into *out. args has 6 slots. */
void asm_call_capture(regs_t *out, void *fn, const long *args);

/* Like asm_call_capture, but also marshals 8 double args into the FP argument
 * registers (xmm0-7 / d0-7) and captures the FP return into out->fret. iargs
 * has 6 slots, fargs has 8. */
void asm_call_capture_fp(regs_t *out, void *fn, const long *iargs,
                         const double *fargs);

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
void asmtest_assert_fp_eq(const char *file, int line, const regs_t *r,
                          double expected);
void asmtest_assert_fp_near(const char *file, int line, const regs_t *r,
                            double expected, unsigned long max_ulps);

/* Allocate n writable bytes followed by an inaccessible guard page, so a
 * one-past-the-end access faults (and is reported as a test failure). Free
 * with the same n that was passed to alloc. */
void *asmtest_guarded_alloc(size_t n);
void asmtest_guarded_free(void *p, size_t n);

/* Like asmtest_guarded_alloc, but the guard page precedes the buffer so a
 * one-before-the-start access (underrun) faults. Free with _free_under. */
void *asmtest_guarded_alloc_under(size_t n);
void asmtest_guarded_free_under(void *p, size_t n);

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

/* ASM_FCALLn: call fn with n double args (in FP registers) and capture; the
 * double return lands in out->fret. For routines mixing integer and float
 * args, call asm_call_capture_fp directly. */
#define ASM_FCALL1(out, fn, x)                                                \
    asm_call_capture_fp((out), (void *)(fn), (long[6]){0},                    \
                        (double[8]){(double)(x), 0, 0, 0, 0, 0, 0, 0})
#define ASM_FCALL2(out, fn, x, y)                                             \
    asm_call_capture_fp((out), (void *)(fn), (long[6]){0},                    \
                        (double[8]){(double)(x), (double)(y), 0, 0, 0, 0, 0,  \
                                    0})
#define ASM_FCALL3(out, fn, x, y, z)                                          \
    asm_call_capture_fp((out), (void *)(fn), (long[6]){0},                    \
                        (double[8]){(double)(x), (double)(y), (double)(z), 0, \
                                    0, 0, 0, 0})

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

/* Unsigned 64-bit comparisons — use these for addresses, register values, and
 * anything that may exceed LONG_MAX (the signed ASSERT_* would misorder and
 * misprint those). Compared and reported as unsigned hex. */
#define ASMTEST_UCMP_(a, op, b, opname)                                       \
    do {                                                                      \
        unsigned long asmtest_ua_ = (unsigned long)(a);                      \
        unsigned long asmtest_ub_ = (unsigned long)(b);                      \
        if (!(asmtest_ua_ op asmtest_ub_))                                   \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_" opname "(%s, %s): 0x%lx vs 0x%lx", #a,     \
                         #b, asmtest_ua_, asmtest_ub_);                       \
    } while (0)

#define ASSERT_UEQ(a, b) ASMTEST_UCMP_(a, ==, b, "UEQ")
#define ASSERT_UNE(a, b) ASMTEST_UCMP_(a, !=, b, "UNE")
#define ASSERT_ULT(a, b) ASMTEST_UCMP_(a, <, b, "ULT")
#define ASSERT_ULE(a, b) ASMTEST_UCMP_(a, <=, b, "ULE")
#define ASSERT_UGT(a, b) ASMTEST_UCMP_(a, >, b, "UGT")
#define ASSERT_UGE(a, b) ASMTEST_UCMP_(a, >=, b, "UGE")

#define ASSERT_STREQ(a, b)                                                    \
    asmtest_assert_streq(__FILE__, __LINE__, #a, #b, (a), (b))

#define ASSERT_MEM_EQ(ptr, expect, len)                                       \
    asmtest_assert_mem_eq(__FILE__, __LINE__, #ptr, #expect, (ptr), (expect), \
                          (len))

/* Verify the routine restored all callee-saved registers (ABI compliance). */
#define ASSERT_ABI_PRESERVED(r) asmtest_assert_abi(__FILE__, __LINE__, (r))

/* Compare a named register/struct field unsigned: ASSERT_REG_EQ(&r, rax, 42)
 * (x86) or ASSERT_REG_EQ(&r, x[0], 42) (AArch64 emu). */
#define ASSERT_REG_EQ(r, field, val) ASSERT_UEQ((r)->field, (val))

/* Flag assertions take a short name: ASSERT_FLAG_SET(&r, CF). */
#define ASSERT_FLAG_SET(r, FLG)                                               \
    asmtest_assert_flag(__FILE__, __LINE__, (r), ASMTEST_##FLG, 1, #FLG)
#define ASSERT_FLAG_CLEAR(r, FLG)                                             \
    asmtest_assert_flag(__FILE__, __LINE__, (r), ASMTEST_##FLG, 0, #FLG)

/* Floating-point return assertions (read out->fret from asm_call_capture_fp).
 * ASSERT_FP_EQ is exact; ASSERT_FP_NEAR allows a distance in ULPs. */
#define ASSERT_FP_EQ(r, expected)                                             \
    asmtest_assert_fp_eq(__FILE__, __LINE__, (r), (expected))
#define ASSERT_FP_NEAR(r, expected, ulps)                                     \
    asmtest_assert_fp_near(__FILE__, __LINE__, (r), (expected), (ulps))

#endif /* ASMTEST_H */
