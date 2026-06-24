/*
 * asmtest.c — runner, registries, failure/skip/crash model, fixtures,
 * register/flags assertions, guard-page buffers, and main() (Phase 2).
 */
#include "asmtest.h"

#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Linux spells it MAP_ANONYMOUS; macOS/BSD provide MAP_ANON. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* siglongjmp return codes for the per-test jump buffer. */
enum { JMP_FAIL = 1, JMP_SKIP = 2 };

/* Test outcome. */
enum { ST_PASS, ST_FAIL, ST_SKIP };

static asmtest_case_t *asmtest_head = NULL;
static asmtest_case_t *asmtest_tail = NULL;
static asmtest_hook_t *asmtest_hooks = NULL;

sigjmp_buf asmtest_jmp;
static volatile sig_atomic_t asmtest_in_test = 0;

static char asmtest_msg[1024];
static const char *asmtest_loc_file;
static int asmtest_loc_line;

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

/* ------------------------------------------------------------------ */
/* Failure / skip                                                      */
/* ------------------------------------------------------------------ */

void asmtest_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(asmtest_msg, sizeof asmtest_msg, fmt, ap);
    va_end(ap);
    asmtest_loc_file = file;
    asmtest_loc_line = line;
    siglongjmp(asmtest_jmp, JMP_FAIL);
}

void asmtest_skip(const char *reason) {
    snprintf(asmtest_msg, sizeof asmtest_msg, "%s", reason ? reason : "");
    siglongjmp(asmtest_jmp, JMP_SKIP);
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

void asmtest_assert_abi(const char *file, int line, const regs_t *r) {
    const struct {
        const char *name;
        unsigned long got, want;
    } chk[] = {
#if defined(__x86_64__)
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
#endif
    };
    for (size_t i = 0; i < sizeof chk / sizeof chk[0]; i++) {
        if (chk[i].got != chk[i].want)
            asmtest_fail(file, line,
                         "ASSERT_ABI_PRESERVED: %s not restored "
                         "(got 0x%lx, expected 0x%lx)",
                         chk[i].name, chk[i].got, chk[i].want);
    }
}

void asmtest_assert_flag(const char *file, int line, const regs_t *r,
                         unsigned long mask, int want_set, const char *name) {
    int set = (r->flags & mask) != 0;
    if (set != want_set)
        asmtest_fail(file, line, "ASSERT_FLAG_%s(%s): flags=0x%lx",
                     want_set ? "SET" : "CLEAR", name, r->flags);
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
    return ia > ib ? (uint64_t)(ia - ib) : (uint64_t)(ib - ia);
}

/* 32-bit float ULP distance, analogous to fp_ulp_distance. */
static uint32_t fp_ulp_distance_f(float a, float b) {
    int32_t ia, ib;
    memcpy(&ia, &a, sizeof ia);
    memcpy(&ib, &b, sizeof ib);
    if (ia < 0)
        ia = (int32_t)0x80000000U - ia;
    if (ib < 0)
        ib = (int32_t)0x80000000U - ib;
    return ia > ib ? (uint32_t)(ia - ib) : (uint32_t)(ib - ia);
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
        asmtest_fail(file, line, "ASSERT_*NEAR (double): NaN mismatch (%.17g "
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
        asmtest_fail(file, line, "ASSERT_FNEAR (float): NaN mismatch (%.9g vs "
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

/* ------------------------------------------------------------------ */
/* Guard-page buffers                                                  */
/* ------------------------------------------------------------------ */

static size_t round_up_page(size_t n, long pg) {
    size_t usable = ((n + (size_t)pg - 1) / (size_t)pg) * (size_t)pg;
    return usable == 0 ? (size_t)pg : usable;
}

void *asmtest_guarded_alloc(size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t usable = round_up_page(n, pg);
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
    unsigned char *base = (unsigned char *)p - (usable - n);
    munmap(base, usable + (size_t)pg);
}

void *asmtest_guarded_alloc_under(size_t n) {
    long pg = sysconf(_SC_PAGESIZE);
    size_t usable = round_up_page(n, pg);
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
    unsigned char *base = (unsigned char *)p - (size_t)pg;
    munmap(base, (size_t)pg + usable);
}

/* ------------------------------------------------------------------ */
/* Large struct-by-value parameter (arch-divergent dispatch)           */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)
extern void asm_bigstruct_x86(regs_t *out, void *fn, const long *iargs,
                              int niargs, const void *sptr, size_t ssize);
#endif

void asm_call_capture_bigstruct(regs_t *out, void *fn, const long *iargs,
                                int niargs, const void *sptr, size_t ssize) {
#if defined(__aarch64__)
    /* AAPCS64: a >16-byte struct is passed as a pointer to a copy; for a
     * read-only callee, passing the struct's own address is equivalent. */
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

long asmtest_rng_long(asmtest_rng_t *rng) {
    return (long)asmtest_rng_u64(rng);
}

long asmtest_rng_range(asmtest_rng_t *rng, long lo, long hi) {
    if (hi <= lo)
        return lo;
    uint64_t span = (uint64_t)(hi - lo) + 1; /* inclusive of both endpoints */
    return lo + (long)(asmtest_rng_u64(rng) % span);
}

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

void asmtest_match_ref1(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref1_fn ref, asmtest_gen_fn gen,
                        int trials) {
    asmtest_rng_t rng = {asmtest_seed()};
    for (int t = 0; t < trials; t++) {
        long a[8] = {0};
        int n = gen(&rng, a, 8);
        if (n != 1)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF1(%s): generator returned arity %d, "
                         "expected 1",
                         fnexpr, n);
        long expected = ref(a[0]);
        regs_t r;
        asm_call_capture_args(&r, fn, a, 1);
        long actual = (long)r.ret;
        if (actual != expected)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF1(%s): trial %d input [%ld]: got "
                         "%ld (0x%lx), reference %ld (0x%lx) [seed=0x%llx]",
                         fnexpr, t, a[0], actual, (unsigned long)actual,
                         expected, (unsigned long)expected,
                         (unsigned long long)asmtest_seed());
    }
}

void asmtest_match_ref2(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref2_fn ref, asmtest_gen_fn gen,
                        int trials) {
    asmtest_rng_t rng = {asmtest_seed()};
    for (int t = 0; t < trials; t++) {
        long a[8] = {0};
        int n = gen(&rng, a, 8);
        if (n != 2)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF2(%s): generator returned arity %d, "
                         "expected 2",
                         fnexpr, n);
        long expected = ref(a[0], a[1]);
        regs_t r;
        asm_call_capture_args(&r, fn, a, 2);
        long actual = (long)r.ret;
        if (actual != expected)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF2(%s): trial %d input [%ld, %ld]: "
                         "got %ld (0x%lx), reference %ld (0x%lx) [seed=0x%llx]",
                         fnexpr, t, a[0], a[1], actual, (unsigned long)actual,
                         expected, (unsigned long)expected,
                         (unsigned long long)asmtest_seed());
    }
}

void asmtest_match_ref3(const char *file, int line, const char *fnexpr,
                        void *fn, asmtest_ref3_fn ref, asmtest_gen_fn gen,
                        int trials) {
    asmtest_rng_t rng = {asmtest_seed()};
    for (int t = 0; t < trials; t++) {
        long a[8] = {0};
        int n = gen(&rng, a, 8);
        if (n != 3)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF3(%s): generator returned arity %d, "
                         "expected 3",
                         fnexpr, n);
        long expected = ref(a[0], a[1], a[2]);
        regs_t r;
        asm_call_capture_args(&r, fn, a, 3);
        long actual = (long)r.ret;
        if (actual != expected)
            asmtest_fail(file, line,
                         "ASSERT_MATCHES_REF3(%s): trial %d input [%ld, %ld, "
                         "%ld]: got %ld (0x%lx), reference %ld (0x%lx) "
                         "[seed=0x%llx]",
                         fnexpr, t, a[0], a[1], a[2], actual,
                         (unsigned long)actual, expected,
                         (unsigned long)expected,
                         (unsigned long long)asmtest_seed());
    }
}

/* ------------------------------------------------------------------ */
/* Crash handling: turn fatal signals into test failures               */
/* ------------------------------------------------------------------ */

static const char *sig_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL";
    default:      return "signal";
    }
}

static void asmtest_on_signal(int sig) {
    if (!asmtest_in_test) {
        /* Crash outside a test body: restore default and re-raise. */
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    snprintf(asmtest_msg, sizeof asmtest_msg, "caught fatal signal %d (%s)",
             sig, sig_name(sig));
    asmtest_loc_file = "(signal)";
    asmtest_loc_line = 0;
    siglongjmp(asmtest_jmp, JMP_FAIL);
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = asmtest_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Runner                                                              */
/* ------------------------------------------------------------------ */

static void run_hooks(const char *suite, int kind) {
    for (asmtest_hook_t *h = asmtest_hooks; h != NULL; h = h->next) {
        if (h->kind == kind && strcmp(h->suite, suite) == 0)
            h->fn();
    }
}

/* Run one test through setup -> body -> teardown; returns an ST_* outcome. */
static int run_one(asmtest_case_t *tc) {
    asmtest_in_test = 1;

    /* setup */
    if (sigsetjmp(asmtest_jmp, 1) != 0) {
        asmtest_in_test = 0;
        return ST_FAIL; /* setup failed/crashed: skip body and teardown */
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

    /* teardown — runs because setup succeeded; may itself fail */
    if (sigsetjmp(asmtest_jmp, 1) == 0)
        run_hooks(tc->suite, 1);
    else if (outcome != ST_FAIL)
        outcome = ST_FAIL;

    asmtest_in_test = 0;
    return outcome;
}

int main(void) {
    install_handlers();

    int use_color = isatty(STDOUT_FILENO);
    const char *grn = use_color ? "\033[32m" : "";
    const char *red = use_color ? "\033[31m" : "";
    const char *yel = use_color ? "\033[33m" : "";
    const char *dim = use_color ? "\033[2m" : "";
    const char *rst = use_color ? "\033[0m" : "";

    int total = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next)
        total++;

    printf("TAP version 13\n");
    printf("1..%d\n", total);

    int passed = 0, failed = 0, skipped = 0, i = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next) {
        i++;
        int outcome = run_one(tc);
        if (outcome == ST_PASS) {
            passed++;
            printf("%sok%s %d - %s.%s\n", grn, rst, i, tc->suite, tc->name);
        } else if (outcome == ST_SKIP) {
            skipped++;
            printf("%sok%s %d - %s.%s %s# SKIP %s%s\n", yel, rst, i, tc->suite,
                   tc->name, dim, asmtest_msg, rst);
        } else {
            failed++;
            printf("%snot ok%s %d - %s.%s\n", red, rst, i, tc->suite,
                   tc->name);
            printf("  %s---%s\n", dim, rst);
            printf("  at:  %s:%d\n", asmtest_loc_file, asmtest_loc_line);
            printf("  msg: %s\n", asmtest_msg);
            printf("  %s...%s\n", dim, rst);
        }
    }

    const char *summary_color = failed ? red : grn;
    printf("%s# %d passed, %d failed, %d skipped, %d total%s\n", summary_color,
           passed, failed, skipped, total, rst);

    return failed ? 1 : 0;
}
