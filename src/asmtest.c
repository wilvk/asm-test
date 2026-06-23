/*
 * asmtest.c — runner, test registry, failure model, and main() (Phase 0).
 */
#include "asmtest.h"

#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

static asmtest_case_t *asmtest_head = NULL;
static asmtest_case_t *asmtest_tail = NULL;

jmp_buf asmtest_jmp;

static char asmtest_fail_msg[1024];
static const char *asmtest_fail_file;
static int asmtest_fail_line;

void asmtest_register(asmtest_case_t *tc) {
    tc->next = NULL;
    if (asmtest_head == NULL)
        asmtest_head = tc;
    else
        asmtest_tail->next = tc;
    asmtest_tail = tc;
}

void asmtest_fail(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(asmtest_fail_msg, sizeof asmtest_fail_msg, fmt, ap);
    va_end(ap);
    asmtest_fail_file = file;
    asmtest_fail_line = line;
    longjmp(asmtest_jmp, 1);
}

int main(void) {
    int use_color = isatty(STDOUT_FILENO);
    const char *grn = use_color ? "\033[32m" : "";
    const char *red = use_color ? "\033[31m" : "";
    const char *dim = use_color ? "\033[2m" : "";
    const char *rst = use_color ? "\033[0m" : "";

    int total = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next)
        total++;

    printf("TAP version 13\n");
    printf("1..%d\n", total);

    int passed = 0, failed = 0, i = 0;
    for (asmtest_case_t *tc = asmtest_head; tc != NULL; tc = tc->next) {
        i++;
        if (setjmp(asmtest_jmp) == 0) {
            tc->fn();
            passed++;
            printf("%sok%s %d - %s.%s\n", grn, rst, i, tc->suite, tc->name);
        } else {
            failed++;
            printf("%snot ok%s %d - %s.%s\n", red, rst, i, tc->suite,
                   tc->name);
            printf("  %s---%s\n", dim, rst);
            printf("  at:  %s:%d\n", asmtest_fail_file, asmtest_fail_line);
            printf("  msg: %s\n", asmtest_fail_msg);
            printf("  %s...%s\n", dim, rst);
        }
    }

    const char *summary_color = failed ? red : grn;
    printf("%s# %d passed, %d failed, %d total%s\n", summary_color, passed,
           failed, total, rst);

    return failed ? 1 : 0;
}
