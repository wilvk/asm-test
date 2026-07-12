/*
 * dataflow_helpers.c — Phase 4 (increment 3): runtime-helper SUMMARY EDGES over
 * an L0 value trace. See asmtest_valtrace.h.
 *
 * A managed value trace steps THROUGH the CoreCLR runtime helpers the JIT emits
 * for language-level operations (allocation for `new`, the write-barrier for a
 * reference-field store, the generic-dictionary lookup for a shared-generic
 * handle). Those helper bodies are ordinary instrumented blocks, so a raw def-use
 * (asmtest_defuse_build) threads the caller's flow through the helper's internal
 * churn — scratch registers, card-table math, slow-path branches — which is noisy
 * and CoreCLR-version-specific. This pass models a RECOGNIZED helper as a SUMMARY:
 * at its entry step it emits only the helper's declared INPUT reads and OUTPUT
 * writes (its data-flow contract) and drops the body's records, so a def-use built
 * over the rewritten stream connects the caller ACROSS the helper (arg def ->
 * summary node -> return use) without descending into CoreCLR internals. An
 * UNRECOGNIZED call is descended normally, so no summary edge is ever invented.
 *
 * The transform rewrites the record stream and hands it to the SHARED, already-
 * tested asmtest_defuse_build — no duplicated last-writer logic — and reuses the
 * increment-1 asmtest_method_resolve_pc for identification. PURE C (no Capstone,
 * no Unicorn, no runtime), the same tier as dataflow.c, so it unit-tests on every
 * host. Helper identification from a LIVE runtime symbolizer is a producer concern;
 * this is the pure model it drives.
 */
#include "asmtest_valtrace.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Helper-table matching + the built-in representative table           */
/* ------------------------------------------------------------------ */

int asmtest_helper_match(const asmtest_helper_t *helpers, size_t nhelpers,
                         const char *name) {
    if (helpers == NULL || name == NULL)
        return -1;
    for (size_t i = 0; i < nhelpers; i++) {
        const char *pat = helpers[i].name;
        if (pat == NULL)
            continue;
        size_t plen = strlen(pat);
        if (plen > 0 && pat[plen - 1] == '*') {
            /* trailing '*' -> prefix match on the first (plen-1) chars */
            if (strncmp(name, pat, plen - 1) == 0)
                return (int)i;
        } else if (strcmp(name, pat) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Register ids for the built-in table mirror the x86-64 SysV convention as
 * Capstone x86_reg enum values, kept as literals so this PURE tier needs no
 * Capstone header: RDI/RSI are the first two integer args, RAX the return. */
enum {
    HLP_X86_RAX = 35,
    HLP_X86_RDI = 39,
    HLP_X86_RSI = 43,
};

/* Allocation helper: MethodTable/size in the first arg -> new object ref in RAX. */
static const asmtest_helper_loc_t ALLOC_IN[] = {
    {AT_HELPER_REG, HLP_X86_RDI, 0}};
static const asmtest_helper_loc_t ALLOC_OUT[] = {
    {AT_HELPER_REG, HLP_X86_RAX, 0}};

/* Write-barrier (CORINFO_HELP_ASSIGN_REF / JIT_WriteBarrier): the reference value
 * in the 2nd arg is stored to the destination field pointed at by the 1st arg —
 * so the value flows to memory at [RDI], the (object, field) slot. */
static const asmtest_helper_loc_t WB_IN[] = {{AT_HELPER_REG, HLP_X86_RSI, 0}};
static const asmtest_helper_loc_t WB_OUT[] = {
    {AT_HELPER_MEM_AT_REG, HLP_X86_RDI, 8}};

/* Generic-dictionary / runtime-handle lookup: the context in the 1st arg ->
 * the resolved type/method handle in RAX. */
static const asmtest_helper_loc_t GDICT_IN[] = {
    {AT_HELPER_REG, HLP_X86_RDI, 0}};
static const asmtest_helper_loc_t GDICT_OUT[] = {
    {AT_HELPER_REG, HLP_X86_RAX, 0}};

/* Names cover both the CORINFO_HELP_* enum spellings and the JIT_* runtime symbol
 * spellings a symbolizer may surface; trailing '*' matches the many suffixed
 * variants (NEWSFAST / NEWFAST / NEWARR_1 / ...). Representative, not exhaustive. */
static const asmtest_helper_t DEFAULT_HELPERS[] = {
    {"CORINFO_HELP_NEW*", ALLOC_IN, 1, ALLOC_OUT, 1},
    {"JIT_New*", ALLOC_IN, 1, ALLOC_OUT, 1},
    {"CORINFO_HELP_ASSIGN_REF*", WB_IN, 1, WB_OUT, 1},
    {"JIT_WriteBarrier*", WB_IN, 1, WB_OUT, 1},
    {"CORINFO_HELP_RUNTIMEHANDLE*", GDICT_IN, 1, GDICT_OUT, 1},
    {"JIT_GenericHandle*", GDICT_IN, 1, GDICT_OUT, 1},
};

const asmtest_helper_t *asmtest_helper_default_table(size_t *n_out) {
    if (n_out != NULL)
        *n_out = sizeof DEFAULT_HELPERS / sizeof DEFAULT_HELPERS[0];
    return DEFAULT_HELPERS;
}

/* ------------------------------------------------------------------ */
/* reg -> last captured value (for MEM_AT_REG address resolution)      */
/*                                                                     */
/* A small open-addressing map, populated ONLY from the caller's real  */
/* register writes (a record with value_valid) as the trace is walked  */
/* in order, so at a helper run's entry it holds the pointer register's */
/* value as the caller left it just before the call.                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t used;
    uint32_t reg;
    uint64_t val;
} rv_ent;

typedef struct {
    rv_ent *e;
    size_t cap;
    size_t cnt;
} rv_map;

static uint64_t rv_hash(uint32_t reg) {
    uint64_t h = (uint64_t)reg * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

static bool rv_grow(rv_map *m) {
    size_t ncap = m->cap ? m->cap * 2 : 32;
    rv_ent *ne = (rv_ent *)calloc(ncap, sizeof *ne);
    if (ne == NULL)
        return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->e[i].used)
            continue;
        size_t j = (size_t)(rv_hash(m->e[i].reg) & (ncap - 1));
        while (ne[j].used)
            j = (j + 1) & (ncap - 1);
        ne[j] = m->e[i];
    }
    free(m->e);
    m->e = ne;
    m->cap = ncap;
    return true;
}

static void rv_put(rv_map *m, uint32_t reg, uint64_t val) {
    if (m->cap == 0 || (m->cnt + 1) * 4 >= m->cap * 3) {
        if (!rv_grow(m))
            return; /* OOM: MEM_AT_REG for this reg just resolves to "unknown" */
    }
    size_t j = (size_t)(rv_hash(reg) & (m->cap - 1));
    while (m->e[j].used) {
        if (m->e[j].reg == reg) {
            m->e[j].val = val;
            return;
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->e[j].used = 1;
    m->e[j].reg = reg;
    m->e[j].val = val;
    m->cnt++;
}

static bool rv_get(const rv_map *m, uint32_t reg, uint64_t *val) {
    if (m->cap == 0)
        return false;
    size_t j = (size_t)(rv_hash(reg) & (m->cap - 1));
    while (m->e[j].used) {
        if (m->e[j].reg == reg) {
            *val = m->e[j].val;
            return true;
        }
        j = (j + 1) & (m->cap - 1);
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Rewritten-record vector                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    at_val_rec_t *v;
    size_t n, cap;
} recvec;

static bool rec_push(recvec *rv, at_val_rec_t r) {
    if (rv->n == rv->cap) {
        size_t nc = rv->cap ? rv->cap * 2 : 64;
        at_val_rec_t *nv = (at_val_rec_t *)realloc(rv->v, nc * sizeof *nv);
        if (nv == NULL)
            return false;
        rv->v = nv;
        rv->cap = nc;
    }
    rv->v[rv->n++] = r;
    return true;
}

/* Emit one summary record (a read or a write) for a helper location, stamped with
 * the summary step. A MEM_AT_REG location resolves its absolute address from the
 * pointer register's captured value; an unknown pointer value skips the location
 * (conservative — no fabricated edge). Returns false only on OOM. */
static bool emit_helper_loc(recvec *out, const asmtest_helper_loc_t *loc,
                            bool is_write, uint32_t step,
                            const rv_map *regvals) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.step = step;
    r.is_write = is_write;
    if (loc->kind == AT_HELPER_REG) {
        r.kind = AT_LOC_REG;
        r.reg = loc->reg;
        return rec_push(out, r);
    }
    /* AT_HELPER_MEM_AT_REG: resolve [reg] from the pointer's captured value. */
    uint64_t base;
    if (!rv_get(regvals, loc->reg, &base))
        return true; /* pointer value unknown -> skip, but not an error */
    r.kind = AT_LOC_MEM_ABS;
    r.addr = base;
    r.size = loc->size ? loc->size : 8;
    return rec_push(out, r);
}

/* ------------------------------------------------------------------ */
/* The summarizing def-use builder                                     */
/* ------------------------------------------------------------------ */

asmtest_defuse_t *asmtest_defuse_build_summarized(
    const asmtest_valtrace_t *v, const asmtest_method_t *methods,
    size_t nmethods, const asmtest_helper_t *helpers, size_t nhelpers) {
    if (v == NULL)
        return NULL;
    /* Nothing to recognize with -> the plain last-writer graph. */
    if (helpers == NULL || nhelpers == 0 || methods == NULL || nmethods == 0)
        return asmtest_defuse_build(v);

    size_t nsteps = v->steps_len;

    /* Per-step helper-entry index (or -1): resolve the PC to a method record,
     * then match its name against the helper table. */
    int *hi = NULL;
    if (nsteps > 0) {
        hi = (int *)malloc(nsteps * sizeof *hi);
        if (hi == NULL)
            return asmtest_defuse_build(v); /* degrade: no summarization */
        for (size_t s = 0; s < nsteps; s++) {
            uint64_t pc = v->insn_off != NULL ? v->insn_off[s] : 0;
            int rec = asmtest_method_resolve_pc(methods, nmethods, pc);
            hi[s] = (rec >= 0) ? asmtest_helper_match(helpers, nhelpers,
                                                      methods[rec].name)
                               : -1;
        }
    }

    /* Per-step record boundaries: recs are grouped and ascending by step (the
     * append contract), so one linear pass gives each step's [start,next) range. */
    size_t *rstart = (size_t *)malloc((nsteps + 1) * sizeof *rstart);
    if (rstart == NULL) {
        free(hi);
        return asmtest_defuse_build(v);
    }
    {
        size_t ci = 0;
        for (size_t s = 0; s <= nsteps; s++) {
            while (ci < v->recs_len && v->recs[ci].step < s)
                ci++;
            rstart[s] = ci;
        }
    }

    recvec out = {0};
    rv_map regvals = {0};
    bool ok = true;

    size_t s = 0;
    while (s < nsteps && ok) {
        int h = hi[s];
        if (h < 0) {
            /* Ordinary step: copy its records verbatim and track reg values so a
             * later helper's MEM_AT_REG pointer can be resolved. */
            for (size_t r = rstart[s]; r < rstart[s + 1]; r++) {
                if (!rec_push(&out, v->recs[r])) {
                    ok = false;
                    break;
                }
                const at_val_rec_t *rr = &v->recs[r];
                if (rr->kind == AT_LOC_REG && rr->is_write && rr->value_valid)
                    rv_put(&regvals, rr->reg, rr->value);
            }
            s++;
        } else {
            /* Helper run [s..b]: the maximal following stretch resolving to the
             * SAME helper entry. Summarize it as the helper's input reads +
             * output writes at step s; drop every record inside the run. (Nested
             * or back-to-back-distinct helper calls are not specially merged —
             * each maximal same-entry run is one summary.) */
            size_t b = s;
            while (b + 1 < nsteps && hi[b + 1] == h)
                b++;
            const asmtest_helper_t *H = &helpers[h];
            for (size_t k = 0; k < H->n_in && ok; k++)
                ok = emit_helper_loc(&out, &H->ins[k], false, (uint32_t)s,
                                     &regvals);
            for (size_t k = 0; k < H->n_out && ok; k++)
                ok = emit_helper_loc(&out, &H->outs[k], true, (uint32_t)s,
                                     &regvals);
            s = b + 1;
        }
    }

    free(hi);
    free(rstart);
    free(regvals.e);

    if (!ok) { /* OOM mid-rewrite: degrade to the un-summarized graph. */
        free(out.v);
        return asmtest_defuse_build(v);
    }

    /* Run the SHARED last-writer builder over the rewritten record stream. The
     * shim borrows v's per-step offsets and step count (unchanged) and points at
     * the rewritten records — every field asmtest_defuse_build reads. */
    asmtest_valtrace_t shim;
    memset(&shim, 0, sizeof shim);
    shim.insn_off = v->insn_off;
    shim.steps_cap = nsteps;
    shim.steps_len = nsteps;
    shim.steps_total = nsteps;
    shim.recs = out.v;
    shim.recs_cap = out.n;
    shim.recs_len = out.n;
    shim.recs_total = out.n;
    shim.mem_space = v->mem_space;

    asmtest_defuse_t *g = asmtest_defuse_build(&shim);
    free(out.v);
    return g;
}
