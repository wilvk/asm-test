/*
 * asmtest.c — runner, registries, failure/skip/crash model, fixtures,
 * register/flags assertions, guard-page buffers, and main() (Phase 2).
 */
#include "asmtest.h"

#include <signal.h>
#include <stdarg.h>
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
