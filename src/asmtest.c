/*
 * asmtest.c — runner, registries, failure/skip model, fixtures, main() (Phase 1).
 */
#include "asmtest.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* longjmp return codes for the per-test jump buffer. */
enum { JMP_FAIL = 1, JMP_SKIP = 2 };

/* Test outcome. */
enum { ST_PASS, ST_FAIL, ST_SKIP };

static asmtest_case_t *asmtest_head = NULL;
static asmtest_case_t *asmtest_tail = NULL;
static asmtest_hook_t *asmtest_hooks = NULL;

jmp_buf asmtest_jmp;

static char asmtest_msg[1024];
static const char *asmtest_loc_file;
static int asmtest_loc_line;

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

void asmtest_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(asmtest_msg, sizeof asmtest_msg, fmt, ap);
    va_end(ap);
    asmtest_loc_file = file;
    asmtest_loc_line = line;
    longjmp(asmtest_jmp, JMP_FAIL);
}

void asmtest_skip(const char *reason) {
    snprintf(asmtest_msg, sizeof asmtest_msg, "%s", reason ? reason : "");
    longjmp(asmtest_jmp, JMP_SKIP);
}

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

void asmtest_assert_mem_eq(const char *file, int line, const char *aexpr,
                           const char *bexpr, const void *a, const void *b,
                           size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            asmtest_fail(file, line,
                         "ASSERT_MEM_EQ(%s, %s, %zu): byte %zu differs: "
                         "0x%02x != 0x%02x",
                         aexpr, bexpr, n, i, pa[i], pb[i]);
    }
}

static void run_hooks(const char *suite, int kind) {
    for (asmtest_hook_t *h = asmtest_hooks; h != NULL; h = h->next) {
        if (h->kind == kind && strcmp(h->suite, suite) == 0)
            h->fn();
    }
}

/* Run one test through setup -> body -> teardown; returns an ST_* outcome. */
static int run_one(asmtest_case_t *tc) {
    int rc;

    /* setup */
    rc = setjmp(asmtest_jmp);
    if (rc == 0) {
        run_hooks(tc->suite, 0);
    } else {
        return ST_FAIL; /* setup failed or skipped -> count as fail/skip */
    }

    /* body */
    int outcome = ST_PASS;
    rc = setjmp(asmtest_jmp);
    if (rc == 0)
        tc->fn();
    else if (rc == JMP_SKIP)
        outcome = ST_SKIP;
    else
        outcome = ST_FAIL;

    /* teardown — runs because setup succeeded; may itself fail */
    rc = setjmp(asmtest_jmp);
    if (rc == 0)
        run_hooks(tc->suite, 1);
    else if (outcome != ST_FAIL)
        outcome = ST_FAIL;

    return outcome;
}

int main(void) {
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
