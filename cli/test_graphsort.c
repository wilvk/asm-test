/* test_graphsort.c — headless unit test for the call-graph sort comparator.
 *
 * gnode_cmp (asmspy_graphsort.h) ranks the whole-process call-graph view for
 * both --graph --sort=invocations|fanout and the TUI, but its ordering and
 * two-level tiebreak (other metric, then name) were unasserted — a flipped
 * comparison would just render a differently-ordered table that still "looks
 * right". Pins the contract: DESCENDING by the selected key, ties DESCENDING
 * on the other metric, remaining ties ASCENDING by name. Built + run by
 * `make cli-smoke` before the ptrace smoke (no ncurses, no ptrace).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmspy_graphsort.h"

static int failures;

/* Build a node with just the fields the comparator reads. */
static asmspy_gnode_t node(const char *name, unsigned long long inv,
                           unsigned fanout) {
    asmspy_gnode_t n;
    memset(&n, 0, sizeof n);
    snprintf(n.name, sizeof n.name, "%s", name);
    n.invocations = inv;
    n.fanout = fanout;
    return n;
}

/* qsort `v` under `key` and assert the resulting name order. */
static void check_order(const char *what, gsort_t key, asmspy_gnode_t *v,
                        size_t n, const char *const *want) {
    graph_sort_key = key;
    qsort(v, n, sizeof *v, gnode_cmp);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(v[i].name, want[i]) != 0) {
            fprintf(stderr, "FAIL %s: pos %zu is '%s', want '%s'\n", what, i,
                    v[i].name, want[i]);
            failures++;
            return;
        }
    }
}

/* Assert the comparator itself: sign of cmp(a,b), and antisymmetry. */
static void check_cmp(const char *what, gsort_t key, asmspy_gnode_t a,
                      asmspy_gnode_t b, int want_sign) {
    graph_sort_key = key;
    int ab = gnode_cmp(&a, &b);
    int ba = gnode_cmp(&b, &a);
    int sab = ab < 0 ? -1 : ab > 0 ? 1 : 0;
    int sba = ba < 0 ? -1 : ba > 0 ? 1 : 0;
    if (sab != want_sign || sba != -want_sign) {
        fprintf(stderr, "FAIL %s: cmp(a,b)=%d cmp(b,a)=%d, want signs %d/%d\n",
                what, ab, ba, want_sign, -want_sign);
        failures++;
    }
}

/* Assert the context-explicit CORE directly, with NO latch assignment — this is
 * the entry the GUI's concurrent panels use, so it must be correct without the
 * file-scope selector ever being touched. Checks the sign of cmp(a,b) and
 * antisymmetry, AND that the qsort adapter (latch = key) agrees with the core on
 * the same pair. */
static void check_cmp_key(const char *what, gsort_t key, asmspy_gnode_t a,
                          asmspy_gnode_t b, int want_sign) {
    int ab = gnode_cmp_key(&a, &b, key);
    int ba = gnode_cmp_key(&b, &a, key);
    int sab = ab < 0 ? -1 : ab > 0 ? 1 : 0;
    int sba = ba < 0 ? -1 : ba > 0 ? 1 : 0;
    if (sab != want_sign || sba != -want_sign) {
        fprintf(stderr,
                "FAIL %s: cmp_key(a,b)=%d cmp_key(b,a)=%d, want signs %d/%d\n",
                what, ab, ba, want_sign, -want_sign);
        failures++;
    }
    /* adapter (single-thread latch) must equal the core on the same pair */
    graph_sort_key = key;
    if (gnode_cmp(&a, &b) != ab) {
        fprintf(stderr, "FAIL %s: adapter disagrees with core (%d vs %d)\n", what,
                gnode_cmp(&a, &b), ab);
        failures++;
    }
}

int main(void) {
    /* --sort=invocations: descending by invocations, regardless of fanout */
    {
        asmspy_gnode_t v[] = {node("low", 1, 9), node("high", 100, 0),
                              node("mid", 10, 5)};
        const char *const want[] = {"high", "mid", "low"};
        check_order("inv/primary", GSORT_INVOCATIONS, v, 3, want);
    }
    /* --sort=fanout: descending by fanout, regardless of invocations */
    {
        asmspy_gnode_t v[] = {node("low", 900, 1), node("high", 1, 7),
                              node("mid", 50, 3)};
        const char *const want[] = {"high", "mid", "low"};
        check_order("fanout/primary", GSORT_FANOUT, v, 3, want);
    }
    /* tie on the selected key -> the OTHER metric breaks it, descending */
    {
        asmspy_gnode_t v[] = {node("thin", 5, 1), node("wide", 5, 8),
                              node("mid", 5, 4)};
        const char *const want[] = {"wide", "mid", "thin"};
        check_order("inv/tie->fanout", GSORT_INVOCATIONS, v, 3, want);
    }
    {
        asmspy_gnode_t v[] = {node("cold", 2, 3), node("hot", 70, 3),
                              node("warm", 9, 3)};
        const char *const want[] = {"hot", "warm", "cold"};
        check_order("fanout/tie->inv", GSORT_FANOUT, v, 3, want);
    }
    /* tie on BOTH metrics -> name ascending, so the order is deterministic */
    {
        asmspy_gnode_t v[] = {node("zeta", 4, 2), node("alpha", 4, 2),
                              node("mu", 4, 2)};
        const char *const want[] = {"alpha", "mu", "zeta"};
        check_order("full-tie->name", GSORT_INVOCATIONS, v, 3, want);
        const char *const want2[] = {"alpha", "mu", "zeta"};
        check_order("full-tie->name/fanout", GSORT_FANOUT, v, 3, want2);
    }
    /* the two sort keys really select different primaries on the same data */
    {
        asmspy_gnode_t v1[] = {node("caller", 1, 30), node("leaf", 1000, 0)};
        const char *const want_inv[] = {"leaf", "caller"};
        check_order("keys-differ/inv", GSORT_INVOCATIONS, v1, 2, want_inv);
        asmspy_gnode_t v2[] = {node("caller", 1, 30), node("leaf", 1000, 0)};
        const char *const want_fan[] = {"caller", "leaf"};
        check_order("keys-differ/fanout", GSORT_FANOUT, v2, 2, want_fan);
    }
    /* comparator properties: reflexive equality, antisymmetric signs */
    check_cmp("cmp/equal", GSORT_INVOCATIONS, node("same", 3, 3),
              node("same", 3, 3), 0);
    check_cmp("cmp/inv", GSORT_INVOCATIONS, node("a", 9, 0), node("b", 3, 9),
              -1); /* more invocations sorts FIRST (descending) */
    check_cmp("cmp/fanout", GSORT_FANOUT, node("a", 9, 1), node("b", 0, 2),
              1); /* less fanout sorts LAST under the fanout key */
    check_cmp("cmp/tie-name", GSORT_INVOCATIONS, node("aaa", 5, 5),
              node("bbb", 5, 5), -1); /* full tie: name ascending */

    /* context-explicit core (the GUI entry): correct with NO latch touched, for
     * both keys, and in agreement with the qsort adapter */
    check_cmp_key("key/inv", GSORT_INVOCATIONS, node("a", 9, 0), node("b", 3, 9),
                  -1); /* more invocations sorts FIRST (descending) */
    check_cmp_key("key/fanout", GSORT_FANOUT, node("a", 9, 1), node("b", 0, 2),
                  1); /* less fanout sorts LAST under the fanout key */
    check_cmp_key("key/tie->other", GSORT_INVOCATIONS, node("a", 5, 8),
                  node("b", 5, 1), -1); /* tie on inv -> more fanout first */
    check_cmp_key("key/full-tie->name", GSORT_FANOUT, node("aaa", 5, 5),
                  node("bbb", 5, 5), -1); /* full tie: name ascending */
    check_cmp_key("key/equal", GSORT_INVOCATIONS, node("same", 3, 3),
                  node("same", 3, 3), 0);

    if (failures) {
        fprintf(stderr, "test_graphsort: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_graphsort: PASS\n");
    return 0;
}
