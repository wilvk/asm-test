/* asmspy_treefilter.h — the call-tree view's OUTPUT filter: depth cap, symbol
 * focus, module filter. Pure (no ptrace, no ncurses, no allocation), so the
 * decision that turns a firehose into a readable tree is unit-testable on its
 * own (cli/test_treefilter.c) instead of only reachable through a single-stepped
 * live process. Shared by asmspy_engine_tree() and the headless --tree flags.
 *
 * Header-only + static inline, matching the other extracted view-model pieces
 * (asmspy_graphsort.h, asmspy_logview.h, asmspy_dataview.h).
 *
 * THE ONE INVARIANT: filtering changes only what is EMITTED, never what is
 * TRACKED. The engine's per-thread call depth must keep counting every call and
 * every return whether or not the line is printed — a depth counter fed only the
 * surviving calls would drift the moment the first line is suppressed, and the
 * indentation of everything after it would be wrong.
 */
#ifndef ASMSPY_TREEFILTER_H
#define ASMSPY_TREEFILTER_H

#include <string.h>

/* All three filters are optional; a zeroed struct emits every call (the
 * pre-filter behaviour) and is what a NULL filter is treated as. */
typedef struct {
    int max_depth;      /* >0: emit only calls at effective depth < max_depth,
                         * i.e. `tree -L N` — N levels, not "up to depth N".   */
    const char *focus;  /* non-NULL/non-empty: a substring of a symbol name.
                         * Emit only calls inside the dynamic extent of a call
                         * to a matching symbol (that call included), and RE-BASE
                         * their depth so the focused function sits at depth 0.  */
    const char *module; /* non-NULL/non-empty: a substring of a module basename.
                         * Emit only calls whose callee is backed by it.        */
} asmspy_tree_filter_t;

/* Per-thread focus state: the real depth at which this thread entered the
 * focused subtree, or ASMSPY_TF_NO_FOCUS when it is outside one. Each thread
 * needs its own — thread A being inside the focused function says nothing about
 * thread B — so the engine keeps it in its per-thread table, not here. */
#define ASMSPY_TF_NO_FOCUS (-1)

/* Substring match with "no pattern matches everything" semantics. A NULL
 * haystack (an unresolved name/module) matches only the empty pattern. */
static inline int asmspy_tf_match(const char *hay, const char *needle) {
    if (!needle || !*needle)
        return 1;
    return hay && strstr(hay, needle) != NULL;
}

/* Decide whether a CALL to `name` (backed by `module`), made by a thread whose
 * live call depth is `depth` and whose focus state is *focus_depth, should be
 * emitted — and at what effective depth.
 *
 * On entering a focus root this sets *focus_depth (the caller must persist it
 * per-thread and pass it back). Returns 1 and writes *eff_depth when the call
 * should be emitted, 0 when it should be suppressed.
 *
 * The three filters compose as AND, and `focus` is applied FIRST because it is
 * the only one that establishes scope: --focus=work --module=libc means "the
 * libc calls work() makes", not "libc calls, and separately work()".
 */
static inline int asmspy_tree_filter_call(const asmspy_tree_filter_t *f,
                                          const char *name, const char *module,
                                          int depth, int *focus_depth,
                                          int *eff_depth) {
    int base = 0;
    if (f && f->focus && *f->focus) {
        if (*focus_depth == ASMSPY_TF_NO_FOCUS) {
            if (!asmspy_tf_match(name, f->focus))
                return 0;         /* outside every focused subtree */
            *focus_depth = depth; /* this call roots the displayed tree */
        }
        /* Already inside one: keep the OUTERMOST root, so a recursive focused
         * function stays one tree instead of re-rooting on every self-call. */
        base = *focus_depth;
    }
    int d = depth - base;
    if (d < 0) /* defensive: a drifted shadow counter must not go negative */
        d = 0;
    if (f && f->max_depth > 0 && d >= f->max_depth)
        return 0; /* below the cap */
    if (f && f->module && *f->module && !asmspy_tf_match(module, f->module))
        return 0;
    *eff_depth = d;
    return 1;
}

/* Update a thread's focus state after a RET popped its depth to `depth`.
 * Returning to (or above) the depth at which the focus root was ENTERED means
 * the focused function has returned, so the next matching call roots a fresh
 * tree. Cheap and unconditional — call it on every return. */
static inline void asmspy_tree_filter_ret(const asmspy_tree_filter_t *f,
                                          int depth, int *focus_depth) {
    if (!f || !f->focus || !*f->focus)
        return;
    if (*focus_depth != ASMSPY_TF_NO_FOCUS && depth <= *focus_depth)
        *focus_depth = ASMSPY_TF_NO_FOCUS;
}

/* 1 when any filter is actually engaged (lets a front-end label a filtered
 * view without re-deriving the condition from the three fields). */
static inline int asmspy_tree_filter_active(const asmspy_tree_filter_t *f) {
    if (!f)
        return 0;
    return f->max_depth > 0 || (f->focus && *f->focus) ||
           (f->module && *f->module);
}

#endif /* ASMSPY_TREEFILTER_H */
