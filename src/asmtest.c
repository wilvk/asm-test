/*
 * asmtest.c — runner, registries, failure/skip/crash model, fixtures,
 * register/flags assertions, guard-page buffers, and main() (Phase 2).
 */
#include "asmtest.h"

#include <errno.h>
#include <fnmatch.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
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

/* Per-test wall-clock timeout, enforced via alarm() (0 disables). Set from
 * --timeout / ASMTEST_TIMEOUT before the run; read by the SIGALRM handler. */
static int asmtest_timeout_secs = 10;

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
    case SIGABRT: return "SIGABRT";
    case SIGALRM: return "SIGALRM";
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
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = asmtest_on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL); /* per-test timeout (see run_one) */
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

/* Run one test through setup -> body -> teardown; returns an ST_* outcome.
 * A per-test alarm() converts an infinite loop into a SIGALRM the handler turns
 * into a timeout failure (works even in-process). On return the failure/skip
 * message lives in asmtest_msg / asmtest_loc_*. */
static int run_one(asmtest_case_t *tc) {
    asmtest_in_test = 1;
    if (asmtest_timeout_secs > 0)
        alarm((unsigned)asmtest_timeout_secs);

    /* setup */
    if (sigsetjmp(asmtest_jmp, 1) != 0) {
        alarm(0);
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

    alarm(0);
    asmtest_in_test = 0;
    return outcome;
}

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

/* Run in this process. Crashes that escape the signal handler (e.g. SIGABRT
 * from heap corruption) take down the whole runner — that's what --fork avoids. */
static void run_inproc(asmtest_case_t *tc, test_result_t *res) {
    wire_result_t w;
    capture_wire(&w, run_one(tc));
    apply_wire(res, &w);
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
    if (got == sizeof w) {
        apply_wire(res, &w);
        return;
    }
    /* Child died before reporting: describe it from the wait status. */
    res->outcome = ST_FAIL;
    res->line = 0;
    snprintf(res->file, sizeof res->file, "(child)");
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        if (sig == SIGALRM)
            snprintf(res->msg, sizeof res->msg,
                     "timed out after %d s (killed)", asmtest_timeout_secs);
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
    int fork_tests; /* per-test fork isolation (default on) */
    int timeout;    /* seconds; <0 = leave the default in place */
    int bench;      /* run benchmarks instead of tests */
    long bench_reps; /* fixed inner reps; 0 = auto-calibrate */
} options_t;

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
        "  --format=tap|junit   output format (default tap)\n"
        "  --bench              run registered benchmarks (BENCH) instead of "
        "tests\n"
        "  --bench-reps=N       fixed inner reps per round (default: "
        "auto-calibrate)\n"
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
    return fnmatch(glob, id, 0) == 0 || fnmatch(glob, suite, 0) == 0 ||
           fnmatch(glob, name, 0) == 0;
}

static int test_matches(const asmtest_case_t *tc, const char *glob) {
    return id_matches(tc->suite, tc->name, glob);
}

static void xml_print_escaped(const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&':  fputs("&amp;", stdout); break;
        case '<':  fputs("&lt;", stdout); break;
        case '>':  fputs("&gt;", stdout); break;
        case '"':  fputs("&quot;", stdout); break;
        case '\n': fputs("&#10;", stdout); break;
        default:   fputc((unsigned char)*s, stdout);
        }
    }
}

/* Render results as JUnit XML, grouped into <testsuite> by suite name. */
static void render_junit(const test_result_t *results, int n) {
    int failures = 0, skipped = 0;
    for (int i = 0; i < n; i++) {
        if (results[i].outcome == ST_FAIL)
            failures++;
        else if (results[i].outcome == ST_SKIP)
            skipped++;
    }
    printf("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    printf("<testsuites tests=\"%d\" failures=\"%d\" skipped=\"%d\">\n", n,
           failures, skipped);

    int *done = (int *)calloc((size_t)(n > 0 ? n : 1), sizeof(int));
    for (int i = 0; i < n; i++) {
        if (done[i])
            continue;
        const char *suite = results[i].suite;
        int s_tests = 0, s_fail = 0, s_skip = 0;
        for (int j = i; j < n; j++) {
            if (strcmp(results[j].suite, suite) != 0)
                continue;
            s_tests++;
            if (results[j].outcome == ST_FAIL)
                s_fail++;
            else if (results[j].outcome == ST_SKIP)
                s_skip++;
        }
        printf("  <testsuite name=\"");
        xml_print_escaped(suite);
        printf("\" tests=\"%d\" failures=\"%d\" skipped=\"%d\">\n", s_tests,
               s_fail, s_skip);
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
                printf("\n      <failure message=\"");
                xml_print_escaped(r->msg);
                printf("\">at %s:%d&#10;", r->file, r->line);
                xml_print_escaped(r->msg);
                printf("</failure>\n    ");
            } else if (r->outcome == ST_SKIP) {
                printf("\n      <skipped message=\"");
                xml_print_escaped(r->msg);
                printf("\"/>\n    ");
            }
            printf("</testcase>\n");
        }
        printf("  </testsuite>\n");
    }
    printf("</testsuites>\n");
    free(done);
}

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* Benchmark mode (Phase 9)                                            */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)
#define ASMTEST_BENCH_UNIT "cyc" /* rdtsc reference cycles */
#else
#define ASMTEST_BENCH_UNIT "ticks" /* cntvct_el0 virtual-timer ticks */
#endif

/* Calibration/measurement constants. Reps are auto-grown until one round spans
 * at least BENCH_TARGET ticks (so the counter's resolution doesn't dominate),
 * capped at BENCH_REPS_CAP; then BENCH_ROUNDS rounds are measured. */
enum {
    BENCH_TARGET = 50000,    /* per-round counter delta to calibrate toward */
    BENCH_REPS_CAP = 5000000, /* never loop more than this per round         */
    BENCH_ROUNDS = 11         /* measured rounds (odd, for a clean median)   */
};

typedef struct {
    double min, median, mean;
    long reps;
} bench_stats_t;

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
    st->reps = reps;
}

/* Run the selected benchmarks, printing an aligned table of cycles per call.
 * Each runs in-process under the same signal/timeout guard as a test, so a
 * crashing or hung routine is reported as an error rather than taking the
 * process down. forced_reps > 0 overrides auto-calibration. */
static void run_benchmarks(asmtest_bench_t **sel, int n, long forced_reps,
                           int use_color) {
    const char *dim = use_color ? "\033[2m" : "";
    const char *red = use_color ? "\033[31m" : "";
    const char *rst = use_color ? "\033[0m" : "";

    int width = 1;
    for (int i = 0; i < n; i++) {
        int w = (int)(strlen(sel[i]->suite) + strlen(sel[i]->name) + 1);
        if (w > width)
            width = w;
    }

    printf("%s# benchmarks — %s per call, min/median over %d rounds%s\n", dim,
           ASMTEST_BENCH_UNIT, BENCH_ROUNDS, rst);

    for (int i = 0; i < n; i++) {
        asmtest_bench_t *b = sel[i];
        char id[256];
        snprintf(id, sizeof id, "%s.%s", b->suite, b->name);

        asmtest_in_test = 1;
        if (asmtest_timeout_secs > 0)
            alarm((unsigned)asmtest_timeout_secs);
        if (sigsetjmp(asmtest_jmp, 1) != 0) {
            alarm(0);
            asmtest_in_test = 0;
            printf("  %-*s  %sERROR: %s%s\n", width, id, red, asmtest_msg, rst);
            continue;
        }
        long reps = forced_reps > 0 ? forced_reps : bench_calibrate(b->fn);
        bench_stats_t st;
        bench_measure(b->fn, reps, &st);
        alarm(0);
        asmtest_in_test = 0;

        printf("  %-*s  min=%9.2f  median=%9.2f  mean=%9.2f  %s(reps=%ld)%s\n",
               width, id, st.min, st.median, st.mean, dim, st.reps, rst);
    }
}

int main(int argc, char **argv) {
    options_t opt;
    memset(&opt, 0, sizeof opt);
    opt.fork_tests = 1;
    opt.timeout = -1;

    for (int ai = 1; ai < argc; ai++) {
        const char *a = argv[ai];
        const char *v;
        if (strcmp(a, "--list") == 0) {
            opt.do_list = 1;
        } else if (strcmp(a, "--shuffle") == 0) {
            opt.shuffle = 1;
        } else if (strcmp(a, "--no-fork") == 0) {
            opt.fork_tests = 0;
        } else if (strcmp(a, "--bench") == 0) {
            opt.bench = 1;
        } else if (opt_prefix(a, "--bench-reps=", &v)) {
            opt.bench = 1;
            opt.bench_reps = strtol(v, NULL, 0);
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

    /* Benchmark mode: collect, optionally list, then time the BENCH cases. */
    if (opt.bench) {
        int btotal = 0;
        for (asmtest_bench_t *b = asmtest_bench_head; b != NULL; b = b->next)
            btotal++;
        asmtest_bench_t **bsel = (asmtest_bench_t **)malloc(
            (size_t)(btotal > 0 ? btotal : 1) * sizeof *bsel);
        int bn = 0;
        for (asmtest_bench_t *b = asmtest_bench_head; b != NULL; b = b->next) {
            if (opt.filter == NULL || id_matches(b->suite, b->name, opt.filter))
                bsel[bn++] = b;
        }
        if (opt.do_list) {
            for (int i = 0; i < bn; i++)
                printf("%s.%s\n", bsel[i]->suite, bsel[i]->name);
        } else {
            run_benchmarks(bsel, bn, opt.bench_reps, isatty(STDOUT_FILENO));
        }
        free(bsel);
        return 0;
    }

    /* Collect the registered tests that pass the filter into an array. */
    int total = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next)
        total++;
    asmtest_case_t **sel =
        (asmtest_case_t **)malloc((size_t)(total > 0 ? total : 1) * sizeof *sel);
    int n = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next) {
        if (opt.filter == NULL || test_matches(tc, opt.filter))
            sel[n++] = tc;
    }

    if (opt.do_list) {
        for (int i = 0; i < n; i++)
            printf("%s.%s\n", sel[i]->suite, sel[i]->name);
        free(sel);
        return 0;
    }

    if (opt.shuffle) {
        uint64_t seed = opt.has_seed
                            ? opt.seed
                            : ((uint64_t)time(NULL) ^ ((uint64_t)getpid() << 32));
        asmtest_rng_t rng = {seed};
        for (int i = n - 1; i > 0; i--) { /* Fisher-Yates */
            int j = (int)(asmtest_rng_u64(&rng) % (uint64_t)(i + 1));
            asmtest_case_t *t = sel[i];
            sel[i] = sel[j];
            sel[j] = t;
        }
        if (!opt.format_junit)
            printf("# shuffle seed=0x%llx\n", (unsigned long long)seed);
    }

    int use_color = !opt.format_junit && isatty(STDOUT_FILENO);
    const char *grn = use_color ? "\033[32m" : "";
    const char *red = use_color ? "\033[31m" : "";
    const char *yel = use_color ? "\033[33m" : "";
    const char *dim = use_color ? "\033[2m" : "";
    const char *rst = use_color ? "\033[0m" : "";

    if (!opt.format_junit) {
        printf("TAP version 13\n");
        printf("1..%d\n", n);
    }

    test_result_t *results =
        (test_result_t *)malloc((size_t)(n > 0 ? n : 1) * sizeof *results);
    int passed = 0, failed = 0, skipped = 0;

    for (int i = 0; i < n; i++) {
        asmtest_case_t *tc = sel[i];
        test_result_t *r = &results[i];
        memset(r, 0, sizeof *r);
        r->suite = tc->suite;
        r->name = tc->name;

        double t0 = now_secs();
        if (opt.fork_tests)
            run_forked(tc, r);
        else
            run_inproc(tc, r);
        r->secs = now_secs() - t0;

        if (r->outcome == ST_PASS)
            passed++;
        else if (r->outcome == ST_SKIP)
            skipped++;
        else
            failed++;

        if (opt.format_junit)
            continue; /* JUnit is rendered in one pass at the end */

        if (r->outcome == ST_PASS) {
            printf("%sok%s %d - %s.%s\n", grn, rst, i + 1, r->suite, r->name);
        } else if (r->outcome == ST_SKIP) {
            printf("%sok%s %d - %s.%s %s# SKIP %s%s\n", yel, rst, i + 1,
                   r->suite, r->name, dim, r->msg, rst);
        } else {
            printf("%snot ok%s %d - %s.%s\n", red, rst, i + 1, r->suite,
                   r->name);
            printf("  %s---%s\n", dim, rst);
            printf("  at:  %s:%d\n", r->file, r->line);
            printf("  msg: %s\n", r->msg);
            printf("  %s...%s\n", dim, rst);
        }
    }

    if (opt.format_junit) {
        render_junit(results, n);
    } else {
        const char *summary_color = failed ? red : grn;
        printf("%s# %d passed, %d failed, %d skipped, %d total%s\n",
               summary_color, passed, failed, skipped, n, rst);
    }

    free(results);
    free(sel);
    return failed ? 1 : 0;
}
