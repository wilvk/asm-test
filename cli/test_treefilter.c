/* test_treefilter.c — headless unit test for the call-tree output filter
 * (asmspy_treefilter.h: --depth depth cap, --focus symbol focus, --module
 * module filter).
 *
 * The filter decides what a busy process's call tree SHOWS, and every one of its
 * interesting cases is a sequence — focus opens on a call and closes on the
 * matching return, depth re-bases underneath it — which a live single-stepped
 * victim can only demonstrate for whichever shapes that victim happens to run.
 * So the sequences are replayed here directly: each test drives the same
 * call/ret pairs the engine would, and asserts BOTH which lines survive and at
 * what depth.
 *
 * The invariant under test throughout: filtering changes what is EMITTED, never
 * what is TRACKED. Every test therefore keeps the raw depth counter running over
 * ALL calls/returns (exactly as the engine does) and only asks the filter what
 * to print — a filter that fed itself would drift after its first suppression.
 *
 * Built + run by `make cli-smoke` before the ptrace smoke (no ncurses, no
 * ptrace, no allocation).
 */
#include <stdio.h>
#include <string.h>

#include "asmspy_treefilter.h"

static int failures;

/* ------------------------------------------------------------------ */
/* A miniature of the engine's tree loop: replay a scripted call/ret    */
/* stream through the filter and record the surviving lines.            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;   /* callee symbol, or NULL for a RET */
    const char *module; /* callee module ("?" when unresolved) */
} step_t;

#define RET ((const char *)0)

typedef struct {
    char out[64][80]; /* "<eff_depth>:<name>" per surviving line */
    size_t n;
} rec_t;

/* Drive `steps` through `f` exactly as asmspy_engine_tree does: push the depth
 * on every call and pop it on every return REGARDLESS of the filter verdict,
 * and only consult the filter for whether to emit. */
static void replay(const asmspy_tree_filter_t *f, const step_t *steps,
                   size_t ns, rec_t *r) {
    int depth = 0;
    int focus = ASMSPY_TF_NO_FOCUS;
    r->n = 0;
    for (size_t i = 0; i < ns; i++) {
        if (steps[i].name == RET) {
            if (depth > 0)
                depth--;
            asmspy_tree_filter_ret(f, depth, &focus);
            continue;
        }
        int eff = 0;
        if (asmspy_tree_filter_call(f, steps[i].name, steps[i].module, depth,
                                    &focus, &eff) &&
            r->n < 64)
            snprintf(r->out[r->n++], sizeof r->out[0], "%d:%s", eff,
                     steps[i].name);
        depth++; /* unconditional — the shadow counter tracks reality */
    }
}

/* Assert the surviving lines are exactly `want` (NULL-terminated). */
static void check(const char *what, const asmspy_tree_filter_t *f,
                  const step_t *steps, size_t ns, const char *const *want) {
    rec_t r;
    replay(f, steps, ns, &r);
    size_t nw = 0;
    while (want[nw])
        nw++;
    if (r.n != nw) {
        fprintf(stderr, "FAIL %s: emitted %zu lines, want %zu\n", what, r.n,
                nw);
        for (size_t i = 0; i < r.n; i++)
            fprintf(stderr, "   got[%zu] = %s\n", i, r.out[i]);
        failures++;
        return;
    }
    for (size_t i = 0; i < nw; i++) {
        if (strcmp(r.out[i], want[i]) != 0) {
            fprintf(stderr, "FAIL %s: line %zu is '%s', want '%s'\n", what, i,
                    r.out[i], want[i]);
            failures++;
            return;
        }
    }
    printf("ok   %s (%zu lines)\n", what, nw);
}

/* main -> work -> helper -> leaf, then work -> puts@plt: the shape the smoke's
 * spy_victim actually runs, deep enough to exercise a cap and a re-base. */
static const step_t prog[] = {
    {"work", "spy_victim"},    /*  depth 0 */
    {"helper", "spy_victim"},  /*  depth 1 */
    {"leaf", "spy_victim"},    /*  depth 2 */
    {RET, NULL},               /*  -> 2    */
    {RET, NULL},               /*  -> 1    */
    {"puts@plt", "libc.so.6"}, /*  depth 1 */
    {RET, NULL},               /*  -> 1    */
    {RET, NULL},               /*  -> 0    */
    {"other", "spy_victim"},   /*  depth 0 */
    {RET, NULL},
};
#define NPROG (sizeof prog / sizeof prog[0])

int main(void) {
    /* ---- unfiltered: every call, true depths (the pre-filter behaviour) --- */
    {
        asmspy_tree_filter_t f = {0};
        const char *want[] = {"0:work",     "1:helper", "2:leaf",
                              "1:puts@plt", "0:other",  NULL};
        check("unfiltered emits every call at its true depth", &f, prog, NPROG,
              want);
        /* A NULL filter must behave identically — the engine passes NULL from
         * the TUI, so a divergence here would be a live-view-only bug. */
        rec_t a, b;
        replay(&f, prog, NPROG, &a);
        replay(NULL, prog, NPROG, &b);
        if (a.n != b.n || memcmp(a.out, b.out, a.n * sizeof a.out[0]) != 0) {
            fprintf(stderr, "FAIL NULL filter differs from a zeroed filter\n");
            failures++;
        } else {
            printf("ok   NULL filter == zeroed filter\n");
        }
        if (asmspy_tree_filter_active(&f) || asmspy_tree_filter_active(NULL)) {
            fprintf(stderr, "FAIL zeroed/NULL filter reports active\n");
            failures++;
        } else {
            printf("ok   zeroed/NULL filter reports inactive\n");
        }
    }

    /* ---- depth cap: `tree -L N` semantics — N levels, depths 0..N-1 ------- */
    {
        asmspy_tree_filter_t f = {0};
        f.max_depth = 1;
        const char *want[] = {"0:work", "0:other", NULL};
        check("--depth=1 keeps only the top level", &f, prog, NPROG, want);
    }
    {
        asmspy_tree_filter_t f = {0};
        f.max_depth = 2;
        const char *want[] = {"0:work", "1:helper", "1:puts@plt", "0:other",
                              NULL};
        check("--depth=2 keeps two levels", &f, prog, NPROG, want);
    }
    {
        asmspy_tree_filter_t f = {0};
        f.max_depth = 99;
        const char *want[] = {"0:work",     "1:helper", "2:leaf",
                              "1:puts@plt", "0:other",  NULL};
        check("--depth beyond the tree keeps everything", &f, prog, NPROG,
              want);
        if (!asmspy_tree_filter_active(&f)) {
            fprintf(stderr, "FAIL a depth cap must report active\n");
            failures++;
        }
    }

    /* ---- focus: root the tree at a symbol, RE-BASE the depths under it ---- */
    {
        asmspy_tree_filter_t f = {0};
        f.focus = "helper";
        /* helper is called at real depth 1 -> it must render at 0, and leaf,
         * its callee at real depth 2, at 1. Everything outside is gone. */
        const char *want[] = {"0:helper", "1:leaf", NULL};
        check("--focus re-bases the subtree to depth 0", &f, prog, NPROG, want);
    }
    {
        /* THE closing assertion: focus must END when the focused function
         * returns. puts@plt runs at real depth 1, the SAME depth helper ran at,
         * but AFTER helper returned — a filter that never closed its focus (or
         * that closed it on the wrong return) would leak it in here. */
        asmspy_tree_filter_t f = {0};
        f.focus = "helper";
        rec_t r;
        replay(&f, prog, NPROG, &r);
        for (size_t i = 0; i < r.n; i++) {
            if (strstr(r.out[i], "puts@plt")) {
                fprintf(stderr,
                        "FAIL --focus leaked a post-return sibling: "
                        "%s\n",
                        r.out[i]);
                failures++;
            }
        }
        printf("ok   --focus closes on the focused function's return\n");
    }
    {
        asmspy_tree_filter_t f = {0};
        f.focus = "nosuchsymbol";
        const char *want[] = {NULL};
        check("--focus on an absent symbol emits nothing", &f, prog, NPROG,
              want);
    }
    {
        /* Two separate invocations of the focused function must produce two
         * separate trees, each re-based — proving the state RESET, not just
         * that it was set once. */
        static const step_t twice[] = {
            {"work", "m"},   {"helper", "m"}, {"leaf", "m"},
            {RET, NULL},     {RET, NULL},     {RET, NULL}, /* back to depth 0 */
            {"helper", "m"}, /* second invocation, real depth 0 */
            {"leaf", "m"},   {RET, NULL},     {RET, NULL},
        };
        asmspy_tree_filter_t f = {0};
        f.focus = "helper";
        const char *want[] = {"0:helper", "1:leaf", "0:helper", "1:leaf", NULL};
        check("--focus re-opens on a second invocation", &f, twice,
              sizeof twice / sizeof twice[0], want);
    }
    {
        /* A recursive focused function must stay ONE tree rooted at the
         * outermost entry, not re-root (and re-base to 0) on each self-call. */
        static const step_t rec[] = {
            {"rec", "m"}, /* depth 0 -> root */
            {"rec", "m"}, /* depth 1 -> must render at 1, NOT 0 */
            {"rec", "m"}, /* depth 2 -> must render at 2 */
            {RET, NULL},  {RET, NULL}, {RET, NULL},
        };
        asmspy_tree_filter_t f = {0};
        f.focus = "rec";
        const char *want[] = {"0:rec", "1:rec", "2:rec", NULL};
        check("--focus keeps the OUTERMOST root when recursive", &f, rec,
              sizeof rec / sizeof rec[0], want);
    }

    /* ---- module filter ---------------------------------------------------- */
    {
        asmspy_tree_filter_t f = {0};
        f.module = "libc";
        const char *want[] = {"1:puts@plt", NULL};
        check("--module keeps only that module's callees", &f, prog, NPROG,
              want);
    }
    {
        /* An unresolved callee renders "0x…"/"?" — it must NOT match a real
         * module filter, but --module='?' must single it out. */
        static const step_t un[] = {
            {"named", "libc.so.6"},
            {RET, NULL},
            {"0x7f00", "?"},
            {RET, NULL},
        };
        asmspy_tree_filter_t f = {0};
        f.module = "libc";
        const char *want[] = {"0:named", NULL};
        check("--module skips unresolved callees", &f, un,
              sizeof un / sizeof un[0], want);
        asmspy_tree_filter_t g = {0};
        g.module = "?";
        const char *want2[] = {"0:0x7f00", NULL};
        check("--module='?' selects the unresolved callees", &g, un,
              sizeof un / sizeof un[0], want2);
    }

    /* ---- composition: focus establishes SCOPE, module narrows within it --- */
    {
        /* --focus=work --module=libc = "the libc calls work() makes", NOT
         * "libc calls anywhere". other_libc_caller's libc call must not leak. */
        static const step_t mix[] = {
            {"work", "m"},             /* 0: focus root (module m, dropped) */
            {"puts@plt", "libc.so.6"}, /* 1 -> emitted at 1 */
            {RET, NULL},
            {RET, NULL},                 /* out of work */
            {"other_caller", "m"},       /* 0 */
            {"malloc@plt", "libc.so.6"}, /* 1 — outside the focus: dropped */
            {RET, NULL},
            {RET, NULL},
        };
        asmspy_tree_filter_t f = {0};
        f.focus = "work";
        f.module = "libc";
        const char *want[] = {"1:puts@plt", NULL};
        check("--focus + --module = that module's calls INSIDE the focus", &f,
              mix, sizeof mix / sizeof mix[0], want);
    }
    {
        /* All three at once, and the depth cap must measure from the RE-BASED
         * root: helper is the root (eff 0), leaf is eff 1 -> --depth=1 cuts it. */
        asmspy_tree_filter_t f = {0};
        f.focus = "helper";
        f.max_depth = 1;
        const char *want[] = {"0:helper", NULL};
        check("--depth measures from the --focus root, not the real depth", &f,
              prog, NPROG, want);
    }

    /* ---- substring semantics + defensive edges --------------------------- */
    {
        asmspy_tree_filter_t f = {0};
        f.focus = "elp"; /* substring, not exact — matches "helper" */
        const char *want[] = {"0:helper", "1:leaf", NULL};
        check("--focus matches on a substring", &f, prog, NPROG, want);
    }
    {
        asmspy_tree_filter_t f = {0};
        f.focus = ""; /* empty = not engaged (same as unset) */
        f.module = "";
        const char *want[] = {"0:work",     "1:helper", "2:leaf",
                              "1:puts@plt", "0:other",  NULL};
        check("empty filter strings are treated as unset", &f, prog, NPROG,
              want);
        if (asmspy_tree_filter_active(&f)) {
            fprintf(stderr, "FAIL empty-string filters report active\n");
            failures++;
        }
    }
    {
        /* A drifted shadow counter (a return we never saw a call for) must not
         * produce a negative effective depth. */
        asmspy_tree_filter_t f = {0};
        f.focus = "b";
        int focus = 5; /* focus root recorded deeper than we now are */
        int eff = -99;
        if (!asmspy_tree_filter_call(&f, "b", "m", 2, &focus, &eff) ||
            eff != 0) {
            fprintf(stderr,
                    "FAIL drifted depth below the focus root: eff=%d, want 0\n",
                    eff);
            failures++;
        } else {
            printf("ok   a drifted depth clamps to 0, never negative\n");
        }
    }

    if (failures) {
        fprintf(stderr, "test_treefilter: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_treefilter: all checks passed\n");
    return 0;
}
