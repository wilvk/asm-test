/*
 * asmtest.c — runner, registries, failure/skip/crash model, fixtures,
 * register/flags assertions, guard-page buffers, and main() (Phase 2).
 */
#include "asmtest.h"
#include "platform.h" /* POSIX vs Win32 includes + ASMTEST_FNMATCH */

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <setjmp.h> /* sig{setjmp,longjmp}; no longer pulled in via asmtest.h */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Linux spells it MAP_ANONYMOUS; macOS/BSD provide MAP_ANON. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* siglongjmp return codes for the per-test jump buffer. */
enum { JMP_FAIL = 1, JMP_SKIP = 2 };

/* Test outcome. */
enum { ST_PASS, ST_FAIL, ST_SKIP };

static double now_secs(void); /* monotonic seconds; defined below */

static asmtest_case_t *asmtest_head = NULL;
static asmtest_case_t *asmtest_tail = NULL;
static asmtest_hook_t *asmtest_hooks = NULL;

#if !defined(_WIN32)
/* Per-test recovery buffer. Internal to the runner (no consumer references it),
 * so it stays out of the public header — where sigjmp_buf, a POSIX type, would
 * break strict -std=c11 consumers that #include "asmtest.h". */
static sigjmp_buf asmtest_jmp;
static volatile sig_atomic_t asmtest_in_test = 0;
#else
static volatile int asmtest_in_test = 0;
#endif

static char asmtest_msg[1024];
static const char *asmtest_loc_file;
static int asmtest_loc_line;

/* Per-test wall-clock timeout, enforced via alarm() (0 disables). Set from
 * --timeout / ASMTEST_TIMEOUT before the run; read by the SIGALRM handler. */
static int asmtest_timeout_secs = 10;

/* Runner-internal allocation guard. The library APIs (trace.c/disasm.c/fuzz.c)
 * degrade gracefully on OOM because their caller can handle a NULL/0 return; the
 * runner's own small startup allocations have no such caller and the run can't
 * proceed without them, so fail loudly with a clear diagnostic (exit 2, distinct
 * from the 0/1 pass/fail codes) instead of dereferencing NULL. */
static void *asmtest_xalloc(void *p, const char *what) {
    if (p == NULL) {
        fprintf(stderr, "asmtest: out of memory allocating %s\n", what);
        exit(2);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

void asmtest_register(asmtest_case_t *tc) {
    tc->next = NULL;
    if (asmtest_head == NULL)
        asmtest_head = tc;
    else
        asmtest_tail->next = tc;
    asmtest_tail = tc;
}

void asmtest_register_hook(asmtest_hook_t *h) {
    h->next = asmtest_hooks;
    asmtest_hooks = h;
}

static asmtest_bench_t *asmtest_bench_head = NULL;
static asmtest_bench_t *asmtest_bench_tail = NULL;

void asmtest_register_bench(asmtest_bench_t *b) {
    b->next = NULL;
    if (asmtest_bench_head == NULL)
        asmtest_bench_head = b;
    else
        asmtest_bench_tail->next = b;
    asmtest_bench_tail = b;
}

volatile long asmtest_bench_sink;

/* ------------------------------------------------------------------ */
/* Failure / skip                                                      */
/* ------------------------------------------------------------------ */

/* The per-test non-local exit. POSIX uses siglongjmp into the runner's jump
 * buffer; the Win64 tier uses the __builtin_longjmp recovery armed by the
 * in-process test facility (src/platform_win32.c), shared with its crash and
 * timeout paths. Both unwind to run_one's recovery point. */
#if defined(_WIN32)
__attribute__((noreturn)) static void asmtest_jump(int reason) {
    asmtest_win32_test_reason = reason;
    asmtest_win32_test_disarm();
    __builtin_longjmp(asmtest_win32_test_recover, 1);
}
#else
__attribute__((noreturn)) static void asmtest_jump(int reason) {
    siglongjmp(asmtest_jmp, reason);
}
#endif

void asmtest_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(asmtest_msg, sizeof asmtest_msg, fmt, ap);
    va_end(ap);
    asmtest_loc_file = file;
    asmtest_loc_line = line;
    asmtest_jump(JMP_FAIL);
}

void asmtest_skip(const char *reason) {
    snprintf(asmtest_msg, sizeof asmtest_msg, "%s", reason ? reason : "");
    asmtest_jump(JMP_SKIP);
}

/* ------------------------------------------------------------------ */
/* Value / memory assertions                                           */
/* ------------------------------------------------------------------ */

void asmtest_assert_streq(const char *file, int line, const char *aexpr,
                          const char *bexpr, const char *a, const char *b) {
    if (a == NULL || b == NULL) {
        if (a != b)
            asmtest_fail(file, line, "ASSERT_STREQ(%s, %s): %s vs %s", aexpr,
                         bexpr, a ? "string" : "NULL", b ? "string" : "NULL");
        return;
    }
    if (strcmp(a, b) != 0)
        asmtest_fail(file, line, "ASSERT_STREQ(%s, %s): \"%s\" != \"%s\"",
                     aexpr, bexpr, a, b);
}

/* Append a hexdump of bytes [start,end) of `p` into buf (best-effort). */
static void hexdump_window(char *buf, size_t cap, const unsigned char *p,
                           size_t start, size_t end) {
    size_t off = 0;
    for (size_t j = start; j < end && off + 4 < cap; j++)
        off += (size_t)snprintf(buf + off, cap - off, "%02x%s", p[j],
                                j + 1 < end ? " " : "");
}

void asmtest_assert_mem_eq(const char *file, int line, const char *aexpr,
                           const char *bexpr, const void *a, const void *b,
                           size_t n) {
    const unsigned char *pa = (const unsigned char *)a; /* actual */
    const unsigned char *pb = (const unsigned char *)b; /* expected */
    size_t i = 0;
    while (i < n && pa[i] == pb[i])
        i++;
    if (i == n)
        return; /* equal */

    /* Hexdump a window around the first difference: expected vs actual. */
    size_t start = (i >= 8) ? i - 8 : 0;
    size_t end = start + 16;
    if (end > n)
        end = n;
    char expect_hex[64], actual_hex[64];
    hexdump_window(expect_hex, sizeof expect_hex, pb, start, end);
    hexdump_window(actual_hex, sizeof actual_hex, pa, start, end);

    asmtest_fail(file, line,
                 "ASSERT_MEM_EQ(%s, %s, %zu): first diff at byte %zu "
                 "(0x%02x != 0x%02x)\n"
                 "       bytes [%zu,%zu)\n"
                 "       expect: %s\n"
                 "       actual: %s",
                 aexpr, bexpr, n, i, pb[i], pa[i], start, end, expect_hex,
                 actual_hex);
}

/* ------------------------------------------------------------------ */
/* Register / flag assertions                                          */
/* ------------------------------------------------------------------ */

/* Non-jumping verdict shims (Track 0). These RETURN the verdict and write a
 * reason into msg instead of longjmp-ing into the runner, so an FFI binding can
 * validate a capture without the C runner present. The jumping asmtest_assert_*
 * wrappers below delegate here, keeping one source of truth for the checks. */
int asmtest_check_abi(const regs_t *r, char *msg, size_t n) {
    /* 64-bit got/want: on Win64 (LLP64) the registers and sentinels are 64-bit
     * but `unsigned long` is 32-bit, which would truncate the comparison. */
    const struct {
        const char *name;
        unsigned long long got, want;
    } chk[] = {
#if defined(ASMTEST_ABI_WIN64)
        /* Win64 adds rdi/rsi to the integer callee-saved set vs System V. */
        {"rbx", r->rbx, ASMTEST_SENTINEL_RBX},
        {"rbp", r->rbp, ASMTEST_SENTINEL_RBP},
        {"rdi", r->rdi, ASMTEST_SENTINEL_RDI},
        {"rsi", r->rsi, ASMTEST_SENTINEL_RSI},
        {"r12", r->r12, ASMTEST_SENTINEL_R12},
        {"r13", r->r13, ASMTEST_SENTINEL_R13},
        {"r14", r->r14, ASMTEST_SENTINEL_R14},
        {"r15", r->r15, ASMTEST_SENTINEL_R15},
#elif defined(__x86_64__)
        {"rbx", r->rbx, ASMTEST_SENTINEL_RBX},
        {"rbp", r->rbp, ASMTEST_SENTINEL_RBP},
        {"r12", r->r12, ASMTEST_SENTINEL_R12},
        {"r13", r->r13, ASMTEST_SENTINEL_R13},
        {"r14", r->r14, ASMTEST_SENTINEL_R14},
        {"r15", r->r15, ASMTEST_SENTINEL_R15},
#elif defined(__aarch64__)
        {"x19", r->x19, ASMTEST_SENTINEL_X19},
        {"x20", r->x20, ASMTEST_SENTINEL_X20},
        {"x21", r->x21, ASMTEST_SENTINEL_X21},
        {"x22", r->x22, ASMTEST_SENTINEL_X22},
        {"x23", r->x23, ASMTEST_SENTINEL_X23},
        {"x24", r->x24, ASMTEST_SENTINEL_X24},
        {"x25", r->x25, ASMTEST_SENTINEL_X25},
        {"x26", r->x26, ASMTEST_SENTINEL_X26},
        {"x27", r->x27, ASMTEST_SENTINEL_X27},
        {"x28", r->x28, ASMTEST_SENTINEL_X28},
        {"x29", r->x29, ASMTEST_SENTINEL_X29},
#elif defined(__riscv) && __riscv_xlen == 64
        /* rv64 LP64D integer callee-saved set s0-s11 (x8-x9, x18-x27). The base
         * trampolines seed and check s0 too; only the variable-arity paths use
         * it as a frame pointer (mirroring rbp/x29), where it is reported
         * preserved-but-not-checked. */
        {"s0", r->s0, ASMTEST_SENTINEL_S0},
        {"s1", r->s1, ASMTEST_SENTINEL_S1},
        {"s2", r->s2, ASMTEST_SENTINEL_S2},
        {"s3", r->s3, ASMTEST_SENTINEL_S3},
        {"s4", r->s4, ASMTEST_SENTINEL_S4},
        {"s5", r->s5, ASMTEST_SENTINEL_S5},
        {"s6", r->s6, ASMTEST_SENTINEL_S6},
        {"s7", r->s7, ASMTEST_SENTINEL_S7},
        {"s8", r->s8, ASMTEST_SENTINEL_S8},
        {"s9", r->s9, ASMTEST_SENTINEL_S9},
        {"s10", r->s10, ASMTEST_SENTINEL_S10},
        {"s11", r->s11, ASMTEST_SENTINEL_S11},
#endif
    };
    for (size_t i = 0; i < sizeof chk / sizeof chk[0]; i++) {
        if (chk[i].got != chk[i].want) {
            if (msg && n)
                snprintf(msg, n,
                         "%s not restored (got 0x%llx, expected 0x%llx)",
                         chk[i].name, chk[i].got, chk[i].want);
            return 1;
        }
    }
    if (msg && n)
        msg[0] = '\0';
    return 0;
}

int asmtest_check_flag(const regs_t *r, unsigned long mask, int want_set,
                       const char *name, char *msg, size_t n) {
    int set = (r->flags & mask) != 0;
    if (set != want_set) {
        if (msg && n)
            snprintf(msg, n, "FLAG_%s(%s): flags=0x%lx",
                     want_set ? "SET" : "CLEAR", name, r->flags);
        return 1;
    }
    if (msg && n)
        msg[0] = '\0';
    return 0;
}

void asmtest_assert_abi(const char *file, int line, const regs_t *r) {
    char msg[128];
    if (asmtest_check_abi(r, msg, sizeof msg))
        asmtest_fail(file, line, "ASSERT_ABI_PRESERVED: %s", msg);
}

#if defined(ASMTEST_ABI_WIN64)
/* xmm6-15 are callee-saved on Win64; the _vec trampolines seed xmm(6+k) with both
 * lanes == 6+k, so a restored register reads back vec[i] == {i, i}. */
int asmtest_check_abi_vec(const regs_t *r, char *msg, size_t n) {
    for (unsigned i = 6; i <= 15; i++) {
        if (r->vec[i].u64[0] != (uint64_t)i ||
            r->vec[i].u64[1] != (uint64_t)i) {
            if (msg && n)
                snprintf(msg, n,
                         "xmm%u not restored (got {0x%llx, 0x%llx}, "
                         "expected {0x%x, 0x%x})",
                         i, (unsigned long long)r->vec[i].u64[0],
                         (unsigned long long)r->vec[i].u64[1], i, i);
            return 1;
        }
    }
    if (msg && n)
        msg[0] = '\0';
    return 0;
}
#elif defined(__aarch64__)
/* d8-d15 are callee-saved on AArch64, but only their low 64 bits (AAPCS64
 * 6.1.2); the _vec trampolines seed d(8+k) low == 8+k, so a restored register
 * reads back vec[i].u64[0] == i. The upper lane is caller-saved (not checked). */
int asmtest_check_abi_vec(const regs_t *r, char *msg, size_t n) {
    for (unsigned i = 8; i <= 15; i++) {
        if (r->vec[i].u64[0] != (uint64_t)i) {
            if (msg && n)
                snprintf(msg, n, "d%u not restored (got 0x%llx, expected 0x%x)",
                         i, (unsigned long long)r->vec[i].u64[0], i);
            return 1;
        }
    }
    if (msg && n)
        msg[0] = '\0';
    return 0;
}
#endif

#if defined(ASMTEST_ABI_WIN64) || defined(__aarch64__)
void asmtest_assert_abi_vec(const char *file, int line, const regs_t *r) {
    char msg[128];
    if (asmtest_check_abi_vec(r, msg, sizeof msg))
        asmtest_fail(file, line, "ASSERT_ABI_PRESERVED_VEC: %s", msg);
}
#endif

void asmtest_assert_flag(const char *file, int line, const regs_t *r,
                         unsigned long mask, int want_set, const char *name) {
    char msg[128];
    if (asmtest_check_flag(r, mask, want_set, name, msg, sizeof msg))
        asmtest_fail(file, line, "ASSERT_%s", msg);
}

/* Distance between two doubles in units in the last place (ULPs). Maps the
 * sign-magnitude bit pattern to a monotonic ordering so adjacent representable
 * values differ by 1. Assumes neither is NaN. */
static uint64_t fp_ulp_distance(double a, double b) {
    int64_t ia, ib;
    memcpy(&ia, &a, sizeof ia);
    memcpy(&ib, &b, sizeof ib);
    if (ia < 0)
        ia = (int64_t)0x8000000000000000ULL - ia;
    if (ib < 0)
        ib = (int64_t)0x8000000000000000ULL - ib;
    /* Take the gap in uint64_t. For the larger-minus-smaller key the true distance is
     * in [0, 2^64) and equals the modular unsigned subtraction exactly, whereas the
     * signed `ia - ib` overflows int64_t for far-apart operands (e.g. ASSERT_DNEAR(
     * -DBL_MAX, DBL_MAX)) — UB that trips this repo's own `make sanitize` UBSan lane. */
    return ia > ib ? (uint64_t)ia - (uint64_t)ib : (uint64_t)ib - (uint64_t)ia;
}

/* 32-bit float ULP distance, analogous to fp_ulp_distance (same unsigned-gap rule). */
static uint32_t fp_ulp_distance_f(float a, float b) {
    int32_t ia, ib;
    memcpy(&ia, &a, sizeof ia);
    memcpy(&ib, &b, sizeof ib);
    if (ia < 0)
        ia = (int32_t)0x80000000U - ia;
    if (ib < 0)
        ib = (int32_t)0x80000000U - ib;
    return ia > ib ? (uint32_t)ia - (uint32_t)ib : (uint32_t)ib - (uint32_t)ia;
}

void asmtest_assert_double_eq(const char *file, int line, double actual,
                              double expected) {
    if (!(actual == expected)) /* '==' is false for NaN, which is intended */
        asmtest_fail(file, line, "ASSERT_*EQ (double): %.17g != %.17g", actual,
                     expected);
}

void asmtest_assert_double_near(const char *file, int line, double actual,
                                double expected, unsigned long max_ulps) {
    if (isnan(actual) || isnan(expected)) {
        if (isnan(actual) && isnan(expected))
            return;
        asmtest_fail(file, line,
                     "ASSERT_*NEAR (double): NaN mismatch (%.17g "
                     "vs %.17g)",
                     actual, expected);
    }
    uint64_t d = fp_ulp_distance(actual, expected);
    if (d > max_ulps)
        asmtest_fail(file, line,
                     "ASSERT_*NEAR (double): %.17g vs %.17g differ by %llu "
                     "ulps (max %lu)",
                     actual, expected, (unsigned long long)d, max_ulps);
}

void asmtest_assert_float_eq(const char *file, int line, float actual,
                             float expected) {
    if (!(actual == expected))
        asmtest_fail(file, line, "ASSERT_FEQ (float): %.9g != %.9g",
                     (double)actual, (double)expected);
}

void asmtest_assert_float_near(const char *file, int line, float actual,
                               float expected, unsigned long max_ulps) {
    if (isnan(actual) || isnan(expected)) {
        if (isnan(actual) && isnan(expected))
            return;
        asmtest_fail(file, line,
                     "ASSERT_FNEAR (float): NaN mismatch (%.9g vs "
                     "%.9g)",
                     (double)actual, (double)expected);
    }
    uint32_t d = fp_ulp_distance_f(actual, expected);
    if (d > max_ulps)
        asmtest_fail(file, line,
                     "ASSERT_FNEAR (float): %.9g vs %.9g differ by %u ulps "
                     "(max %lu)",
                     (double)actual, (double)expected, d, max_ulps);
}

void asmtest_assert_vec_eq(const char *file, int line, const char *idxexpr,
                           const unsigned char *actual,
                           const unsigned char *expected) {
    size_t i = 0;
    while (i < 16 && actual[i] == expected[i])
        i++;
    if (i == 16)
        return;
    char expect_hex[64], actual_hex[64];
    hexdump_window(expect_hex, sizeof expect_hex, expected, 0, 16);
    hexdump_window(actual_hex, sizeof actual_hex, actual, 0, 16);
    asmtest_fail(file, line,
                 "ASSERT_VEC_EQ(vec[%s]): first diff at byte %zu "
                 "(0x%02x != 0x%02x)\n"
                 "       expect: %s\n"
                 "       actual: %s",
                 idxexpr, i, expected[i], actual[i], expect_hex, actual_hex);
}

void asmtest_assert_vec256_eq(const char *file, int line, const char *idxexpr,
                              const unsigned char *actual,
                              const unsigned char *expected) {
    size_t i = 0;
    while (i < 32 && actual[i] == expected[i])
        i++;
    if (i == 32)
        return;
    char expect_hex[128], actual_hex[128];
    hexdump_window(expect_hex, sizeof expect_hex, expected, 0, 32);
    hexdump_window(actual_hex, sizeof actual_hex, actual, 0, 32);
    asmtest_fail(file, line,
                 "ASSERT_VEC256_EQ(vec[%s]): first diff at byte %zu "
                 "(0x%02x != 0x%02x)\n"
                 "       expect: %s\n"
                 "       actual: %s",
                 idxexpr, i, expected[i], actual[i], expect_hex, actual_hex);
}

void asmtest_assert_vec512_eq(const char *file, int line, const char *idxexpr,
                              const unsigned char *actual,
                              const unsigned char *expected) {
    size_t i = 0;
    while (i < 64 && actual[i] == expected[i])
        i++;
    if (i == 64)
        return;
    char expect_hex[256], actual_hex[256];
    hexdump_window(expect_hex, sizeof expect_hex, expected, 0, 64);
    hexdump_window(actual_hex, sizeof actual_hex, actual, 0, 64);
    asmtest_fail(file, line,
                 "ASSERT_VEC512_EQ(vec[%s]): first diff at byte %zu "
                 "(0x%02x != 0x%02x)\n"
                 "       expect: %s\n"
                 "       actual: %s",
                 idxexpr, i, expected[i], actual[i], expect_hex, actual_hex);
}

/* CPU feature probes (x86-64). AVX/AVX2/AVX-512 each need both the CPUID
 * feature bit AND OS enablement of the wider register state (OSXSAVE + the
 * matching XCR0 bits), so a kernel that doesn't save the state can't be used. */
#if defined(__x86_64__)
#include <cpuid.h>
static int xcr0_has(unsigned mask) {
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (eax & mask) == mask;
}
int asmtest_cpu_has_avx2(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 0;
    if (!(c & (1u << 27)) || !(c & (1u << 28))) /* OSXSAVE + AVX */
        return 0;
    if (!xcr0_has(0x6)) /* XMM + YMM state enabled by the OS */
        return 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d))
        return 0;
    return (b & (1u << 5)) != 0; /* AVX2 */
}
int asmtest_cpu_has_avx512f(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d))
        return 0;
    if (!(c & (1u << 27))) /* OSXSAVE */
        return 0;
    if (!xcr0_has(0xe6)) /* opmask + ZMM_Hi256 + Hi16_ZMM (+ YMM/XMM) */
        return 0;
    if (!__get_cpuid_count(7, 0, &a, &b, &c, &d))
        return 0;
    return (b & (1u << 16)) != 0; /* AVX-512F */
}
#else
int asmtest_cpu_has_avx2(void) { return 0; }
int asmtest_cpu_has_avx512f(void) { return 0; }
#endif

/* Whether the native 128-bit vector capture path (asm_call_capture_vec and the
 * ASM_VCALL* macros) exists on this target. x86-64 (SSE) and AArch64 (NEON) have
 * a 128-bit vector register file and a real trampoline; rv64gc has NO vector
 * registers (its _vec trampolines are ret-only stubs, and RVV would be a future
 * arm), so it returns 0 there and the ASM_VCALL macros SELF-SKIP rather than
 * call a stub. The designed hook for an eventual RVV capture arm. */
int asmtest_cpu_has_vec128(void) {
#if defined(__x86_64__) || defined(__aarch64__)
    return 1;
#else
    return 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Guard-page buffers                                                  */
/*                                                                     */
/* POSIX mmap/mprotect implementation; the Win64 tier uses the         */
/* VirtualAlloc/PAGE_NOACCESS equivalents in src/platform_win32.c.     */
/* ------------------------------------------------------------------ */
#if !defined(_WIN32)

/* Returns 0 (never a valid usable size) if rounding n up to a page boundary
 * would wrap: n > SIZE_MAX - 2*pg leaves no room for the two-page total a
 * caller builds from the result (the usable region plus a guard page) --
 * conservative, since no such allocation could ever succeed anyway. Without
 * this, n within a page of SIZE_MAX wrapped `n + pg - 1`, usable collapsed to
 * pg via the `== 0` fallback below, the (small) 2-page mmap succeeded, and
 * `base + (usable - n)` in the callers wrapped to a pointer inside the
 * PROT_NONE guard page instead of failing. */
static size_t round_up_page(size_t n, long pg) {
    if (n > SIZE_MAX - 2 * (size_t)pg)
        return 0;
    size_t usable = ((n + (size_t)pg - 1) / (size_t)pg) * (size_t)pg;
    return usable == 0 ? (size_t)pg : usable;
}

void *asmtest_guarded_alloc(size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t usable = round_up_page(n, pg);
    if (usable == 0)
        return NULL;
    size_t total = usable + (size_t)pg;
    unsigned char *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    if (mprotect(base + usable, (size_t)pg, PROT_NONE) != 0) {
        munmap(base, total);
        return NULL;
    }
    /* Place the buffer so its last byte abuts the guard page. */
    return base + (usable - n);
}

void asmtest_guarded_free(void *p, size_t n) {
    if (p == NULL)
        return;
    long pg = sysconf(_SC_PAGESIZE);
    size_t usable = round_up_page(n, pg);
    if (usable == 0) /* a wrapped size can never have been allocated */
        return;
    unsigned char *base = (unsigned char *)p - (usable - n);
    munmap(base, usable + (size_t)pg);
}

void *asmtest_guarded_alloc_under(size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t usable = round_up_page(n, pg);
    if (usable == 0)
        return NULL;
    size_t total = (size_t)pg + usable;
    unsigned char *base = mmap(NULL, total, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED)
        return NULL;
    if (mprotect(base, (size_t)pg, PROT_NONE) != 0) { /* leading guard page */
        munmap(base, total);
        return NULL;
    }
    /* Buffer starts right after the guard page, so buf[-1] faults. */
    return base + (size_t)pg;
}

void asmtest_guarded_free_under(void *p, size_t n) {
    if (p == NULL)
        return;
    long pg = sysconf(_SC_PAGESIZE);
    size_t usable = round_up_page(n, pg);
    if (usable == 0) /* a wrapped size can never have been allocated */
        return;
    unsigned char *base = (unsigned char *)p - (size_t)pg;
    munmap(base, (size_t)pg + usable);
}

#endif /* !_WIN32 */

/* ------------------------------------------------------------------ */
/* Large struct-by-value parameter (arch-divergent dispatch)           */
/*                                                                     */
/* These framework helpers drive the System V capture trampoline; the  */
/* Win64 tier uses the asm_call_capture*_win64 entry points instead, so */
/* they are omitted from the Win64 runner build (they would otherwise   */
/* pull in unresolved SysV trampoline symbols).                         */
/* ------------------------------------------------------------------ */
#if !defined(_WIN32)

#if defined(__x86_64__)
extern void asm_bigstruct_x86(regs_t *out, void *fn, const long *iargs,
                              int niargs, const void *sptr, size_t ssize);
#endif

void asm_call_capture_bigstruct(regs_t *out, void *fn, const long *iargs,
                                int niargs, const void *sptr, size_t ssize) {
#if defined(__aarch64__) || (defined(__riscv) && __riscv_xlen == 64)
    /* AAPCS64 and the rv64 psABI both pass a >16-byte (>2*XLEN) struct as a
     * pointer to a copy; for a read-only callee, passing the struct's own
     * address is equivalent. */
    long args[16];
    int i;
    for (i = 0; i < niargs && i < 15; i++)
        args[i] = iargs[i];
    args[i] = (long)(uintptr_t)sptr;
    (void)ssize;
    asm_call_capture_args(out, fn, args, i + 1);
#else
    /* SysV: copy the struct inline onto the stack. */
    asm_bigstruct_x86(out, fn, iargs, niargs, sptr, ssize);
#endif
}

#endif /* !_WIN32 (bigstruct dispatch) */

/* ------------------------------------------------------------------ */
/* Differential / property testing (Phase 7)                           */
/* ------------------------------------------------------------------ */

uint64_t asmtest_rng_u64(asmtest_rng_t *rng) {
    /* splitmix64: fast, well-distributed, and trivially seedable. */
    uint64_t z = (rng->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

long asmtest_rng_long(asmtest_rng_t *rng) { return (long)asmtest_rng_u64(rng); }

long asmtest_rng_range(asmtest_rng_t *rng, long lo, long hi) {
    if (hi <= lo)
        return lo;
    /* Span in uint64_t: `hi - lo` as signed longs overflows for ranges wider than
     * LONG_MAX (e.g. [LONG_MIN, LONG_MAX]) — UB, and the wrap makes `(uint64_t)(hi-lo)+1`
     * evaluate to 0, so the `% span` below is a division by zero (SIGFPE). Both operands
     * unsigned keeps it defined; span wraps to 0 iff the range is the full 2^64 width. */
    uint64_t span =
        (uint64_t)hi - (uint64_t)lo + 1; /* inclusive of both endpoints */
    if (span == 0)                       /* whole range — every value is in */
        return (long)asmtest_rng_u64(rng);
    /* Offset added in uint64_t too: lo + offset is in [lo, hi] so it never truly
     * overflows, but the signed form `lo + (long)offset` can when offset > LONG_MAX. */
    return (long)((uint64_t)lo + asmtest_rng_u64(rng) % span);
}

/* asmtest_seed + the differential helpers + the POSIX signal-based crash/timeout
 * handlers below drive the System V trampoline / sigaction; the Win64 tier uses
 * the asm_call_capture*_win64 entry points and the platform_win32.c test
 * facility, so they are gated out of the Win64 runner build. */
#if !defined(_WIN32)

/* The seed in effect for this run: ASMTEST_SEED (decimal or 0x-hex) if set,
 * otherwise a fixed default so failures reproduce. Resolved once and cached. */
static uint64_t asmtest_seed(void) {
    static int init = 0;
    static uint64_t seed = 0x243F6A8885A308D3ULL; /* default (digits of pi) */
    if (!init) {
        const char *e = getenv("ASMTEST_SEED");
        if (e && *e)
            seed = (uint64_t)strtoull(e, NULL, 0);
        init = 1;
    }
    return seed;
}

/* One engine drives all three arities: `ref` travels as a generic function
 * pointer and is called back through its real type, selected by `arity`. */
typedef void (*asmtest_anyfn_t)(void);

/* Run fn through the real ABI and ref on one tuple; return 1 if they disagree
 * (filling *got and *want either way). */
static int match_ref_diverges(void *fn, asmtest_anyfn_t ref, int arity,
                              const long *a, long *got, long *want) {
    long expected;
    switch (arity) {
    case 1:
        expected = ((asmtest_ref1_fn)ref)(a[0]);
        break;
    case 2:
        expected = ((asmtest_ref2_fn)ref)(a[0], a[1]);
        break;
    default:
        expected = ((asmtest_ref3_fn)ref)(a[0], a[1], a[2]);
        break;
    }
    long args[8] = {0};
    memcpy(args, a, (size_t)arity * sizeof *args);
    regs_t r;
    asm_call_capture_args(&r, fn, args, arity);
    *got = (long)r.ret;
    *want = expected;
    return *got != expected;
}

/* Shrink a failing tuple toward the boundary values where asm bugs actually
 * live, so the report leads with e.g. [0, -1] instead of a raw 64-bit draw.
 * One ordered pass per position: try 0 / 1 / -1 / LONG_MAX / LONG_MIN, else
 * halve toward zero — keeping only replacements that still diverge from the
 * model, so the tuple stays a counterexample throughout. Deterministic (no
 * RNG) and bounded (~70 extra calls per position). Greedy per position, so
 * not globally minimal — it is a readability aid, not a proof. */
static void match_ref_shrink(void *fn, asmtest_anyfn_t ref, int arity,
                             long *a) {
    static const long cand[] = {0, 1, -1, LONG_MAX, LONG_MIN};
    long got, want;
    for (int i = 0; i < arity; i++) {
        long keep = a[i];
        int replaced = 0;
        for (size_t c = 0; c < sizeof cand / sizeof cand[0]; c++) {
            if (cand[c] == keep)
                continue;
            a[i] = cand[c];
            if (match_ref_diverges(fn, ref, arity, a, &got, &want)) {
                replaced = 1;
                break;
            }
        }
        if (!replaced) {
            a[i] = keep;
            while (a[i] != 0) { /* halve toward zero while it still fails */
                long prev = a[i];
                a[i] = prev / 2;
                if (!match_ref_diverges(fn, ref, arity, a, &got, &want)) {
                    a[i] = prev;
                    break;
                }
            }
        }
    }
}

static void fmt_tuple(char *buf, size_t cap, const long *a, int arity) {
    size_t off = (size_t)snprintf(buf, cap, "[");
    for (int i = 0; i < arity; i++)
        off += (size_t)snprintf(buf + off, off < cap ? cap - off : 0, "%s%ld",
                                i ? ", " : "", a[i]);
    snprintf(buf + off, off < cap ? cap - off : 0, "]");
}

static void match_ref_run(const char *file, int line, const char *fnexpr,
                          void *fn, asmtest_anyfn_t ref, asmtest_gen_fn gen,
                          int trials, int arity) {
    asmtest_rng_t rng = {asmtest_seed()};
    for (int t = 0; t < trials; t++) {
        long a[8] = {0};
        int n = gen(&rng, a, 8);
        if (n != arity)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF%d(%s): generator returned arity "
                         "%d, expected %d",
                         arity, fnexpr, n, arity);
        long got, want;
        if (!match_ref_diverges(fn, ref, arity, a, &got, &want))
            continue;
        long s[8];
        memcpy(s, a, sizeof s);
        match_ref_shrink(fn, ref, arity, s);
        char orig[176], shrunk[224] = "";
        fmt_tuple(orig, sizeof orig, a, arity);
        if (memcmp(s, a, (size_t)arity * sizeof *a) != 0) {
            long sg, sw;
            char st[176];
            match_ref_diverges(fn, ref, arity, s, &sg, &sw);
            fmt_tuple(st, sizeof st, s, arity);
            snprintf(shrunk, sizeof shrunk,
                     "; shrinks to %s: got %ld, reference %ld", st, sg, sw);
        }
        asmtest_fail(file, line,
                     "ASSERT_MATCHES_REF%d(%s): trial %d input %s: got %ld "
                     "(0x%lx), reference %ld (0x%lx)%s [seed=0x%llx]",
                     arity, fnexpr, t, orig, got, (unsigned long)got, want,
                     (unsigned long)want, shrunk,
                     (unsigned long long)asmtest_seed());
    }
}

void asmtest_match_ref1(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref1_fn ref, asmtest_gen_fn gen,
                        int trials) {
    match_ref_run(file, line, fnexpr, fn, (asmtest_anyfn_t)ref, gen, trials, 1);
}

void asmtest_match_ref2(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref2_fn ref, asmtest_gen_fn gen,
                        int trials) {
    match_ref_run(file, line, fnexpr, fn, (asmtest_anyfn_t)ref, gen, trials, 2);
}

void asmtest_match_ref3(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref3_fn ref, asmtest_gen_fn gen,
                        int trials) {
    match_ref_run(file, line, fnexpr, fn, (asmtest_anyfn_t)ref, gen, trials, 3);
}

/* --- FP differential engine (ASSERT_MATCHES_FREF{1,2,3}) --- */

/* As match_ref_diverges, but through the FP register file, judged by ULP
 * distance. NaN matches only NaN (any max_ulps). */
static int match_fref_diverges(void *fn, asmtest_anyfn_t ref, int arity,
                               const double *f, unsigned long max_ulps,
                               double *got, double *want, uint64_t *dist) {
    double expected;
    switch (arity) {
    case 1:
        expected = ((asmtest_fref1_fn)ref)(f[0]);
        break;
    case 2:
        expected = ((asmtest_fref2_fn)ref)(f[0], f[1]);
        break;
    default:
        expected = ((asmtest_fref3_fn)ref)(f[0], f[1], f[2]);
        break;
    }
    long ia[6] = {0};
    double fa[8] = {0};
    memcpy(fa, f, (size_t)arity * sizeof *fa);
    regs_t r;
    asm_call_capture_fp(&r, fn, ia, fa);
    *got = r.fret;
    *want = expected;
    *dist = 0;
    if (isnan(r.fret) || isnan(expected))
        return !(isnan(r.fret) && isnan(expected));
    *dist = fp_ulp_distance(r.fret, expected);
    return *dist > max_ulps;
}

static void fmt_ftuple(char *buf, size_t cap, const double *f, int arity) {
    size_t off = (size_t)snprintf(buf, cap, "[");
    for (int i = 0; i < arity; i++)
        off += (size_t)snprintf(buf + off, off < cap ? cap - off : 0, "%s%.17g",
                                i ? ", " : "", f[i]);
    snprintf(buf + off, off < cap ? cap - off : 0, "]");
}

static void match_fref_run(const char *file, int line, const char *fnexpr,
                           void *fn, asmtest_anyfn_t ref, asmtest_fgen_fn gen,
                           int trials, int arity, unsigned long max_ulps) {
    asmtest_rng_t rng = {asmtest_seed()};
    for (int t = 0; t < trials; t++) {
        double f[8] = {0};
        int n = gen(&rng, f, 8);
        if (n != arity)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_FREF%d(%s): generator returned arity "
                         "%d, expected %d",
                         arity, fnexpr, n, arity);
        double got, want;
        uint64_t dist;
        if (!match_fref_diverges(fn, ref, arity, f, max_ulps, &got, &want,
                                 &dist))
            continue;
        char tup[224];
        fmt_ftuple(tup, sizeof tup, f, arity);
        asmtest_fail(file, line,
                     "ASSERT_MATCHES_FREF%d(%s): trial %d input %s: got %.17g,"
                     " reference %.17g (%llu ulps, max %lu) [seed=0x%llx]",
                     arity, fnexpr, t, tup, got, want, (unsigned long long)dist,
                     max_ulps, (unsigned long long)asmtest_seed());
    }
}

void asmtest_match_fref1(const char *file, int line, const char *fnexpr,
                         void *fn, asmtest_fref1_fn ref, asmtest_fgen_fn gen,
                         int trials, unsigned long max_ulps) {
    match_fref_run(file, line, fnexpr, fn, (asmtest_anyfn_t)ref, gen, trials, 1,
                   max_ulps);
}

void asmtest_match_fref2(const char *file, int line, const char *fnexpr,
                         void *fn, asmtest_fref2_fn ref, asmtest_fgen_fn gen,
                         int trials, unsigned long max_ulps) {
    match_fref_run(file, line, fnexpr, fn, (asmtest_anyfn_t)ref, gen, trials, 2,
                   max_ulps);
}

void asmtest_match_fref3(const char *file, int line, const char *fnexpr,
                         void *fn, asmtest_fref3_fn ref, asmtest_fgen_fn gen,
                         int trials, unsigned long max_ulps) {
    match_fref_run(file, line, fnexpr, fn, (asmtest_anyfn_t)ref, gen, trials, 3,
                   max_ulps);
}

/* ------------------------------------------------------------------ */
/* Crash handling: turn fatal signals into test failures               */
/* ------------------------------------------------------------------ */

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGBUS:
        return "SIGBUS";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    case SIGABRT:
        return "SIGABRT";
    case SIGALRM:
        return "SIGALRM";
    default:
        return "signal";
    }
}

static void asmtest_on_signal(int sig) {
    if (!asmtest_in_test) {
        /* Crash outside a test body: restore default and re-raise. */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    if (sig == SIGALRM) {
        snprintf(asmtest_msg, sizeof asmtest_msg, "timed out after %d s",
                 asmtest_timeout_secs);
        asmtest_loc_file = "(timeout)";
    } else {
        snprintf(asmtest_msg, sizeof asmtest_msg, "caught fatal signal %d (%s)",
                 sig, sig_name(sig));
        asmtest_loc_file = "(signal)";
    }
    asmtest_loc_line = 0;
    siglongjmp(asmtest_jmp, JMP_FAIL);
}

static void install_handlers(void) {
    /* Alternate signal stack so a stack-overflow SIGSEGV in a routine under test
     * can still run the handler — the thread's normal stack is unusable then, so
     * without SA_ONSTACK the handler cannot run and the whole runner dies. This
     * matters for the in-process paths (--no-fork, --bench, and the fork/pipe
     * fallback), where the forked-child model is not containing the crash. A fixed
     * 64 KiB (SIGSTKSZ is a runtime value on modern glibc, unusable as an array
     * size). Registered once; harmless if re-installed. */
    static char altstack[65536];
    stack_t ss;
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = asmtest_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL); /* per-test timeout (see run_one) */
}

#else /* _WIN32: crashes/timeouts are handled per-test by the test facility */

static void install_handlers(void) {}

#endif /* !_WIN32 */

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

static void run_hooks(const char *suite, int kind) {
    for (asmtest_hook_t *h = asmtest_hooks; h != NULL; h = h->next) {
        if (h->kind == kind && strcmp(h->suite, suite) == 0)
            h->fn();
    }
}

/* Run one test through setup -> body -> teardown; returns an ST_* outcome.
 * A per-test alarm() converts an infinite loop into a SIGALRM the handler turns
 * into a timeout failure (works even in-process). On return the failure/skip
 * message lives in asmtest_msg / asmtest_loc_*. */
#if !defined(_WIN32)
static int run_one(asmtest_case_t *tc) {
    asmtest_in_test = 1;
    if (asmtest_timeout_secs > 0)
        alarm((unsigned)asmtest_timeout_secs);

    /* setup */
    int setup_rc = sigsetjmp(asmtest_jmp, 1);
    if (setup_rc != 0) {
        alarm(0);
        asmtest_in_test = 0;
        /* SKIP() in a SETUP hook skips the whole test (GTEST_SKIP-in-SetUp
         * idiom); any other longjmp is a genuine setup failure/crash. */
        return setup_rc == JMP_SKIP ? ST_SKIP : ST_FAIL;
    }
    run_hooks(tc->suite, 0);

    /* body */
    int outcome = ST_PASS;
    int rc = sigsetjmp(asmtest_jmp, 1);
    if (rc == 0)
        tc->fn();
    else if (rc == JMP_SKIP)
        outcome = ST_SKIP;
    else
        outcome = ST_FAIL;

    /* Cancel the per-test timeout as soon as the body is done — BEFORE the
     * teardown sigsetjmp frame. Otherwise a late SIGALRM (the timer already
     * expiring) could fire after that frame has returned but before alarm(0),
     * siglongjmp-ing into a stack frame that no longer exists (UB). The teardown
     * hooks run untimed; they are expected to be quick, and the default forked
     * model still contains a hanging teardown. */
    alarm(0);

    /* teardown — runs because setup succeeded; may itself fail or SKIP */
    int td_rc = sigsetjmp(asmtest_jmp, 1);
    if (td_rc == 0)
        run_hooks(tc->suite, 1);
    else if (td_rc == JMP_SKIP) {
        if (outcome == ST_PASS)
            outcome =
                ST_SKIP; /* SKIP() in TEARDOWN downgrades a pass to skip */
    } else if (outcome != ST_FAIL)
        outcome = ST_FAIL; /* teardown failed/crashed */

    asmtest_in_test = 0;
    return outcome;
}
#else
/* Fill asmtest_msg/loc for a crash or timeout reason. A JMP_FAIL already has its
 * message from asmtest_fail and a JMP_SKIP from asmtest_skip, so those are left
 * untouched. */
static void win32_set_reason_msg(int reason) {
    if (reason == ASMTEST_WIN32_REASON_CRASH) {
        snprintf(asmtest_msg, sizeof asmtest_msg,
                 "caught fatal exception 0x%lx", asmtest_win32_test_fault.code);
        asmtest_loc_file = "(exception)";
        asmtest_loc_line = 0;
    } else if (reason == ASMTEST_WIN32_REASON_TIMEOUT) {
        snprintf(asmtest_msg, sizeof asmtest_msg, "timed out after %d s",
                 asmtest_timeout_secs);
        asmtest_loc_file = "(timeout)";
        asmtest_loc_line = 0;
    }
}

/* Win64 in-process run_one: the test facility arms a vectored exception handler
 * (crash) and a watchdog (timeout); a crash, timeout, or assertion failure all
 * land via __builtin_longjmp at the single recovery point with the reason set.
 * Mirrors the POSIX run_one's three-scope setup -> body -> teardown structure so
 * teardown runs whenever setup succeeded — even after a body failure, skip,
 * crash, or timeout (finding #33). Each phase re-establishes the recovery point;
 * the body and teardown phases re-arm the crash/timeout facility (an assertion
 * disarmed it) so a fault or hang in teardown is folded into the outcome. */
static int run_one(asmtest_case_t *tc) {
    asmtest_in_test = 1;
    unsigned timeout_ms =
        asmtest_timeout_secs > 0 ? (unsigned)asmtest_timeout_secs * 1000u : 0;
    asmtest_win32_test_begin(timeout_ms);

    /* setup */
    if (__builtin_setjmp(asmtest_win32_test_recover) != 0) {
        /* SKIP() in a SETUP hook skips the whole test (GTEST_SKIP-in-SetUp
         * idiom); any other longjmp is a genuine setup failure/crash/timeout.
         * Teardown does NOT run — setup did not complete — matching POSIX. */
        int reason = asmtest_win32_test_reason;
        int outcome = (reason == JMP_SKIP) ? ST_SKIP : ST_FAIL;
        win32_set_reason_msg(reason);
        asmtest_win32_test_end();
        asmtest_in_test = 0;
        return outcome;
    }
    run_hooks(tc->suite, 0);

    /* body */
    int outcome = ST_PASS;
    asmtest_win32_test_rearm();
    if (__builtin_setjmp(asmtest_win32_test_recover) == 0) {
        tc->fn();
    } else {
        int reason = asmtest_win32_test_reason;
        if (reason == JMP_SKIP) {
            outcome = ST_SKIP;
        } else {
            outcome = ST_FAIL;
            win32_set_reason_msg(reason);
        }
    }

    /* teardown — runs because setup succeeded; may itself fail, skip, or crash */
    asmtest_win32_test_rearm();
    if (__builtin_setjmp(asmtest_win32_test_recover) == 0) {
        run_hooks(tc->suite, 1);
    } else {
        int reason = asmtest_win32_test_reason;
        if (reason == JMP_SKIP) {
            if (outcome == ST_PASS)
                outcome =
                    ST_SKIP; /* SKIP() in TEARDOWN downgrades a pass to skip */
        } else if (outcome != ST_FAIL) {
            outcome = ST_FAIL; /* teardown failed/crashed/timed out */
            win32_set_reason_msg(reason);
        }
    }

    asmtest_win32_test_end();
    asmtest_in_test = 0;
    return outcome;
}
#endif

/* ------------------------------------------------------------------ */
/* Per-test isolation (fork) and result plumbing                       */
/* ------------------------------------------------------------------ */

/* A finished test's outcome, accumulated by the parent for reporting. */
typedef struct {
    const char *suite;
    const char *name;
    int outcome; /* ST_PASS / ST_FAIL / ST_SKIP */
    int line;
    char file[256];
    char msg[sizeof asmtest_msg];
    double secs;
} test_result_t;

/* The subset of a result a forked child ships back over its pipe. */
typedef struct {
    int outcome;
    int line;
    char file[256];
    char msg[sizeof asmtest_msg];
} wire_result_t;

/* Snapshot the globals left by run_one into a wire record. */
static void capture_wire(wire_result_t *w, int outcome) {
    memset(w, 0, sizeof *w);
    w->outcome = outcome;
    w->line = asmtest_loc_line;
    snprintf(w->file, sizeof w->file, "%s",
             asmtest_loc_file ? asmtest_loc_file : "");
    snprintf(w->msg, sizeof w->msg, "%s", asmtest_msg);
}

static void apply_wire(test_result_t *res, const wire_result_t *w) {
    res->outcome = w->outcome;
    res->line = w->line;
    snprintf(res->file, sizeof res->file, "%s", w->file);
    snprintf(res->msg, sizeof res->msg, "%s", w->msg);
}

#if !defined(_WIN32) /* fork/pipe result plumbing — POSIX isolation path */
static int write_full(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static size_t read_full(int fd, void *buf, size_t n) {
    char *p = (char *)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break; /* EOF: child died before finishing its write */
        got += (size_t)r;
    }
    return got;
}
#endif /* !_WIN32 (fork/pipe plumbing) */

/* Run in this process. On POSIX, crashes that escape the signal handler (e.g.
 * SIGABRT from heap corruption) take down the whole runner — that's what --fork
 * avoids; on the Win64 tier this is the only path and the test facility's
 * vectored handler contains the common faults. */
static void run_inproc(asmtest_case_t *tc, test_result_t *res) {
    wire_result_t w;
    capture_wire(&w, run_one(tc));
    apply_wire(res, &w);
}

#if defined(_WIN32)
/* ---- Win64 per-test isolation via process re-exec (Track B) ---------------
 * There is no fork(): the parent re-execs THIS exe with --asmtest-child=<global
 * registration index>; the child runs that one test in-process (run_one, which
 * arms the vectored-handler best-effort guard) and writes its wire_result_t to a
 * temp file; the parent maps the file — or the child's death/overrun — into a
 * result, the same containment reap_child gives on POSIX. asmtest_win32_run /
 * _run_pool (src/platform_win32.c) supply the isolation, deadline, and -jN pool.
 * The global index is stable because re-exec re-runs the same registering
 * constructors in the same order. */

/* Set when this process IS a re-exec'd child (runs exactly one test). */
static int asmtest_child_index = -1;
static const char *asmtest_child_out = NULL;

static asmtest_case_t *asmtest_case_at(int gindex) {
    int i = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next, i++)
        if (i == gindex)
            return tc;
    return NULL;
}

/* A child writes its one result here; the parent reads it back. */
static void child_out_path(char *buf, size_t cap, int gindex) {
    char dir[MAX_PATH];
    DWORD d = GetTempPathA((DWORD)sizeof dir, dir);
    if (d == 0 || d >= sizeof dir)
        snprintf(dir, sizeof dir, ".\\");
    snprintf(buf, cap, "%sasmtest_%lu_%d.bin", dir,
             (unsigned long)GetCurrentProcessId(), gindex);
}

/* Child command line: this exe, the test's global index, the out path, and
 * --timeout=0 — the PARENT's deadline (asmtest_win32_run) is the timeout, so the
 * child needs no watchdog of its own. */
static void child_cmdline(char *buf, size_t cap, const char *exe, int gindex,
                          const char *out) {
    snprintf(buf, cap,
             "\"%s\" --asmtest-child=%d --asmtest-child-out=\"%s\" --timeout=0",
             exe, gindex, out);
}

static int read_wire_file(const char *path, wire_result_t *w) {
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    size_t got = fread(w, 1, sizeof *w, f);
    fclose(f);
    return got == sizeof *w;
}

/* Map one isolated run (the asmtest_win32_run verdict + the wire file) into a
 * result; synthesize crash/timeout when the child died before reporting. */
static void apply_isolated(test_result_t *res, asmtest_win32_run_t verdict,
                           unsigned long exit_code, const char *out) {
    wire_result_t w;
    int have = (verdict == ASMTEST_W32_OK) && read_wire_file(out, &w);
    remove(out);
    if (have) {
        apply_wire(res, &w);
        return;
    }
    res->outcome = ST_FAIL;
    res->line = 0;
    snprintf(res->file, sizeof res->file, "(child)");
    if (verdict == ASMTEST_W32_TIMEOUT)
        snprintf(res->msg, sizeof res->msg, "timed out after %d s (killed)",
                 asmtest_timeout_secs);
    else if (verdict == ASMTEST_W32_CRASH)
        snprintf(res->msg, sizeof res->msg, "crashed: fatal exception 0x%lx",
                 exit_code);
    else if (verdict == ASMTEST_W32_SPAWN_FAIL)
        snprintf(res->msg, sizeof res->msg, "failed to spawn child process");
    else
        snprintf(res->msg, sizeof res->msg, "no result reported by child");
}

static unsigned isolation_timeout_ms(void) {
    return asmtest_timeout_secs > 0 ? (unsigned)asmtest_timeout_secs * 1000u
                                    : 0;
}

/* Serial isolation: re-exec one child, wait, map. */
static void run_isolated_win32(int gindex, test_result_t *res) {
    char exe[MAX_PATH], out[MAX_PATH], cmd[MAX_PATH * 2];
    if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) == 0) {
        run_inproc(asmtest_case_at(gindex), res); /* fallback: in-process */
        return;
    }
    child_out_path(out, sizeof out, gindex);
    remove(out);
    child_cmdline(cmd, sizeof cmd, exe, gindex, out);
    unsigned long code = 0;
    asmtest_win32_run_t v =
        asmtest_win32_run(cmd, isolation_timeout_ms(), &code);
    apply_isolated(res, v, code, out);
}

/* Parallel isolation: the -jN pool (WaitForMultipleObjects) over re-exec'd
 * children. Results fill in registration order; returns 0 on success, -1 if the
 * exe path or the pool machinery was unavailable (caller falls back to serial). */
static int run_parallel_win32(asmtest_case_t **sel, const int *gidx,
                              test_result_t *results, int n, int jobs) {
    char exe[MAX_PATH];
    if (GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe) == 0)
        return -1;
    char **cmds = (char **)calloc((size_t)n, sizeof *cmds);
    char **outs = (char **)calloc((size_t)n, sizeof *outs);
    asmtest_win32_run_t *verds =
        (asmtest_win32_run_t *)calloc((size_t)n, sizeof *verds);
    unsigned long *codes = (unsigned long *)calloc((size_t)n, sizeof *codes);
    if (!cmds || !outs || !verds || !codes) {
        free(cmds);
        free(outs);
        free(verds);
        free(codes);
        return -1;
    }
    for (int i = 0; i < n; i++) {
        outs[i] = (char *)asmtest_xalloc(malloc(MAX_PATH), "child out path");
        cmds[i] = (char *)asmtest_xalloc(malloc(MAX_PATH * 2), "child cmdline");
        child_out_path(outs[i], MAX_PATH, gidx[i]);
        remove(outs[i]);
        child_cmdline(cmds[i], MAX_PATH * 2, exe, gidx[i], outs[i]);
    }
    int rc = asmtest_win32_run_pool((const char *const *)cmds, n, jobs,
                                    isolation_timeout_ms(), verds, codes);
    for (int i = 0; i < n; i++) {
        if (rc == 0) {
            results[i].suite = sel[i]->suite;
            results[i].name = sel[i]->name;
            apply_isolated(&results[i], verds[i], codes[i], outs[i]);
        }
        free(outs[i]);
        free(cmds[i]);
    }
    free(cmds);
    free(outs);
    free(verds);
    free(codes);
    return rc;
}
#endif /* _WIN32 (re-exec isolation) */

#if !defined(_WIN32) /* fork-based isolation + the parallel pool — POSIX only */

/* Turn a finished child's pipe payload (`got` bytes of `w`) and wait `status`
 * into a result. A full wire record wins; otherwise the child died before
 * reporting and we synthesize the outcome from how it was killed. Shared by the
 * serial (run_forked) and parallel (run_parallel) paths. */
static void reap_child(test_result_t *res, size_t got, const wire_result_t *w,
                       int status) {
    if (got == sizeof *w) {
        apply_wire(res, w);
        return;
    }
    res->outcome = ST_FAIL;
    res->line = 0;
    snprintf(res->file, sizeof res->file, "(child)");
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGALRM)
            snprintf(res->msg, sizeof res->msg, "timed out after %d s (killed)",
                     asmtest_timeout_secs);
        else
            snprintf(res->msg, sizeof res->msg,
                     "crashed: killed by signal %d (%s)", sig, sig_name(sig));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        snprintf(res->msg, sizeof res->msg,
                 "child exited abnormally (status %d)", WEXITSTATUS(status));
    } else {
        snprintf(res->msg, sizeof res->msg, "no result reported by child");
    }
}

/* Run in a forked child so a crash, abort, or hard hang is contained: the
 * child reports its outcome up a pipe; if it dies before reporting, the parent
 * synthesizes the result from the wait status. */
static void run_forked(asmtest_case_t *tc, test_result_t *res) {
    int fds[2];
    if (pipe(fds) != 0) {
        run_inproc(tc, res);
        return;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        run_inproc(tc, res);
        return;
    }
    if (pid == 0) {
        /* Child: run, ship the result, exit without flushing inherited bufs. */
        close(fds[0]);
        wire_result_t w;
        capture_wire(&w, run_one(tc));
        write_full(fds[1], &w, sizeof w);
        fflush(stdout); /* preserve anything the test itself printed */
        fflush(stderr);
        _exit(0);
    }
    /* Parent: read the child's result, then reap it. */
    close(fds[1]);
    wire_result_t w;
    size_t got = read_full(fds[0], &w, sizeof w);
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    reap_child(res, got, &w, status);
}

/* One in-flight (or free) child in the parallel scheduler. */
typedef struct {
    pid_t pid;
    int fd;  /* read end of the child's result pipe; -1 when the slot is free */
    int idx; /* index into sel[]/results[] this child is running */
    double t0;
} job_slot_t;

/* Spawn the test at sel[idx] into a free slot, recording its pid/pipe/timer.
 * On a fork/pipe failure, run it in-process synchronously and leave the slot
 * free (so the result is already finalized). Returns 1 if a child is now
 * in-flight in *slot, 0 if the test was handled in-process. */
static int spawn_job(job_slot_t *slot, asmtest_case_t **sel,
                     test_result_t *results, int idx) {
    test_result_t *r = &results[idx];
    r->suite = sel[idx]->suite;
    r->name = sel[idx]->name;

    int fds[2];
    if (pipe(fds) != 0) {
        double t0 = now_secs();
        run_inproc(sel[idx], r);
        r->secs = now_secs() - t0;
        return 0;
    }
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        double t0 = now_secs();
        run_inproc(sel[idx], r);
        r->secs = now_secs() - t0;
        return 0;
    }
    if (pid == 0) {
        close(fds[0]);
        wire_result_t w;
        capture_wire(&w, run_one(sel[idx]));
        write_full(fds[1], &w, sizeof w);
        fflush(stdout);
        fflush(stderr);
        _exit(0);
    }
    close(fds[1]);
    slot->pid = pid;
    slot->fd = fds[0];
    slot->idx = idx;
    slot->t0 = now_secs();
    return 1;
}

/* Run the n selected tests with up to `jobs` forked children in flight at once,
 * filling results[] in registration order (output is rendered afterward, so it
 * stays deterministic regardless of completion order). Each child enforces its
 * own per-test alarm()/signal isolation exactly as the serial path does. */
static void run_parallel(asmtest_case_t **sel, test_result_t *results, int n,
                         int jobs) {
    if (jobs < 1)
        jobs = 1;
    if (jobs > n)
        jobs = n;
    job_slot_t *slots = (job_slot_t *)asmtest_xalloc(
        malloc((size_t)jobs * sizeof *slots), "job slots");
    struct pollfd *pfds = (struct pollfd *)asmtest_xalloc(
        malloc((size_t)jobs * sizeof *pfds), "poll fds");
    for (int j = 0; j < jobs; j++)
        slots[j].fd = -1;

    int next = 0; /* next test to launch */
    int done = 0; /* tests finalized so far */
    /* poll() is only an efficiency layer over reading each child's pipe in
     * slot order; a non-EINTR failure (e.g. ENOMEM under memory pressure)
     * degrades to blocking reaps instead of abandoning the run with tests
     * still unspawned — which main's pre-zeroed results[] plus ST_PASS == 0
     * would otherwise report as false-green passes with NULL suite/name. */
    int poll_ok = 1;
    /* Internal test hook (not documented in --help): forces one simulated
     * poll() failure so tests/expect.sh can exercise the degrade path. */
    int inject = getenv("ASMTEST_DEBUG_FAIL_POLL") != NULL;

    while (done < n) {
        /* Fill every free slot with a pending test. */
        for (int j = 0; j < jobs && next < n; j++) {
            if (slots[j].fd != -1)
                continue;
            if (!spawn_job(&slots[j], sel, results, next++))
                done++; /* handled in-process; slot stays free */
        }

        /* If nothing is in flight, the rest were all handled in-process. */
        int nready = 0;
        for (int j = 0; j < jobs; j++) {
            if (slots[j].fd < 0)
                continue;
            pfds[nready].fd = slots[j].fd;
            pfds[nready].events = POLLIN;
            pfds[nready].revents = 0;
            nready++;
        }
        if (nready == 0)
            continue;

        if (poll_ok) {
            int rc;
            if (inject) {
                rc = -1;
                errno = ENOMEM;
                inject = 0;
            } else {
                rc = poll(pfds, (nfds_t)nready, -1);
            }
            if (rc < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr,
                        "asmtest: poll failed (%s); falling back to blocking "
                        "reaps\n",
                        strerror(errno));
                poll_ok = 0;
            }
        }

        /* Reap any slot whose child has data or hung up. When poll_ok == 0,
         * poll() itself is unusable this run: treat every in-flight slot as
         * ready and let read_full block per child until it writes or dies —
         * still correct, just serialized instead of parallel-drained. */
        for (int j = 0; j < jobs; j++) {
            if (slots[j].fd < 0)
                continue;
            if (poll_ok) {
                short re = 0;
                for (int k = 0; k < nready; k++)
                    if (pfds[k].fd == slots[j].fd)
                        re = pfds[k].revents;
                if (!(re & (POLLIN | POLLHUP | POLLERR)))
                    continue;
            }

            wire_result_t w;
            size_t got = read_full(slots[j].fd, &w, sizeof w);
            close(slots[j].fd);
            int status = 0;
            while (waitpid(slots[j].pid, &status, 0) < 0 && errno == EINTR)
                ;
            test_result_t *r = &results[slots[j].idx];
            reap_child(r, got, &w, status);
            r->secs = now_secs() - slots[j].t0;
            slots[j].fd = -1;
            done++;
        }
    }

    free(pfds);
    free(slots);
}
#endif /* !_WIN32 (fork isolation + parallel pool) */

/* ANSI color escapes (empty strings when not writing to a TTY). */
typedef struct {
    const char *grn, *red, *yel, *dim, *rst;
} tap_palette_t;

/* Emit one TAP line (plus a YAML block for failures) for result `r`, numbered
 * `num`. Shared by the serial and parallel paths so output is byte-identical. */
static void print_tap_result(int num, const test_result_t *r,
                             const tap_palette_t *p) {
    if (r->outcome == ST_PASS) {
        printf("%sok%s %d - %s.%s\n", p->grn, p->rst, num, r->suite, r->name);
    } else if (r->outcome == ST_SKIP) {
        printf("%sok%s %d - %s.%s %s# SKIP %s%s\n", p->yel, p->rst, num,
               r->suite, r->name, p->dim, r->msg, p->rst);
    } else {
        printf("%snot ok%s %d - %s.%s\n", p->red, p->rst, num, r->suite,
               r->name);
        printf("  %s---%s\n", p->dim, p->rst);
        printf("  at:  %s:%d\n", r->file, r->line);
        /* Block scalar (`|`): assertion messages routinely contain `): ` and
         * other glyphs that are illegal in a YAML plain scalar (PyYAML rejects
         * even the single-line case). Emit each line indented under the key. */
        if (!r->msg[0]) {
            printf("  msg: \"\"\n");
        } else {
            printf("  msg: |\n");
            for (const char *s = r->msg; *s;) {
                const char *nl = strchr(s, '\n');
                int len = nl ? (int)(nl - s) : (int)strlen(s);
                printf("    %.*s\n", len, s);
                if (!nl)
                    break;
                s = nl + 1;
            }
        }
        printf("  %s...%s\n", p->dim, p->rst);
    }
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *filter; /* fnmatch glob; NULL = all */
    int do_list;
    int shuffle;
    int has_seed;
    uint64_t seed;
    int format_junit;
    int fork_tests;  /* per-test fork isolation (default on) */
    int jobs;        /* concurrent forked children; 1 = serial (default) */
    int timeout;     /* seconds; <0 = leave the default in place */
    int bench;       /* run benchmarks instead of tests */
    long bench_reps; /* fixed inner reps; 0 = auto-calibrate */
    int bench_json;  /* --bench-format=json: machine-readable bench output */
    int fail_if_no_tests; /* exit nonzero when the selection is empty */
    int color;            /* 0 auto, 1 always, 2 never (--color=) */
    int fail_fast;        /* stop at the first failing test (forces serial) */
    long repeat;          /* run the selection N times (default 1) */
    int shard_k, shard_n; /* --shard=K/N slice; shard_n = 0 disables */
} options_t;

enum { COLOR_AUTO = 0, COLOR_ALWAYS = 1, COLOR_NEVER = 2 };

/* Whether to emit ANSI color. --color=always/never force it; the default (auto)
 * colors only an interactive stdout with NO_COLOR unset — honoring the
 * https://no-color.org convention (any non-empty NO_COLOR disables color), which
 * `isatty`-only detection ignored. */
static int want_color(const options_t *opt) {
    if (opt->color == COLOR_ALWAYS)
        return 1;
    if (opt->color == COLOR_NEVER)
        return 0;
    const char *nc = getenv("NO_COLOR");
    if (nc != NULL && nc[0] != '\0')
        return 0;
    return ASMTEST_ISATTY();
}

static void usage(const char *prog) {
    printf(
        "usage: %s [options]\n"
        "  --filter=GLOB        run only tests whose suite, name, or\n"
        "                       \"suite.name\" matches GLOB (shell wildcards)\n"
        "  --list               list the matching tests and exit\n"
        "  --shuffle            run tests in a random order\n"
        "  --seed=N             seed the shuffle (implies --shuffle; "
        "reproducible)\n"
        "  --timeout=SEC        per-test timeout in seconds (0 disables; "
        "default 10)\n"
        "  --no-fork            run tests in-process (no per-test isolation)\n"
        "  -jN, --jobs=N        run up to N tests concurrently (implies fork;\n"
        "                       default 1 = serial). Output stays in order.\n"
        "  --format=tap|junit   output format (default tap)\n"
        "  --color=auto|always|never  colorize (default auto; honors "
        "NO_COLOR)\n"
        "  --fail-if-no-tests   exit nonzero if the selection is empty (e.g. a "
        "typo'd --filter)\n"
        "  --fail-fast          stop at the first failing test (forces "
        "serial;\n"
        "                       TAP plan moves to the end of the stream)\n"
        "  --repeat=N           run the selection N times (flake hunting; "
        "pairs\n"
        "                       with --shuffle/--seed)\n"
        "  --shard=K/N          run the K-th of N round-robin slices of the\n"
        "                       selection (1-based; split a suite across CI "
        "jobs)\n"
        "  --bench              run registered benchmarks (BENCH) instead of "
        "tests\n"
        "  --bench-reps=N       fixed inner reps per round (default: "
        "auto-calibrate)\n"
        "  --bench-format=text|json  benchmark output (default text; json for "
        "CI ingestion)\n"
        "  -h, --help           show this help\n",
        prog);
}

/* If `a` begins with `pre`, point *rest at the remainder and return 1. */
static int opt_prefix(const char *a, const char *pre, const char **rest) {
    size_t n = strlen(pre);
    if (strncmp(a, pre, n) == 0) {
        *rest = a + n;
        return 1;
    }
    return 0;
}

/* A suite/name pair matches the filter if the glob matches the suite, the
 * name, or the "suite.name" id. */
static int id_matches(const char *suite, const char *name, const char *glob) {
    char id[256];
    snprintf(id, sizeof id, "%s.%s", suite, name);
    return ASMTEST_FNMATCH(glob, id) == 0 ||
           ASMTEST_FNMATCH(glob, suite) == 0 ||
           ASMTEST_FNMATCH(glob, name) == 0;
}

static int test_matches(const asmtest_case_t *tc, const char *glob) {
    return id_matches(tc->suite, tc->name, glob);
}

static void xml_print_escaped(const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '&':
            fputs("&amp;", stdout);
            break;
        case '<':
            fputs("&lt;", stdout);
            break;
        case '>':
            fputs("&gt;", stdout);
            break;
        case '"':
            fputs("&quot;", stdout);
            break;
        case '\n':
            fputs("&#10;", stdout);
            break;
        case '\r':
            fputs("&#13;", stdout);
            break;
        case '\t':
            fputc('\t', stdout);
            break;
        default:
            /* C0 control bytes other than tab/newline/CR are illegal in XML 1.0
             * even when written as entities, so a stray byte in test data would
             * make the whole report unparseable. Emit a visible \xNN escape. */
            if (c < 0x20)
                printf("\\x%02X", c);
            else
                fputc(c, stdout);
        }
    }
}

/* JUnit distinguishes an <error> (a crash, timeout, or abnormal exit — something
 * unexpected went wrong) from a <failure> (a failed assertion). asmtest records
 * both as ST_FAIL, but the crash/timeout paths — the signal/alarm handler and
 * the fork reaper — set one of these message prefixes, which an assertion never
 * does, so the outcome classifies with no extra per-test plumbing. */
static int result_is_error(const test_result_t *r) {
    if (r->outcome != ST_FAIL)
        return 0;
    static const char *const pfx[] = {"caught fatal signal", "timed out",
                                      "crashed:", "child exited abnormally",
                                      "no result reported"};
    for (size_t i = 0; i < sizeof pfx / sizeof pfx[0]; i++)
        if (strncmp(r->msg, pfx[i], strlen(pfx[i])) == 0)
            return 1;
    return 0;
}

/* Render results as JUnit XML, grouped into <testsuite> by suite name. sysout is
 * the test bodies' captured stdout (NULL if none / unavailable), surfaced as a
 * <system-out> so a CI viewer keeps it instead of the old /dev/null discard. */
static void render_junit(const test_result_t *results, int n,
                         const char *sysout) {
    int failures = 0, errors = 0, skipped = 0;
    double total_secs = 0.0;
    for (int i = 0; i < n; i++) {
        total_secs += results[i].secs;
        if (result_is_error(&results[i]))
            errors++;
        else if (results[i].outcome == ST_FAIL)
            failures++;
        else if (results[i].outcome == ST_SKIP)
            skipped++;
    }
    printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    printf("<testsuites tests=\"%d\" failures=\"%d\" errors=\"%d\" "
           "skipped=\"%d\" time=\"%.6f\">\n",
           n, failures, errors, skipped, total_secs);

    int *done = (int *)asmtest_xalloc(
        calloc((size_t)(n > 0 ? n : 1), sizeof(int)), "junit suite map");
    int first_suite = 1;
    for (int i = 0; i < n; i++) {
        if (done[i])
            continue;
        const char *suite = results[i].suite;
        int s_tests = 0, s_fail = 0, s_err = 0, s_skip = 0;
        double s_secs = 0.0;
        for (int j = i; j < n; j++) {
            if (strcmp(results[j].suite, suite) != 0)
                continue;
            s_tests++;
            s_secs += results[j].secs;
            if (result_is_error(&results[j]))
                s_err++;
            else if (results[j].outcome == ST_FAIL)
                s_fail++;
            else if (results[j].outcome == ST_SKIP)
                s_skip++;
        }
        printf("  <testsuite name=\"");
        xml_print_escaped(suite);
        printf("\" tests=\"%d\" failures=\"%d\" errors=\"%d\" skipped=\"%d\" "
               "time=\"%.6f\">\n",
               s_tests, s_fail, s_err, s_skip, s_secs);
        for (int j = i; j < n; j++) {
            if (strcmp(results[j].suite, suite) != 0)
                continue;
            done[j] = 1;
            const test_result_t *r = &results[j];
            printf("    <testcase classname=\"");
            xml_print_escaped(suite);
            printf("\" name=\"");
            xml_print_escaped(r->name);
            printf("\" time=\"%.6f\">", r->secs);
            if (r->outcome == ST_FAIL) {
                /* crash/timeout -> <error>, assertion -> <failure> */
                const char *tag = result_is_error(r) ? "error" : "failure";
                printf("\n      <%s message=\"", tag);
                xml_print_escaped(r->msg);
                printf("\">at ");
                xml_print_escaped(
                    r->file); /* a path with &/< must be escaped */
                printf(":%d&#10;", r->line);
                xml_print_escaped(r->msg);
                printf("</%s>\n    ", tag);
            } else if (r->outcome == ST_SKIP) {
                printf("\n      <skipped message=\"");
                xml_print_escaped(r->msg);
                printf("\"/>\n    ");
            }
            printf("</testcase>\n");
        }
        /* The runner captures stdout at the run level (tests are interleaved, not
         * grouped by suite), so attach the whole-run capture to the first
         * <testsuite> — schema-valid there, and no CI output is discarded. */
        if (first_suite && sysout != NULL && sysout[0] != '\0') {
            printf("    <system-out>");
            xml_print_escaped(sysout);
            printf("</system-out>\n");
        }
        first_suite = 0;
        printf("  </testsuite>\n");
    }
    printf("</testsuites>\n");
    free(done);
}

#if defined(_WIN32)
static double now_secs(void) {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart / (double)freq.QuadPart;
}
#else
static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

/* ------------------------------------------------------------------ */
/* Benchmark mode (Phase 9)                                            */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)
#define ASMTEST_BENCH_UNIT "cyc" /* rdtsc reference cycles */
#else
/* cntvct_el0 virtual-timer ticks (AArch64) or rdtime CSR ticks (rv64) — both
 * coarser than a core cycle, so the unit is "ticks", not "cyc". */
#define ASMTEST_BENCH_UNIT "ticks"
#endif

/* Calibration/measurement constants. Reps are auto-grown until one round spans
 * at least BENCH_TARGET ticks (so the counter's resolution doesn't dominate),
 * capped at BENCH_REPS_CAP; then BENCH_ROUNDS rounds are measured. */
enum {
    BENCH_TARGET = 50000,     /* per-round counter delta to calibrate toward */
    BENCH_REPS_CAP = 5000000, /* never loop more than this per round         */
    BENCH_ROUNDS = 11         /* measured rounds (odd, for a clean median)   */
};

typedef struct {
    double min, median, mean, stddev; /* stddev: round-to-round dispersion */
    long reps;
} bench_stats_t;

/* Portable across the POSIX tiers and Win64: the timing loop is just the cycle
 * counter (rdtsc / cntvct_el0) around an indirect call. A crashing BENCH body is
 * guarded by the installed handlers on POSIX; on Win64 a BENCH is trusted (no
 * per-bench process isolation), like the routine being measured. */

/* Time `reps` back-to-back calls of body; returns the counter delta. The
 * indirect call cannot be elided, so the loop is not optimized away. */
static uint64_t bench_time_reps(void (*body)(void), long reps) {
    uint64_t t0 = asmtest_cycle_counter();
    for (long i = 0; i < reps; i++)
        body();
    uint64_t t1 = asmtest_cycle_counter();
    return t1 - t0;
}

/* Grow reps (doubling) until a round spans BENCH_TARGET ticks or hits the cap. */
static long bench_calibrate(void (*body)(void)) {
    long reps = 1;
    while (reps < BENCH_REPS_CAP) {
        if (bench_time_reps(body, reps) >= (uint64_t)BENCH_TARGET)
            break;
        reps *= 2;
    }
    return reps > BENCH_REPS_CAP ? BENCH_REPS_CAP : reps;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* Measure `reps` calls per round across BENCH_ROUNDS rounds (plus one discarded
 * warmup), reducing to min/median/mean cycles per call. */
static void bench_measure(void (*body)(void), long reps, bench_stats_t *st) {
    double per[BENCH_ROUNDS];
    body(); /* warmup: prime caches/branch predictors (and fault early) */
    for (int r = 0; r < BENCH_ROUNDS; r++)
        per[r] = (double)bench_time_reps(body, reps) / (double)reps;
    qsort(per, BENCH_ROUNDS, sizeof per[0], cmp_double);
    double sum = 0;
    for (int r = 0; r < BENCH_ROUNDS; r++)
        sum += per[r];
    st->min = per[0];
    st->median = per[BENCH_ROUNDS / 2];
    st->mean = sum / BENCH_ROUNDS;
    double var = 0;
    for (int r = 0; r < BENCH_ROUNDS; r++) {
        double d = per[r] - st->mean;
        var += d * d;
    }
    var /= BENCH_ROUNDS; /* population variance over the rounds */
    /* Newton's method for the sd, so the runner needs no libm (-lm) link — the
     * test binaries don't link it, and mingw Win64 keeps it self-contained. */
    double g = var;
    if (g > 0.0)
        for (int i = 0; i < 40; i++)
            g = 0.5 * (g + var / g);
    st->stddev = var > 0.0 ? g : 0.0;
    st->reps = reps;
}

/* Print s as a JSON string literal (quotes + minimal escapes). */
static void json_print_string(const char *s) {
    putchar('"');
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            fputs("\\\"", stdout);
            break;
        case '\\':
            fputs("\\\\", stdout);
            break;
        case '\n':
            fputs("\\n", stdout);
            break;
        case '\r':
            fputs("\\r", stdout);
            break;
        case '\t':
            fputs("\\t", stdout);
            break;
        default:
            if (c < 0x20)
                printf("\\u%04x", c);
            else
                putchar((int)c);
        }
    }
    putchar('"');
}

/* Run the selected benchmarks. Each runs in-process under the same signal/timeout
 * guard as a test, so a crashing or hung routine is reported as an error rather
 * than taking the process down. forced_reps > 0 overrides auto-calibration.
 * format 0 = the aligned human table (min/median/mean + sd + cv%); format 1 =
 * a machine-readable JSON object for CI ingestion / baseline diffing. */
static int run_benchmarks(asmtest_bench_t **sel, int n, long forced_reps,
                          int use_color, int json) {
    const char *dim = use_color ? "\033[2m" : "";
    const char *red = use_color ? "\033[31m" : "";
    const char *rst = use_color ? "\033[0m" : "";
    int errors = 0; /* benches that crashed/timed out — the run must fail */

    int width = 1;
    for (int i = 0; i < n; i++) {
        int w = (int)(strlen(sel[i]->suite) + strlen(sel[i]->name) + 1);
        if (w > width)
            width = w;
    }

    if (json)
        printf("{\"unit\": \"%s\", \"rounds\": %d, \"benchmarks\": [",
               ASMTEST_BENCH_UNIT, BENCH_ROUNDS);
    else
        printf("%s# benchmarks — %s per call, min/median/mean +/- sd over %d "
               "rounds%s\n",
               dim, ASMTEST_BENCH_UNIT, BENCH_ROUNDS, rst);

    int emitted = 0; /* JSON array comma tracking */
    for (int i = 0; i < n; i++) {
        asmtest_bench_t *b = sel[i];
        char id[256];
        snprintf(id, sizeof id, "%s.%s", b->suite, b->name);

        asmtest_in_test = 1;
#if !defined(_WIN32)
        /* Contain a crashing/hung BENCH the same way a test is (POSIX). On Win64
         * a BENCH runs unguarded — it is trusted, like the routine it measures. */
        if (asmtest_timeout_secs > 0)
            alarm((unsigned)asmtest_timeout_secs);
        if (sigsetjmp(asmtest_jmp, 1) != 0) {
            alarm(0);
            asmtest_in_test = 0;
            errors++;
            if (json) {
                printf("%s\n    {\"name\": ", emitted++ ? "," : "");
                json_print_string(id);
                printf(", \"error\": ");
                json_print_string(asmtest_msg);
                printf("}");
            } else {
                printf("  %-*s  %sERROR: %s%s\n", width, id, red, asmtest_msg,
                       rst);
            }
            continue;
        }
#endif
        long reps = forced_reps > 0 ? forced_reps : bench_calibrate(b->fn);
        bench_stats_t st;
        bench_measure(b->fn, reps, &st);
#if !defined(_WIN32)
        alarm(0);
#endif
        asmtest_in_test = 0;

        double cv =
            st.mean != 0.0 ? st.stddev / st.mean : 0.0; /* rel. spread */
        if (json) {
            printf("%s\n    {\"name\": ", emitted++ ? "," : "");
            json_print_string(id);
            printf(", \"min\": %.4f, \"median\": %.4f, \"mean\": %.4f, "
                   "\"stddev\": %.4f, \"cv\": %.5f, \"reps\": %ld}",
                   st.min, st.median, st.mean, st.stddev, cv, st.reps);
        } else {
            printf("  %-*s  min=%9.2f  median=%9.2f  mean=%9.2f  sd=%8.2f  "
                   "cv=%5.1f%%  %s(reps=%ld)%s\n",
                   width, id, st.min, st.median, st.mean, st.stddev, 100.0 * cv,
                   dim, st.reps, rst);
        }
    }
    if (json)
        printf("\n  ]\n}\n");
    return errors;
}

/* The framework provides main(). Define ASMTEST_NO_MAIN to omit it and supply
 * your own (embedding the runtime, or — as the conformance corpus does — reusing
 * only the capture/verdict/emulator entry points with a separate driver). */
#ifndef ASMTEST_NO_MAIN
int main(int argc, char **argv) {
    options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.fork_tests = 1;
    opt.jobs = 1;
    opt.timeout = -1;
    opt.repeat = 1;
    /* Win64 isolates per test by re-exec (Track B), matching POSIX's fork
     * default; --no-fork opts into the in-process facility (vectored handler +
     * watchdog). Both contain a crash/hang; isolation adds the process-death
     * backstop for faults the in-process guard can't unwind. */

    for (int ai = 1; ai < argc; ai++) {
        const char *a = argv[ai];
        const char *v;
        if (strcmp(a, "--list") == 0) {
            opt.do_list = 1;
        } else if (strcmp(a, "--shuffle") == 0) {
            opt.shuffle = 1;
        } else if (strcmp(a, "--no-fork") == 0) {
            opt.fork_tests = 0;
#if defined(_WIN32)
        } else if (opt_prefix(a, "--asmtest-child=", &v)) {
            asmtest_child_index =
                atoi(v); /* internal: re-exec child selector */
        } else if (opt_prefix(a, "--asmtest-child-out=", &v)) {
            asmtest_child_out = v;
#endif
        } else if (opt_prefix(a, "--jobs=", &v) || opt_prefix(a, "-j", &v)) {
            if (*v == '\0' && ai + 1 < argc) /* "-j N" / "--jobs N" form */
                v = argv[++ai];
            int j = atoi(v);
            if (j < 1) {
                fprintf(stderr, "invalid --jobs: %s\n", v);
                return 2;
            }
            opt.jobs = j;
        } else if (strcmp(a, "--jobs") == 0 && ai + 1 < argc) {
            int j = atoi(argv[++ai]);
            if (j < 1) {
                fprintf(stderr, "invalid --jobs: %s\n", argv[ai]);
                return 2;
            }
            opt.jobs = j;
        } else if (strcmp(a, "--bench") == 0) {
            opt.bench = 1;
        } else if (opt_prefix(a, "--bench-reps=", &v)) {
            opt.bench = 1;
            opt.bench_reps = strtol(v, NULL, 0);
        } else if (opt_prefix(a, "--bench-format=", &v)) {
            opt.bench = 1;
            if (strcmp(v, "json") == 0)
                opt.bench_json = 1;
            else if (strcmp(v, "text") == 0)
                opt.bench_json = 0;
            else {
                fprintf(stderr, "unknown --bench-format: %s\n", v);
                return 2;
            }
        } else if (opt_prefix(a, "--color=", &v)) {
            if (strcmp(v, "auto") == 0)
                opt.color = COLOR_AUTO;
            else if (strcmp(v, "always") == 0)
                opt.color = COLOR_ALWAYS;
            else if (strcmp(v, "never") == 0)
                opt.color = COLOR_NEVER;
            else {
                fprintf(stderr, "unknown --color: %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (opt_prefix(a, "--filter=", &v)) {
            opt.filter = v;
        } else if (strcmp(a, "--filter") == 0 && ai + 1 < argc) {
            opt.filter = argv[++ai];
        } else if (opt_prefix(a, "--seed=", &v)) {
            opt.seed = (uint64_t)strtoull(v, NULL, 0);
            opt.has_seed = opt.shuffle = 1;
        } else if (strcmp(a, "--seed") == 0 && ai + 1 < argc) {
            opt.seed = (uint64_t)strtoull(argv[++ai], NULL, 0);
            opt.has_seed = opt.shuffle = 1;
        } else if (opt_prefix(a, "--timeout=", &v)) {
            opt.timeout = atoi(v);
        } else if (strcmp(a, "--timeout") == 0 && ai + 1 < argc) {
            opt.timeout = atoi(argv[++ai]);
        } else if (opt_prefix(a, "--format=", &v)) {
            if (strcmp(v, "junit") == 0)
                opt.format_junit = 1;
            else if (strcmp(v, "tap") == 0)
                opt.format_junit = 0;
            else {
                fprintf(stderr, "unknown --format: %s\n", v);
                return 2;
            }
        } else if (strcmp(a, "--fail-if-no-tests") == 0) {
            opt.fail_if_no_tests = 1;
        } else if (strcmp(a, "--fail-fast") == 0) {
            opt.fail_fast = 1;
        } else if (opt_prefix(a, "--repeat=", &v)) {
            opt.repeat = strtol(v, NULL, 0);
            if (opt.repeat < 1) {
                fprintf(stderr, "invalid --repeat: %s\n", v);
                return 2;
            }
        } else if (opt_prefix(a, "--shard=", &v)) {
            char *slash = NULL;
            long k = strtol(v, &slash, 10);
            long ns = (slash != NULL && *slash == '/')
                          ? strtol(slash + 1, NULL, 10)
                          : 0;
            if (k < 1 || ns < 1 || k > ns || ns > 1000000) {
                fprintf(stderr, "invalid --shard (want K/N, 1<=K<=N): %s\n", v);
                return 2;
            }
            opt.shard_k = (int)k;
            opt.shard_n = (int)ns;
        } else {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    /* Timeout precedence: --timeout, else ASMTEST_TIMEOUT, else the default. */
    if (opt.timeout >= 0) {
        asmtest_timeout_secs = opt.timeout;
    } else {
        const char *e = getenv("ASMTEST_TIMEOUT");
        if (e && *e)
            asmtest_timeout_secs = atoi(e);
    }

    install_handlers();

#if defined(_WIN32)
    /* Re-exec child (Track B): run exactly one test by global index, write its
     * wire record for the parent, and exit. The verdict travels in the file, so
     * exit 0 — the parent reads pass/fail/skip there (a crash/hang is detected by
     * the parent from the process death / deadline). */
    if (asmtest_child_index >= 0) {
        asmtest_case_t *tc = asmtest_case_at(asmtest_child_index);
        wire_result_t w;
        if (tc != NULL) {
            capture_wire(&w, run_one(tc));
        } else {
            memset(&w, 0, sizeof w);
            w.outcome = ST_FAIL;
            snprintf(w.msg, sizeof w.msg, "bad child index %d",
                     asmtest_child_index);
        }
        if (asmtest_child_out != NULL) {
            FILE *f = fopen(asmtest_child_out, "wb");
            if (f != NULL) {
                fwrite(&w, 1, sizeof w, f);
                fclose(f);
            }
        }
        return 0;
    }
#endif

    /* Benchmark mode: collect, optionally list, then time the BENCH cases. */
    if (opt.bench) {
        int btotal = 0;
        for (asmtest_bench_t *b = asmtest_bench_head; b != NULL; b = b->next)
            btotal++;
        asmtest_bench_t **bsel = (asmtest_bench_t **)asmtest_xalloc(
            malloc((size_t)(btotal > 0 ? btotal : 1) * sizeof *bsel),
            "benchmark selection");
        int bn = 0;
        for (asmtest_bench_t *b = asmtest_bench_head; b != NULL; b = b->next) {
            if (opt.filter == NULL || id_matches(b->suite, b->name, opt.filter))
                bsel[bn++] = b;
        }
        int berr = 0;
        if (opt.do_list) {
            for (int i = 0; i < bn; i++)
                printf("%s.%s\n", bsel[i]->suite, bsel[i]->name);
        } else {
            berr = run_benchmarks(bsel, bn, opt.bench_reps,
                                  want_color(&opt) && !opt.bench_json,
                                  opt.bench_json);
        }
        free(bsel);
        /* A benchmark that crashed or timed out must fail the run — `make bench`
         * gates CI, so a silent exit 0 would let a broken BENCH pass. */
        return berr > 0 ? 1 : 0;
    }

    /* Collect the registered tests that pass the filter into an array. */
    int total = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next)
        total++;
    asmtest_case_t **sel = (asmtest_case_t **)asmtest_xalloc(
        malloc((size_t)(total > 0 ? total : 1) * sizeof *sel),
        "test selection");
    /* Global registration index of each selected test, kept in lockstep with
     * sel[] (through the shuffle) so the Win64 re-exec child can select the same
     * test by index. Built everywhere; only read on the _WIN32 isolation path. */
    int *gidx = (int *)asmtest_xalloc(
        malloc((size_t)(total > 0 ? total : 1) * sizeof *gidx),
        "registration index");
    int n = 0, gi = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next, gi++) {
        if (opt.filter == NULL || test_matches(tc, opt.filter)) {
            gidx[n] = gi;
            sel[n++] = tc;
        }
    }

    /* --shard=K/N: keep the K-th of N round-robin slices of the filtered,
     * registration-ordered selection. Round-robin (index mod N) is deterministic
     * and balances suites across shards, so N CI jobs splitting a run neither
     * lose nor duplicate a test. Applied before --list (so a shard can be
     * inspected) and before --repeat/--shuffle (so all shards partition the same
     * base selection). */
    if (opt.shard_n > 1) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (i % opt.shard_n == opt.shard_k - 1) {
                sel[m] = sel[i];
                gidx[m] = gidx[i];
                m++;
            }
        }
        n = m;
    }

    if (opt.do_list) {
        for (int i = 0; i < n; i++)
            printf("%s.%s\n", sel[i]->suite, sel[i]->name);
        free(sel);
        free(gidx);
        return 0;
    }

    /* --repeat=N: block-replicate the selection N times (with --shuffle the
     * rounds interleave) — pairs with --shuffle/--seed for flake hunting. */
    if (opt.repeat > 1 && n > 0) {
        if ((long)n > (long)INT_MAX / opt.repeat) {
            fprintf(stderr, "--repeat=%ld overflows the selection\n",
                    opt.repeat);
            return 2;
        }
        size_t reps = (size_t)opt.repeat;
        sel = (asmtest_case_t **)asmtest_xalloc(
            realloc(sel, (size_t)n * reps * sizeof *sel), "repeat selection");
        gidx = (int *)asmtest_xalloc(
            realloc(gidx, (size_t)n * reps * sizeof *gidx), "repeat index");
        for (size_t r = 1; r < reps; r++) {
            memcpy(sel + r * (size_t)n, sel, (size_t)n * sizeof *sel);
            memcpy(gidx + r * (size_t)n, gidx, (size_t)n * sizeof *gidx);
        }
        n = (int)((size_t)n * reps);
    }

    uint64_t shuffle_seed = 0;
    int did_shuffle = 0;
    if (opt.shuffle) {
        /* --seed=N wins; else fall back to ASMTEST_SEED (decimal or 0x-hex) so a
         * shuffled order is reproducible via the env var; else time^pid. */
        uint64_t seed;
        const char *seed_env = getenv("ASMTEST_SEED");
        if (opt.has_seed)
            seed = opt.seed;
        else if (seed_env && *seed_env)
            seed = (uint64_t)strtoull(seed_env, NULL, 0);
        else
            seed = (uint64_t)time(NULL) ^ ((uint64_t)ASMTEST_GETPID() << 32);
        asmtest_rng_t rng = {seed};
        for (int i = n - 1; i > 0;
             i--) { /* Fisher-Yates (sel + gidx together) */
            int j = (int)(asmtest_rng_u64(&rng) % (uint64_t)(i + 1));
            asmtest_case_t *t = sel[i];
            sel[i] = sel[j];
            sel[j] = t;
            int g = gidx[i];
            gidx[i] = gidx[j];
            gidx[j] = g;
        }
        shuffle_seed = seed;
        did_shuffle = 1;
    }

    int use_color = !opt.format_junit && want_color(&opt);
    tap_palette_t pal = {
        .grn = use_color ? "\033[32m" : "",
        .red = use_color ? "\033[31m" : "",
        .yel = use_color ? "\033[33m" : "",
        .dim = use_color ? "\033[2m" : "",
        .rst = use_color ? "\033[0m" : "",
    };

    if (!opt.format_junit) {
        /* TAP 13: the version line must be the first line of output, so the
         * shuffle-seed diagnostic goes *after* the plan, not before the version.
         * With --fail-fast the number of tests that will run is unknown up
         * front, so the plan moves to the end of the stream (equally valid
         * TAP), covering exactly what ran. */
        printf("TAP version 13\n");
        if (!opt.fail_fast)
            printf("1..%d\n", n);
        if (did_shuffle)
            printf("# shuffle seed=0x%llx\n", (unsigned long long)shuffle_seed);
    }

    /* An empty selection (typically a typo'd --filter) otherwise runs nothing
     * and exits 0, so a CI job that filters can green-light having tested
     * nothing. Warn to stderr always; `1..0` stays valid TAP. --fail-if-no-tests
     * opts into a nonzero exit (pytest exits 5 here; gtest warns). */
    if (n == 0) {
        if (opt.filter)
            fprintf(stderr, "asmtest: no tests matched --filter='%s'\n",
                    opt.filter);
        else
            fprintf(stderr, "asmtest: no tests to run\n");
    }

    test_result_t *results = (test_result_t *)asmtest_xalloc(
        malloc((size_t)(n > 0 ? n : 1) * sizeof *results), "results buffer");
    int passed = 0, failed = 0, skipped = 0;
    int ran = n; /* < n only when --fail-fast stops the loop early */

    /* JUnit's `<?xml?>` declaration must be the first bytes of the document, but
     * forked/in-process test bodies inherit stdout and may printf to it. Point
     * the real stdout fd at /dev/null for the run phase (children inherit it),
     * then restore it before rendering the XML. */
    int junit_saved_out = -1;
    FILE *junit_out = NULL; /* captures test bodies' stdout for <system-out> */
#if !defined(_WIN32)
    if (opt.format_junit) {
        fflush(stdout);
        junit_saved_out = dup(STDOUT_FILENO);
        junit_out = tmpfile(); /* auto-deleted on close */
        if (junit_saved_out >= 0 && junit_out != NULL) {
            dup2(fileno(junit_out), STDOUT_FILENO);
        } else if (junit_saved_out >= 0) {
            int devnull = open("/dev/null", O_WRONLY); /* fallback: discard */
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                close(devnull);
            }
        }
    }
#endif

    /* Parallel needs fork isolation to contain children; --no-fork forces
     * serial. Run concurrently when -jN>1, otherwise run (and stream) serially.
     * In both cases results[] is filled in registration order; parallel renders
     * after the run so output stays deterministic regardless of finish order. */
    /* --fail-fast wants to stop dispatching at the first failure, so it forces
     * the serial path (a parallel batch would keep running past it). */
    int parallel = opt.fork_tests && opt.jobs > 1 && n > 1 && !opt.fail_fast;
    if (parallel) {
        for (int i = 0; i < n; i++)
            memset(&results[i], 0, sizeof results[i]);
#if defined(_WIN32)
        if (run_parallel_win32(sel, gidx, results, n, opt.jobs) != 0)
            parallel = 0; /* pool unavailable: fall back to the serial path */
#else
        run_parallel(sel, results, n, opt.jobs);
#endif
    }

    for (int i = 0; i < n; i++) {
        test_result_t *r = &results[i];

        if (!parallel) {
            asmtest_case_t *tc = sel[i];
            memset(r, 0, sizeof *r);
            r->suite = tc->suite;
            r->name = tc->name;
            double t0 = now_secs();
#if defined(_WIN32)
            if (opt.fork_tests)
                run_isolated_win32(gidx[i], r); /* re-exec per test */
            else
#else
            if (opt.fork_tests)
                run_forked(tc, r);
            else
#endif
                run_inproc(tc, r);
            r->secs = now_secs() - t0;
        }

        if (r->outcome == ST_PASS)
            passed++;
        else if (r->outcome == ST_SKIP)
            skipped++;
        else
            failed++;

        if (!opt.format_junit) /* JUnit is rendered in one pass at the end */
            print_tap_result(i + 1, r, &pal);

        if (opt.fail_fast && r->outcome == ST_FAIL) {
            ran = i + 1; /* results[ran..) never ran; render only [0, ran) */
            break;
        }
    }

    char *junit_sysout = NULL;
#if !defined(_WIN32)
    if (opt.format_junit && junit_saved_out >= 0) {
        fflush(stdout); /* flush any parent buffering before restoring stdout */
        dup2(junit_saved_out, STDOUT_FILENO);
        close(junit_saved_out);
    }
    if (junit_out != NULL) { /* slurp the captured test-body output */
        int fd = fileno(junit_out);
        off_t sz = lseek(fd, 0, SEEK_END);
        if (sz > 0) {
            junit_sysout = (char *)malloc((size_t)sz + 1);
            if (junit_sysout != NULL) {
                lseek(fd, 0, SEEK_SET);
                ssize_t got = read(fd, junit_sysout, (size_t)sz);
                junit_sysout[got > 0 ? (size_t)got : 0] = '\0';
            }
        }
        fclose(junit_out);
    }
#endif

    if (opt.format_junit) {
        render_junit(results, ran, junit_sysout);
    } else {
        if (opt.fail_fast) {
            printf("1..%d\n", ran); /* the trailing plan covers what ran */
            if (ran < n)
                printf("# fail-fast: stopped after %d of %d tests\n", ran, n);
        }
        const char *summary_color = failed ? pal.red : pal.grn;
        printf("%s# %d passed, %d failed, %d skipped, %d total%s\n",
               summary_color, passed, failed, skipped, ran, pal.rst);
    }

    free(results);
    free(sel);
    free(gidx);
    free(junit_sysout);
    return (failed || (n == 0 && opt.fail_if_no_tests)) ? 1 : 0;
}
#endif /* ASMTEST_NO_MAIN */
