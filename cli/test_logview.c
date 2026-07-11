/* test_logview.c — headless unit test for the TUI log-pane viewport math.
 *
 * The scrollback/pause window arithmetic (asmspy_logview.h) is the one piece of
 * the ncurses TUI that is both easy to get subtly wrong and impossible to drive
 * in CI, so it is factored out and checked here. Built + run by `make cli-smoke`
 * before the ptrace smoke.
 */
#include <stdio.h>
#include <stdlib.h>

#include "asmspy_logview.h"

static int failures;

/* assert that a bottom-anchored window yields the expected top line + length */
static void check(const char *name, unsigned long total, int count, long bottom,
                  int h, long want_top, int want_n) {
    long top = -999;
    int n = asmspy_log_window(total, count, bottom, h, &top);
    if (n != want_n || (want_n > 0 && top != want_top)) {
        fprintf(stderr,
                "FAIL %s: total=%lu count=%d bottom=%ld h=%d -> top=%ld n=%d, "
                "want top=%ld n=%d\n",
                name, total, count, bottom, h, top, n, want_top, want_n);
        failures++;
    }
}

int main(void) {
    /* empty buffer draws nothing */
    check("empty", 0, 0, 0, 10, 0, 0);
    check("empty/neg-bottom", 0, 0, -5, 10, 0, 0);

    /* fewer lines than the window: show them all, top = oldest (0) */
    check("partial", 5, 5, 4, 10, 0, 5);
    /* exactly full window at the tail */
    check("exact", 20, 20, 19, 10, 10, 10);

    /* tail of a full-but-uncapped buffer: bottom=newest, h lines up from it */
    check("tail", 100, 100, 99, 10, 90, 10);
    /* scrolled up into history */
    check("scrolled", 100, 100, 50, 10, 41, 10);
    /* scrolled to the very top: clamp top to oldest, window may be shorter */
    check("top-clamp", 100, 100, 5, 10, 0, 6);

    /* capped ring (cap=2048): oldest buffered line is total-count, not 0 */
    check("capped-tail", 5000, 2048, 4999, 10, 4990, 10);
    check("capped-oldest", 5000, 2048, 2952 /*=oldest*/, 10, 2952, 1);
    check("capped-below-oldest", 5000, 2048, 100, 10, 2952, 1);

    /* bottom past the newest is clamped back to newest */
    check("bottom-overshoot", 30, 30, 999, 5, 25, 5);
    /* h larger than the whole buffer */
    check("tall-window", 4, 4, 3, 100, 0, 4);
    /* degenerate height */
    check("h-zero", 50, 50, 49, 0, 0, 0);

    if (failures) {
        fprintf(stderr, "test_logview: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_logview: PASS\n");
    return 0;
}
