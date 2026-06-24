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
#include <stdint.h>

/* Framework version (semantic). ASMTEST_VERSION is the dotted string; the
 * numeric form ASMTEST_VERSION_NUM = MAJOR*10000 + MINOR*100 + PATCH allows
 * compile-time comparisons, e.g. #if ASMTEST_VERSION_NUM >= 10100. */
#define ASMTEST_VERSION_MAJOR 1
#define ASMTEST_VERSION_MINOR 0
#define ASMTEST_VERSION_PATCH 0
#define ASMTEST_VERSION "1.0.0"
#define ASMTEST_VERSION_NUM                                                   \
    (ASMTEST_VERSION_MAJOR * 10000 + ASMTEST_VERSION_MINOR * 100 +            \
     ASMTEST_VERSION_PATCH)

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

/* A benchmark case (Phase 9): like a test, but its body is one measured
 * iteration and the runner times many of them. Registered separately from
 * tests and only run under --bench. */
typedef struct asmtest_bench {
    const char *suite;
    const char *name;
    void (*fn)(void);
    struct asmtest_bench *next;
} asmtest_bench_t;

/* ------------------------------------------------------------------ */
/* Captured CPU state (filled by asm_call_capture in capture.s)        */
/*                                                                     */
/* `ret` (return value) and `flags` are present on every target so     */
/* tests stay portable; the named callee-saved fields differ by arch.  */
/* Field offsets MUST match the stores in src/capture.s.               */
/* ------------------------------------------------------------------ */

/* One 128-bit vector register, viewable as several lane layouts. The vector
 * return value is vec[0] (xmm0 / v0); vec[] is filled by asm_call_capture_vec. */
typedef union {
    unsigned char u8[16];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} vec128_t;

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
    vec128_t vec[16];    /* 80 = xmm0..15; valid after asm_call_capture_vec */
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
    vec128_t vec[32];    /* 112 = v0..31; valid after asm_call_capture_vec */
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

/* Like asm_call_capture, but marshals 8 full 128-bit vector args into the
 * vector argument registers (xmm0-7 / v0-7) and captures the entire vector
 * register file into out->vec[] (the vector return is out->vec[0]). iargs has
 * 6 slots, vargs has 8. */
void asm_call_capture_vec(regs_t *out, void *fn, const long *iargs,
                          const vec128_t *vargs);

/* Like asm_call_capture_fp, but with an arbitrary number of double args: the
 * first 8 go in the FP argument registers (xmm0-7 / d0-7), the rest spill onto
 * the stack per the ABI. iargs has 6 register slots; fargs has `nfargs` entries.
 * As with asm_call_capture_args, this path uses the frame-pointer register
 * (rbp / x29), so that register is reported preserved but not independently
 * checked by ASSERT_ABI_PRESERVED. */
void asm_call_capture_fp_n(regs_t *out, void *fn, const long *iargs,
                           const double *fargs, int nfargs);

/* Like asm_call_capture_vec, but with an arbitrary number of 128-bit vector
 * args: the first 8 go in the vector argument registers (xmm0-7 / v0-7), the
 * rest spill onto the stack (16-byte slots) per the ABI. iargs has 6 register
 * slots; vargs has `nvargs` entries; the whole vector file is captured into
 * out->vec[] as asm_call_capture_vec does. Uses the frame-pointer register
 * (see asm_call_capture_fp_n). */
void asm_call_capture_vec_n(regs_t *out, void *fn, const long *iargs,
                            const vec128_t *vargs, int nvargs);

/* Call fn with an arbitrary number of integer args: the first 6 (x86-64) / 8
 * (AArch64) go in registers, the rest are passed on the stack per the ABI.
 * Captures the GP result and flags. NOTE: on this path the frame-pointer
 * register (rbp / x29) is used as a frame pointer, so it is not independently
 * verified by ASSERT_ABI_PRESERVED — the other callee-saved registers are. */
void asm_call_capture_args(regs_t *out, void *fn, const long *args, int nargs);

/* Call fn that returns a large (memory-class) struct via the hidden result
 * pointer (SysV passes it in rdi, AAPCS64 in x8). The struct is written to
 * `result`; the `nargs` visible integer args follow the ABI (x86-64: rsi.. then
 * stack; AArch64: x0.. then stack). Inspect the returned struct via `result`.
 * Small (<=16-byte) structs are returned in registers instead and need no
 * special call: read them from regs_t (rax/rdx, or vec[0]/vec[1] for SSE). */
void asm_call_capture_sret(regs_t *out, void *fn, void *result,
                           const long *args, int nargs);

/* Call fn taking `niargs` integer register args followed by a large
 * (memory-class) struct passed by value (`ssize` bytes at `sptr`). x86-64 copies
 * the struct inline onto the stack; AArch64 passes a pointer to it (per AAPCS64).
 * Small (<=16-byte) structs need no special call — pass their eightbytes as
 * ordinary integer/double args (e.g. struct{long,long} via ASM_CALL2, or
 * struct{long;double} via asm_call_capture_fp). */
void asm_call_capture_bigstruct(regs_t *out, void *fn, const long *iargs,
                                int niargs, const void *sptr, size_t ssize);

/* ------------------------------------------------------------------ */
/* Differential / property testing (Phase 7)                           */
/* ------------------------------------------------------------------ */

/* A deterministic, seedable pseudo-random source (splitmix64). The seed comes
 * from the ASMTEST_SEED environment variable when set (decimal or 0x-hex),
 * otherwise a fixed default — so a failing input reproduces run to run, and CI
 * can vary it. The seed in effect is reported in any mismatch message. */
typedef struct {
    uint64_t s;
} asmtest_rng_t;

uint64_t asmtest_rng_u64(asmtest_rng_t *rng);     /* next 64 random bits      */
long asmtest_rng_long(asmtest_rng_t *rng);        /* a random long (any value)*/
long asmtest_rng_range(asmtest_rng_t *rng, long lo, long hi); /* in [lo,hi]    */

/* An input generator: fill args[0..arity-1] with one random input tuple and
 * return the arity. `cap` is the number of writable slots in `args` (>= 8). The
 * arity must be constant across calls and match the ASSERT_MATCHES_REF* variant
 * it is paired with. */
typedef int (*asmtest_gen_fn)(asmtest_rng_t *rng, long *args, int cap);

/* C reference models, by arity, returning the expected result for a tuple. */
typedef long (*asmtest_ref1_fn)(long);
typedef long (*asmtest_ref2_fn)(long, long);
typedef long (*asmtest_ref3_fn)(long, long, long);

/* Differential engines: draw `trials` random inputs from `gen`, call the asm
 * routine `fn` through the real ABI (asm_call_capture_args) and the C model
 * `ref`, and fail on the first input whose results differ — reporting that
 * input, both results, and the seed. One per arity (C cannot dispatch on a
 * function pointer's arity); use the matching ASSERT_MATCHES_REF{1,2,3}. */
void asmtest_match_ref1(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref1_fn ref, asmtest_gen_fn gen,
                        int trials);
void asmtest_match_ref2(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref2_fn ref, asmtest_gen_fn gen,
                        int trials);
void asmtest_match_ref3(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref3_fn ref, asmtest_gen_fn gen,
                        int trials);

/* ------------------------------------------------------------------ */
/* Runtime support (src/asmtest.c)                                     */
/* ------------------------------------------------------------------ */

void asmtest_register(asmtest_case_t *tc);
void asmtest_register_hook(asmtest_hook_t *h);
void asmtest_register_bench(asmtest_bench_t *b);

/* A volatile sink: route a computed value through this so the optimizer cannot
 * discard a benchmark body whose result is otherwise unused. Calls to asm
 * routines under test already can't be elided, so this is only needed when a
 * benchmark body does pure-C work. */
extern volatile long asmtest_bench_sink;

/* Read the platform cycle/tick counter used for benchmarking: rdtsc on x86-64
 * (reference cycles) and cntvct_el0 on AArch64 (the virtual timer, whose tick
 * is coarser than a core cycle). Exposed for custom timing in a BENCH body. */
static inline uint64_t asmtest_cycle_counter(void) {
#if defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(v));
    return v;
#endif
}
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
void asmtest_assert_double_eq(const char *file, int line, double actual,
                              double expected);
void asmtest_assert_double_near(const char *file, int line, double actual,
                                double expected, unsigned long max_ulps);
void asmtest_assert_float_eq(const char *file, int line, float actual,
                             float expected);
void asmtest_assert_float_near(const char *file, int line, float actual,
                               float expected, unsigned long max_ulps);
void asmtest_assert_vec_eq(const char *file, int line, const char *idxexpr,
                           const unsigned char *actual,
                           const unsigned char *expected);

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

/* BENCH(suite, name) { ... } — define + auto-register a benchmark. The body is
 * one measured iteration; the runner auto-calibrates an inner repeat count and
 * times several rounds, reporting min/median cycles per call. Benchmarks run
 * only under `--bench` (so a normal `make test` is unaffected). Funnel any
 * pure-C result through BENCH_USE() so it is not optimized away; calls into the
 * asm routine under test need no such help. */
#define BENCH(suite_, name_)                                                  \
    static void asmtest_benchfn_##suite_##_##name_(void);                     \
    static asmtest_bench_t asmtest_bc_##suite_##_##name_ = {                  \
        #suite_, #name_, asmtest_benchfn_##suite_##_##name_, 0};              \
    __attribute__((constructor)) static void                                  \
    asmtest_regbench_##suite_##_##name_(void) {                               \
        asmtest_register_bench(&asmtest_bc_##suite_##_##name_);               \
    }                                                                         \
    static void asmtest_benchfn_##suite_##_##name_(void)

#define BENCH_USE(x) (asmtest_bench_sink = (long)(x))

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

/* ASM_CALLN: call fn with any number (>=1) of integer args, overflowing onto
 * the stack as the ABI requires. nargs is derived from the argument list. */
#define ASM_CALLN(out, fn, ...)                                               \
    asm_call_capture_args(                                                    \
        (out), (void *)(fn), (long[]){__VA_ARGS__},                          \
        (int)(sizeof((long[]){__VA_ARGS__}) / sizeof(long)))

/* ASM_SRET: call fn that returns a large struct into *result, with >=1 visible
 * integer args. */
#define ASM_SRET(out, fn, result, ...)                                        \
    asm_call_capture_sret(                                                    \
        (out), (void *)(fn), (result), (long[]){__VA_ARGS__},                \
        (int)(sizeof((long[]){__VA_ARGS__}) / sizeof(long)))

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

/* ASM_VCALLn: call fn with n 128-bit vector args (vec128_t) and capture the
 * whole vector file; the vector return is out->vec[0]. */
#define ASM_VCALL1(out, fn, v0)                                               \
    asm_call_capture_vec((out), (void *)(fn), (long[6]){0},                   \
                         (vec128_t[8]){(v0)})
#define ASM_VCALL2(out, fn, v0, v1)                                           \
    asm_call_capture_vec((out), (void *)(fn), (long[6]){0},                   \
                         (vec128_t[8]){(v0), (v1)})
#define ASM_VCALL3(out, fn, v0, v1, v2)                                       \
    asm_call_capture_vec((out), (void *)(fn), (long[6]){0},                   \
                         (vec128_t[8]){(v0), (v1), (v2)})

/* ASM_FCALLN: call fn with any number (>=1) of double args, the 9th+ spilling
 * onto the stack as the ABI requires. The double return lands in out->fret. */
#define ASM_FCALLN(out, fn, ...)                                              \
    asm_call_capture_fp_n(                                                    \
        (out), (void *)(fn), (long[6]){0}, (double[]){__VA_ARGS__},           \
        (int)(sizeof((double[]){__VA_ARGS__}) / sizeof(double)))

/* ASM_VCALLN: call fn with any number (>=1) of 128-bit vector args, the 9th+
 * spilling onto the stack. Captures the whole vector file (return = vec[0]). */
#define ASM_VCALLN(out, fn, ...)                                              \
    asm_call_capture_vec_n(                                                   \
        (out), (void *)(fn), (long[6]){0}, (vec128_t[]){__VA_ARGS__},         \
        (int)(sizeof((vec128_t[]){__VA_ARGS__}) / sizeof(vec128_t)))

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
    asmtest_assert_double_eq(__FILE__, __LINE__, (r)->fret, (expected))
#define ASSERT_FP_NEAR(r, expected, ulps)                                     \
    asmtest_assert_double_near(__FILE__, __LINE__, (r)->fret, (expected),     \
                               (ulps))

/* Raw scalar double/float assertions — handy for individual vector lanes,
 * e.g. ASSERT_DEQ(r.vec[0].f64[1], 2.0) or ASSERT_FNEAR(r.vec[0].f32[0], 1.0f,
 * 1). */
#define ASSERT_DEQ(actual, expected)                                          \
    asmtest_assert_double_eq(__FILE__, __LINE__, (actual), (expected))
#define ASSERT_DNEAR(actual, expected, ulps)                                  \
    asmtest_assert_double_near(__FILE__, __LINE__, (actual), (expected), (ulps))
#define ASSERT_FEQ(actual, expected)                                          \
    asmtest_assert_float_eq(__FILE__, __LINE__, (actual), (expected))
#define ASSERT_FNEAR(actual, expected, ulps)                                  \
    asmtest_assert_float_near(__FILE__, __LINE__, (actual), (expected), (ulps))

/* Bytewise compare a captured 128-bit vector register to 16 expected bytes
 * (hexdump diff on failure): ASSERT_VEC_EQ(&r, 0, expected.u8). */
#define ASSERT_VEC_EQ(r, idx, expect_ptr)                                     \
    asmtest_assert_vec_eq(__FILE__, __LINE__, #idx, (r)->vec[idx].u8,         \
                          (const unsigned char *)(expect_ptr))

/* Differential / property assertions: assert the asm routine `fn` matches the
 * C reference `ref` over `n` random input tuples drawn from generator `gen`.
 * Pick the variant whose number matches the routine's integer arity:
 *   ASSERT_MATCHES_REF2(imax, ref_imax, gen_pair, 10000);
 * On a mismatch the failing input, both results, and the seed are reported, so
 * the case reproduces (set ASMTEST_SEED to replay a CI-randomized run). */
#define ASSERT_MATCHES_REF1(fn, ref, gen, n)                                  \
    asmtest_match_ref1(__FILE__, __LINE__, #fn, (void *)(fn), (ref), (gen), (n))
#define ASSERT_MATCHES_REF2(fn, ref, gen, n)                                  \
    asmtest_match_ref2(__FILE__, __LINE__, #fn, (void *)(fn), (ref), (gen), (n))
#define ASSERT_MATCHES_REF3(fn, ref, gen, n)                                  \
    asmtest_match_ref3(__FILE__, __LINE__, #fn, (void *)(fn), (ref), (gen), (n))

#endif /* ASMTEST_H */
