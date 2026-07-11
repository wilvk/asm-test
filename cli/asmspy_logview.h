/*
 * asmspy_logview.h — ring-buffer viewport math for asmspy's TUI log panes.
 *
 * Factored out of the ncurses code (asmspy.c) so the off-by-one-prone index
 * arithmetic behind scrollback/pause can be unit-tested headlessly
 * (cli/test_logview.c) — the TUI itself can't be driven in CI.
 *
 * A log pane is a ring buffer of `cap` lines. `total` is the monotonic count of
 * lines ever pushed; `count = min(total, cap)` are currently buffered. Absolute
 * line index L (0-based, in [total-count, total)) lives at slot `L % cap`.
 */
#ifndef ASMSPY_LOGVIEW_H
#define ASMSPY_LOGVIEW_H

/* Compute the window of lines to draw for a height-`h` pane whose BOTTOM line is
 * absolute index `bottom`. Clamps `bottom` into the buffered range
 * [total-count, total-1], writes the TOP absolute line to *top_abs, and returns
 * how many lines to draw (0 if the buffer is empty or h<1). The caller maps each
 * absolute line L in [*top_abs, *top_abs+n) to its slot with L % cap. */
static inline int asmspy_log_window(unsigned long total, int count, long bottom,
                                    int h, long *top_abs) {
    if (count <= 0 || h < 1) {
        *top_abs = 0;
        return 0;
    }
    long oldest = (long)total - count;
    long newest = (long)total - 1;
    if (bottom > newest)
        bottom = newest;
    if (bottom < oldest)
        bottom = oldest;
    long top = bottom - h + 1;
    if (top < oldest)
        top = oldest;
    *top_abs = top;
    return (int)(bottom - top + 1);
}

#endif /* ASMSPY_LOGVIEW_H */
