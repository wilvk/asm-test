/*
 * descent.c — the call-descent handle (asmtest_descent_t): config in, edges + nested
 * frames out. Sibling of src/trace.c; owns the growable pools and the scalar read
 * accessors every language binding consumes, while the descending step loop in
 * src/ptrace_backend.c fills them through the mutators in asmtest_descent_internal.h.
 *
 * The design keeps asmtest_trace_t byte-for-byte frozen (see docs/internal/archive/plans/call-descent-
 * plan.md): descent is a SEPARATE opaque handle, so bindings add accessor calls rather
 * than mirroring a new struct layout, and frame 0 remains a superset of the flat trace.
 *
 * No external library: plain malloc/realloc pools, so it links into libasmtest_hwtrace
 * next to the ptrace backend and needs nothing Capstone/Unicorn.
 */
#include "asmtest_descent_internal.h"
#include "asmtest_ptrace.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Growable-pool helpers                                               */
/* ------------------------------------------------------------------ */

/* Ensure *arr can hold at least `need` elements of `elem` bytes, doubling capacity.
 * Returns 1 on success, 0 on OOM (leaving the existing buffer intact). */
static int pool_reserve(void **arr, size_t *cap, size_t need, size_t elem) {
    if (need <= *cap)
        return 1;
    size_t ncap = *cap ? *cap : 8;
    while (ncap < need)
        ncap *= 2;
    void *p = realloc(*arr, ncap * elem);
    if (p == NULL)
        return 0;
    *arr = p;
    *cap = ncap;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Allocate / free / configure                                         */
/* ------------------------------------------------------------------ */

asmtest_descent_t *asmtest_descent_new(asmtest_descent_level_t level) {
    asmtest_descent_t *d = (asmtest_descent_t *)calloc(1, sizeof *d);
    if (d == NULL)
        return NULL;
    d->level = level;
    d->max_depth = ASMTEST_DESCENT_DEFAULT_MAX_DEPTH;
    d->insn_budget = ASMTEST_DESCENT_DEFAULT_INSN_BUDGET;
    d->watchdog_ms = ASMTEST_DESCENT_DEFAULT_WATCHDOG_MS;
    return d;
}

void asmtest_descent_free(asmtest_descent_t *d) {
    if (d == NULL)
        return;
    for (size_t i = 0; i < d->frames_len; i++) {
        free(d->frames[i].insns);
        free(d->frames[i].blocks);
    }
    free(d->frames);
    free(d->edges);
    free(d->allow);
    free(d->deny);
    /* Zero the struct before freeing it (defensive: a stray read of the freed handle sees
     * NULLs, not dangling pool pointers). This does NOT make a second free of the same
     * pointer safe — that is still a double free; the caller (or the binding wrapper) must
     * drop its reference. NULL-safe entry is handled at the top. */
    memset(d, 0, sizeof *d);
    free(d);
}

void asmtest_descent_set_max_depth(asmtest_descent_t *d, uint32_t max_depth) {
    if (d != NULL)
        d->max_depth =
            max_depth ? max_depth : ASMTEST_DESCENT_DEFAULT_MAX_DEPTH;
}

void asmtest_descent_set_insn_budget(asmtest_descent_t *d, uint64_t budget) {
    if (d != NULL)
        d->insn_budget = budget ? budget : ASMTEST_DESCENT_DEFAULT_INSN_BUDGET;
}

void asmtest_descent_set_watchdog_ms(asmtest_descent_t *d, uint32_t ms) {
    if (d != NULL)
        d->watchdog_ms = ms ? ms : ASMTEST_DESCENT_DEFAULT_WATCHDOG_MS;
}

static int region_add(asmtest_descent_region_t **arr, size_t *len, size_t *cap,
                      const void *base, size_t rlen) {
    if (!pool_reserve((void **)arr, cap, *len + 1, sizeof **arr))
        return ASMTEST_PTRACE_ETRACE;
    (*arr)[*len].base = (uint64_t)(uintptr_t)base;
    (*arr)[*len].len = (uint64_t)rlen;
    (*len)++;
    return ASMTEST_PTRACE_OK;
}

int asmtest_descent_allow_region(asmtest_descent_t *d, const void *base,
                                 size_t len) {
    if (d == NULL)
        return ASMTEST_PTRACE_EINVAL;
    return region_add(&d->allow, &d->allow_len, &d->allow_cap, base, len);
}

int asmtest_descent_deny_region(asmtest_descent_t *d, const void *base,
                                size_t len) {
    if (d == NULL)
        return ASMTEST_PTRACE_EINVAL;
    return region_add(&d->deny, &d->deny_len, &d->deny_cap, base, len);
}

void asmtest_descent_set_resolver(asmtest_descent_t *d,
                                  asmtest_descent_resolver_fn fn, void *user) {
    if (d != NULL) {
        d->resolver = fn;
        d->resolver_user = user;
    }
}

void asmtest_descent_set_denylist(asmtest_descent_t *d,
                                  asmtest_descent_denylist_fn fn, void *user) {
    if (d != NULL) {
        d->denylist = fn;
        d->denylist_user = user;
    }
}

void asmtest_descent_use_default_denylist(asmtest_descent_t *d) {
    /* Just arms the flag: the regions can only be resolved once the tracee is
     * known, so the ptrace entry points populate the deny pool at trace start
     * (see descend_apply_default_denylist in src/ptrace_backend.c). */
    if (d != NULL)
        d->use_default_denylist = 1;
}

/* ------------------------------------------------------------------ */
/* Mutators driven by the step loop (asmtest_descent_internal.h)       */
/* ------------------------------------------------------------------ */

int asmtest_descent_region_contains(const asmtest_descent_region_t *arr,
                                    size_t n, uint64_t addr, uint64_t *base_out,
                                    uint64_t *len_out) {
    for (size_t i = 0; i < n; i++)
        if (addr >= arr[i].base && addr < arr[i].base + arr[i].len) {
            if (base_out)
                *base_out = arr[i].base;
            if (len_out)
                *len_out = arr[i].len;
            return 1;
        }
    return 0;
}

int32_t asmtest_descent_push_frame(asmtest_descent_t *d, uint64_t base,
                                   uint64_t len, uint32_t depth,
                                   int32_t parent) {
    if (d == NULL)
        return -1;
    if (!pool_reserve((void **)&d->frames, &d->frames_cap, d->frames_len + 1,
                      sizeof *d->frames)) {
        d->truncated = 1;
        return -1;
    }
    asmtest_descent_frame_t *f = &d->frames[d->frames_len];
    memset(f, 0, sizeof *f);
    f->base = base;
    f->len = len;
    f->depth = depth;
    f->parent = parent;
    return (int32_t)d->frames_len++;
}

int asmtest_descent_frame_record(asmtest_descent_t *d, int32_t fi, uint64_t off,
                                 size_t insn_len) {
    if (d == NULL || fi < 0 || (size_t)fi >= d->frames_len)
        return 0;
    asmtest_descent_frame_t *f = &d->frames[fi];
    int new_block = (!f->have_prev || off != f->expected_next);
    if (new_block) {
        /* Dedup against the frame's distinct block set (linear; block counts are small). */
        int seen = 0;
        for (size_t i = 0; i < f->blocks_len; i++)
            if (f->blocks[i] == off) {
                seen = 1;
                break;
            }
        if (!seen) {
            if (pool_reserve((void **)&f->blocks, &f->blocks_cap,
                             f->blocks_len + 1, sizeof *f->blocks))
                f->blocks[f->blocks_len++] = off;
            else
                d->truncated = 1;
        }
    }
    if (pool_reserve((void **)&f->insns, &f->insns_cap, f->insns_len + 1,
                     sizeof *f->insns))
        f->insns[f->insns_len++] = off;
    else
        d->truncated = 1;
    if (insn_len == 0) {
        d->truncated =
            1; /* undecodable byte: cannot derive the next boundary */
        f->have_prev = 0;
    } else {
        f->expected_next = off + insn_len;
        f->have_prev = 1;
    }
    return new_block;
}

void asmtest_descent_add_edge(asmtest_descent_t *d, uint64_t site,
                              uint64_t target, uint32_t depth,
                              int32_t from_frame) {
    if (d == NULL)
        return;
    if (!pool_reserve((void **)&d->edges, &d->edges_cap, d->edges_len + 1,
                      sizeof *d->edges)) {
        d->truncated = 1;
        return;
    }
    asmtest_descent_edge_t *e = &d->edges[d->edges_len++];
    e->site = site;
    e->target = target;
    e->depth = depth;
    e->from_frame = from_frame;
}

void asmtest_descent_mark_truncated(asmtest_descent_t *d) {
    if (d != NULL)
        d->truncated = 1;
}

void asmtest_descent_mark_depth_capped(asmtest_descent_t *d) {
    if (d != NULL)
        d->depth_capped = 1;
}

/* ------------------------------------------------------------------ */
/* Read accessors (opaque-handle FFI idiom; NULL-safe, bounds-checked) */
/* ------------------------------------------------------------------ */

size_t asmtest_descent_edges_len(const asmtest_descent_t *d) {
    return d != NULL ? d->edges_len : 0;
}
uint64_t asmtest_descent_edge_site(const asmtest_descent_t *d, size_t i) {
    return (d != NULL && i < d->edges_len) ? d->edges[i].site : 0;
}
uint64_t asmtest_descent_edge_target(const asmtest_descent_t *d, size_t i) {
    return (d != NULL && i < d->edges_len) ? d->edges[i].target : 0;
}
uint32_t asmtest_descent_edge_depth(const asmtest_descent_t *d, size_t i) {
    return (d != NULL && i < d->edges_len) ? d->edges[i].depth : 0;
}
size_t asmtest_descent_frames_len(const asmtest_descent_t *d) {
    return d != NULL ? d->frames_len : 0;
}
uint64_t asmtest_descent_frame_base(const asmtest_descent_t *d, size_t f) {
    return (d != NULL && f < d->frames_len) ? d->frames[f].base : 0;
}
uint64_t asmtest_descent_frame_len(const asmtest_descent_t *d, size_t f) {
    return (d != NULL && f < d->frames_len) ? d->frames[f].len : 0;
}
uint32_t asmtest_descent_frame_depth(const asmtest_descent_t *d, size_t f) {
    return (d != NULL && f < d->frames_len) ? d->frames[f].depth : 0;
}
int32_t asmtest_descent_frame_parent(const asmtest_descent_t *d, size_t f) {
    return (d != NULL && f < d->frames_len) ? d->frames[f].parent : -1;
}
size_t asmtest_descent_frame_insn_count(const asmtest_descent_t *d, size_t f) {
    return (d != NULL && f < d->frames_len) ? d->frames[f].insns_len : 0;
}
uint64_t asmtest_descent_frame_insn_at(const asmtest_descent_t *d, size_t f,
                                       size_t i) {
    return (d != NULL && f < d->frames_len && i < d->frames[f].insns_len)
               ? d->frames[f].insns[i]
               : 0;
}
size_t asmtest_descent_frame_block_count(const asmtest_descent_t *d, size_t f) {
    return (d != NULL && f < d->frames_len) ? d->frames[f].blocks_len : 0;
}
uint64_t asmtest_descent_frame_block_at(const asmtest_descent_t *d, size_t f,
                                        size_t i) {
    return (d != NULL && f < d->frames_len && i < d->frames[f].blocks_len)
               ? d->frames[f].blocks[i]
               : 0;
}
int asmtest_descent_truncated(const asmtest_descent_t *d) {
    return (d != NULL && d->truncated) ? 1 : 0;
}
int asmtest_descent_depth_capped(const asmtest_descent_t *d) {
    return (d != NULL && d->depth_capped) ? 1 : 0;
}
