/*
 * dataflow.c — the PURE data-flow substrate: the L0 value-trace sink
 * (asmtest_valtrace_*), the L1 last-writer def-use builder (asmtest_defuse_*), and
 * the L2 forward/backward slicer (asmtest_slice_*). See asmtest_valtrace.h.
 *
 * This translation unit has NO Capstone and NO Unicorn dependency — it is the
 * analysis spine every tier shares, exactly as trace.c is the control-trace spine.
 * The operand read/write-set enumerator (the one Capstone-dependent piece) lives
 * apart in dataflow_operands.c so this file compiles and unit-tests on every host.
 */
#include "asmtest_valtrace.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* L0 — the value-trace sink                                           */
/* ------------------------------------------------------------------ */

asmtest_valtrace_t *asmtest_valtrace_new(size_t steps_cap, size_t recs_cap,
                                         size_t wide_cap) {
    asmtest_valtrace_t *v = (asmtest_valtrace_t *)calloc(1, sizeof *v);
    if (v == NULL)
        return NULL;
    if (steps_cap) {
        v->insn_off = (uint64_t *)calloc(steps_cap, sizeof(uint64_t));
        if (v->insn_off)
            v->steps_cap = steps_cap;
    }
    if (recs_cap) {
        v->recs = (at_val_rec_t *)calloc(recs_cap, sizeof(at_val_rec_t));
        if (v->recs)
            v->recs_cap = recs_cap;
    }
    if (wide_cap) {
        v->wide = (uint8_t *)calloc(wide_cap, 1);
        if (v->wide)
            v->wide_cap = wide_cap;
    }
    v->mem_space = AT_LOC_MEM_ABS;
    return v;
}

void asmtest_valtrace_free(asmtest_valtrace_t *v) {
    if (v == NULL)
        return;
    free(v->insn_off);
    free(v->recs);
    free(v->wide);
    free(v);
}

void asmtest_valtrace_append(asmtest_valtrace_t *v, uint64_t off,
                             const at_val_rec_t *recs, size_t n) {
    if (v == NULL)
        return;
    v->steps_total++;
    v->recs_total += n;
    /* No room to record this step's offset: drop the step (and its records) but
     * keep the totals honest (mirrors trace_append_insn's truncate discipline). */
    if (!(v->insn_off != NULL && v->steps_len < v->steps_cap)) {
        v->truncated = true;
        return;
    }
    uint32_t step_idx = (uint32_t)v->steps_len;
    v->insn_off[v->steps_len++] = off;
    for (size_t i = 0; i < n; i++) {
        if (v->recs != NULL && v->recs_len < v->recs_cap) {
            at_val_rec_t r = recs[i];
            r.step = step_idx;
            v->recs[v->recs_len++] = r;
        } else {
            v->truncated = true;
        }
    }
}

size_t asmtest_valtrace_stash_wide(asmtest_valtrace_t *v, const void *bytes,
                                   size_t n) {
    if (v == NULL || n == 0)
        return (size_t)-1;
    if (v->wide == NULL || v->wide_len + n > v->wide_cap) {
        v->truncated = true;
        return (size_t)-1;
    }
    size_t off = v->wide_len;
    memcpy(v->wide + off, bytes, n);
    v->wide_len += n;
    return off;
}

size_t asmtest_valtrace_steps(const asmtest_valtrace_t *v) {
    return v != NULL ? v->steps_len : 0;
}
size_t asmtest_valtrace_recs(const asmtest_valtrace_t *v) {
    return v != NULL ? v->recs_len : 0;
}

/* ------------------------------------------------------------------ */
/* L1 — last-writer map (open-addressing hash)                         */
/*                                                                     */
/* Keyed on (kind, a): kind 0 = register (a = reg id), kind 1 = memory  */
/* byte (a = absolute/normalized byte address). Memory is tracked per   */
/* byte so a partial overlap resolves to the right writer; the small    */
/* per-access byte count (<= 64) keeps this cheap.                      */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t used;
    uint8_t kind; /* 0 = reg, 1 = mem-byte */
    uint64_t a;   /* reg id or byte address */
    uint32_t step;
} lw_ent;

typedef struct {
    lw_ent *e;
    size_t cap;
    size_t cnt;
} lw_map;

static uint64_t lw_hash(uint8_t kind, uint64_t a) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull + kind;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}

static bool lw_grow(lw_map *m, size_t want) {
    size_t ncap = m->cap ? m->cap * 2 : 64;
    while (ncap < want * 2)
        ncap *= 2;
    lw_ent *ne = (lw_ent *)calloc(ncap, sizeof *ne);
    if (ne == NULL)
        return false;
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->e[i].used)
            continue;
        size_t j = (size_t)(lw_hash(m->e[i].kind, m->e[i].a) & (ncap - 1));
        while (ne[j].used)
            j = (j + 1) & (ncap - 1);
        ne[j] = m->e[i];
    }
    free(m->e);
    m->e = ne;
    m->cap = ncap;
    return true;
}

static void lw_put(lw_map *m, uint8_t kind, uint64_t a, uint32_t step) {
    if (m->cap == 0 || (m->cnt + 1) * 4 >= m->cap * 3) {
        if (!lw_grow(m, m->cnt + 1))
            return; /* OOM: silently skip; the edge just won't be recorded */
    }
    size_t j = (size_t)(lw_hash(kind, a) & (m->cap - 1));
    while (m->e[j].used) {
        if (m->e[j].kind == kind && m->e[j].a == a) {
            m->e[j].step = step;
            return;
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->e[j].used = 1;
    m->e[j].kind = kind;
    m->e[j].a = a;
    m->e[j].step = step;
    m->cnt++;
}

static bool lw_get(const lw_map *m, uint8_t kind, uint64_t a, uint32_t *step) {
    if (m->cap == 0)
        return false;
    size_t j = (size_t)(lw_hash(kind, a) & (m->cap - 1));
    while (m->e[j].used) {
        if (m->e[j].kind == kind && m->e[j].a == a) {
            *step = m->e[j].step;
            return true;
        }
        j = (j + 1) & (m->cap - 1);
    }
    return false;
}

static void lw_free(lw_map *m) { free(m->e); }

/* Growable edge vector. */
typedef struct {
    asmtest_defuse_edge_t *v;
    size_t n, cap;
} edgevec;

static void edge_push(edgevec *ev, uint32_t from, uint32_t to,
                      const at_val_rec_t *loc) {
    if (ev->n == ev->cap) {
        size_t nc = ev->cap ? ev->cap * 2 : 32;
        asmtest_defuse_edge_t *nv =
            (asmtest_defuse_edge_t *)realloc(ev->v, nc * sizeof *nv);
        if (nv == NULL)
            return; /* OOM: drop the edge rather than crash */
        ev->v = nv;
        ev->cap = nc;
    }
    ev->v[ev->n].from_step = from;
    ev->v[ev->n].to_step = to;
    ev->v[ev->n].loc = *loc;
    ev->n++;
}

/* Clamp a byte width to the tracked window (guards a bogus/huge size). */
static uint16_t rec_bytes(const at_val_rec_t *r) {
    uint16_t s = r->size ? r->size : 1;
    return s > 64 ? 64 : s;
}

static void emit_read(const at_val_rec_t *rec, const lw_map *map, edgevec *ev,
                      uint32_t step) {
    if (rec->kind == AT_LOC_REG) {
        uint32_t p;
        if (lw_get(map, 0, rec->reg, &p))
            edge_push(ev, p, step, rec);
        return;
    }
    /* memory: one edge per DISTINCT producing step across the touched bytes */
    uint32_t prod[64];
    size_t np = 0;
    uint16_t sz = rec_bytes(rec);
    for (uint16_t b = 0; b < sz; b++) {
        uint32_t p;
        if (!lw_get(map, 1, rec->addr + b, &p))
            continue;
        bool seen = false;
        for (size_t j = 0; j < np; j++)
            if (prod[j] == p) {
                seen = true;
                break;
            }
        if (!seen && np < 64)
            prod[np++] = p;
    }
    for (size_t j = 0; j < np; j++)
        edge_push(ev, prod[j], step, rec);
}

static void apply_write(const at_val_rec_t *rec, lw_map *map, uint32_t step) {
    if (rec->kind == AT_LOC_REG) {
        lw_put(map, 0, rec->reg, step);
        return;
    }
    uint16_t sz = rec_bytes(rec);
    for (uint16_t b = 0; b < sz; b++)
        lw_put(map, 1, rec->addr + b, step);
}

asmtest_defuse_t *asmtest_defuse_build(const asmtest_valtrace_t *v) {
    if (v == NULL)
        return NULL;
    asmtest_defuse_t *g = (asmtest_defuse_t *)calloc(1, sizeof *g);
    if (g == NULL)
        return NULL;
    g->nsteps = v->steps_len;

    lw_map map = {0};
    edgevec ev = {0};

    /* Records are appended grouped and in ascending step order (the append
     * contract), so walk them step by step: read-set first (query the map),
     * then write-set (update it) — a same-instruction write must not shadow the
     * same instruction's reads. */
    size_t i = 0, n = v->recs_len;
    for (uint32_t step = 0; step < (uint32_t)v->steps_len; step++) {
        size_t start = i;
        while (i < n && v->recs[i].step == step)
            i++;
        size_t end = i;
        for (size_t r = start; r < end; r++)
            if (!v->recs[r].is_write)
                emit_read(&v->recs[r], &map, &ev, step);
        for (size_t r = start; r < end; r++)
            if (v->recs[r].is_write)
                apply_write(&v->recs[r], &map, step);
    }

    lw_free(&map);
    g->edges = ev.v;
    g->n = ev.n;
    return g;
}

void asmtest_defuse_free(asmtest_defuse_t *g) {
    if (g == NULL)
        return;
    free(g->edges);
    free(g);
}

/* ------------------------------------------------------------------ */
/* L2 — forward / backward slice                                       */
/* ------------------------------------------------------------------ */

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static asmtest_slice_t *slice_dir(const asmtest_defuse_t *g, uint32_t origin,
                                  bool forward) {
    asmtest_slice_t *s = (asmtest_slice_t *)calloc(1, sizeof *s);
    if (s == NULL)
        return NULL;
    if (g == NULL)
        return s;
    size_t ns = g->nsteps;
    if (origin >= ns)
        return s; /* origin outside the trace: empty slice */

    char *vis = (char *)calloc(ns ? ns : 1, 1);
    uint32_t *q = (uint32_t *)malloc((ns ? ns : 1) * sizeof *q);
    if (vis == NULL || q == NULL) {
        free(vis);
        free(q);
        return s;
    }
    size_t qh = 0, qt = 0;
    vis[origin] = 1;
    q[qt++] = origin;
    while (qh < qt) {
        uint32_t u = q[qh++];
        for (size_t e = 0; e < g->n; e++) {
            uint32_t a = forward ? g->edges[e].from_step : g->edges[e].to_step;
            uint32_t b = forward ? g->edges[e].to_step : g->edges[e].from_step;
            if (a == u && b < ns && !vis[b]) {
                vis[b] = 1;
                q[qt++] = b;
            }
        }
    }

    size_t cnt = 0;
    for (size_t k = 0; k < ns; k++)
        if (vis[k])
            cnt++;
    s->steps = (uint32_t *)malloc((cnt ? cnt : 1) * sizeof(uint32_t));
    if (s->steps != NULL) {
        size_t w = 0;
        for (uint32_t k = 0; k < ns; k++)
            if (vis[k])
                s->steps[w++] = k;
        s->n = w;
        qsort(s->steps, s->n, sizeof(uint32_t), cmp_u32);
    }
    free(vis);
    free(q);
    return s;
}

asmtest_slice_t *asmtest_slice_forward(const asmtest_defuse_t *g,
                                       at_val_rec_t seed) {
    return slice_dir(g, seed.step, true);
}

asmtest_slice_t *asmtest_slice_backward(const asmtest_defuse_t *g,
                                        at_val_rec_t sink) {
    return slice_dir(g, sink.step, false);
}

void asmtest_slice_free(asmtest_slice_t *s) {
    if (s == NULL)
        return;
    free(s->steps);
    free(s);
}

int asmtest_slice_contains(const asmtest_slice_t *s, uint32_t step) {
    if (s == NULL || s->steps == NULL)
        return 0;
    for (size_t i = 0; i < s->n; i++)
        if (s->steps[i] == step)
            return 1;
    return 0;
}
