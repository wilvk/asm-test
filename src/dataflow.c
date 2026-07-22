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

#include "asmtest_grow.h" /* asmtest_grow / _pow2 — overflow-checked pool growth (S6) */
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
/* Keyed on (kind, a): kind 0 = register, raw id (a = Capstone reg id,  */
/* for a register `reg_slice` cannot canonicalize), kind 1 = memory     */
/* byte (a = absolute/normalized byte address), kind 2 = register,      */
/* CONTAINER byte (a = (container id << 8) | byte offset, for a GP      */
/* sub-register alias `reg_slice` resolves — see T3 below). Memory and   */
/* kind-2 registers are tracked per byte so a partial overlap resolves   */
/* to the right writer; the small per-access byte count (<= 64 mem, <= 8 */
/* register) keeps this cheap. The three kinds never collide because     */
/* `kind` is folded into both the hash and the equality check, not       */
/* because the `a` spaces are disjoint on their own.                     */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t used;
    uint8_t kind; /* 0 = reg (raw id), 1 = mem-byte, 2 = reg (container byte) */
    uint64_t a;   /* reg id, byte address, or (container << 8) | byte offset */
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
    /* Overflow-checked power-of-two growth (S6): preserves the original result
     * (smallest pow2 >= max(cap*2, want*2, 64)) while failing closed if want*2
     * or the doubling would wrap size_t. */
    size_t ncap;
    if (!asmtest_grow_pow2(m->cap, (m->cap ? m->cap : 64), sizeof(lw_ent),
                           &ncap) ||
        (want > SIZE_MAX / 2)) /* want * 2 must not wrap */
        return false;
    while (ncap < want * 2) {
        size_t g;
        if (!asmtest_grow_pow2(ncap, ncap + 1, sizeof(lw_ent), &g))
            return false;
        ncap = g;
    }
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
    if (ev->n == ev->cap &&
        !asmtest_grow((void **)&ev->v, &ev->cap, ev->n + 1, sizeof *ev->v))
        return; /* OOM: drop the edge rather than crash */
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

/* ------------------------------------------------------------------ */
/* T3 — sub-register-aware last-writer resolution                      */
/*                                                                     */
/* A Capstone x86 register alias (rax/eax/ax/al/ah, ..., r8/r8d/r8w/r8b)
 * is a set of otherwise-unrelated integer ids with no structural link to
 * the 64-bit container they fold into — exactly the problem
 * `dfp_alias_shape` (src/dataflow_ptrace.c) solves for the ptrace gap
 * barrier's bit-slice compare. `reg_slice` below is the shared builder's
 * MIRROR of that function: same id grouping, same underlying shift/width
 * facts, reshaped into (container id, byte offset, byte length) because
 * the last-writer map keys per BYTE (see the memory axis above), not per
 * bit-shift. A divergence between the two — either in which ids map, or
 * in the offset/width they map to — is itself a bug; keep them in
 * lockstep. In particular AH/BH/CH/DH must stay at byte offset 1 of their
 * container (not offset 0, which is AL/BL/CL/DL's own byte): a write to
 * `ah` must reach a later `ah`/`ax`/`eax`/`rax` read but NOT a later `al`
 * read, or the def-use graph fabricates an edge no instruction created.
 *
 * This file is deliberately Capstone-free (see the file banner and
 * asmtest_valtrace.h) so the ids below are literal mirrors of the
 * capstone/x86.h `x86_reg` enum (pinned Capstone 5.0.1, per CLAUDE.md's
 * dependency rule) rather than an #include — the same tactic
 * examples/test_dataflow_ptrace.c already uses for its own REG_* literals,
 * for the same reason (this translation unit must keep compiling on a host
 * with no Capstone installed). */
#define X86_REG_AH   1
#define X86_REG_AL   2
#define X86_REG_AX   3
#define X86_REG_BH   4
#define X86_REG_BL   5
#define X86_REG_BP   6
#define X86_REG_BPL  7
#define X86_REG_BX   8
#define X86_REG_CH   9
#define X86_REG_CL   10
#define X86_REG_CX   12
#define X86_REG_DH   13
#define X86_REG_DI   14
#define X86_REG_DIL  15
#define X86_REG_DL   16
#define X86_REG_DX   18
#define X86_REG_EAX  19
#define X86_REG_EBP  20
#define X86_REG_EBX  21
#define X86_REG_ECX  22
#define X86_REG_EDI  23
#define X86_REG_EDX  24
#define X86_REG_ESI  29
#define X86_REG_ESP  30
#define X86_REG_RAX  35
#define X86_REG_RBP  36
#define X86_REG_RBX  37
#define X86_REG_RCX  38
#define X86_REG_RDI  39
#define X86_REG_RDX  40
#define X86_REG_RSI  43
#define X86_REG_RSP  44
#define X86_REG_SI   45
#define X86_REG_SIL  46
#define X86_REG_SP   47
#define X86_REG_SPL  48
#define X86_REG_R8   106
#define X86_REG_R9   107
#define X86_REG_R10  108
#define X86_REG_R11  109
#define X86_REG_R12  110
#define X86_REG_R13  111
#define X86_REG_R14  112
#define X86_REG_R15  113
#define X86_REG_R8B  218
#define X86_REG_R9B  219
#define X86_REG_R10B 220
#define X86_REG_R11B 221
#define X86_REG_R12B 222
#define X86_REG_R13B 223
#define X86_REG_R14B 224
#define X86_REG_R15B 225
#define X86_REG_R8D  226
#define X86_REG_R9D  227
#define X86_REG_R10D 228
#define X86_REG_R11D 229
#define X86_REG_R12D 230
#define X86_REG_R13D 231
#define X86_REG_R14D 232
#define X86_REG_R15D 233
#define X86_REG_R8W  234
#define X86_REG_R9W  235
#define X86_REG_R10W 236
#define X86_REG_R11W 237
#define X86_REG_R12W 238
#define X86_REG_R13W 239
#define X86_REG_R14W 240
#define X86_REG_R15W 241

/* Canonicalize a Capstone x86 GP register alias to (64-bit container id,
 * byte offset within the container, byte length). Returns 0 (unmapped) for
 * an id this file cannot resolve — vector registers, segment selectors,
 * X86_REG_EFLAGS, X86_REG_RIP, ... — on which the caller falls back to
 * today's raw single-key behavior (exact for these: none of them alias
 * with anything else, so raw-id keying was never wrong for them). */
static int reg_slice(uint32_t reg, uint32_t *container, uint16_t *off,
                     uint16_t *len) {
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        *container = X86_REG_RAX;
        break;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        *container = X86_REG_RBX;
        break;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        *container = X86_REG_RCX;
        break;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        *container = X86_REG_RDX;
        break;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        *container = X86_REG_RSI;
        break;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        *container = X86_REG_RDI;
        break;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        *container = X86_REG_RBP;
        break;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        *container = X86_REG_RSP;
        break;
    case X86_REG_R8:
    case X86_REG_R8D:
    case X86_REG_R8W:
    case X86_REG_R8B:
        *container = X86_REG_R8;
        break;
    case X86_REG_R9:
    case X86_REG_R9D:
    case X86_REG_R9W:
    case X86_REG_R9B:
        *container = X86_REG_R9;
        break;
    case X86_REG_R10:
    case X86_REG_R10D:
    case X86_REG_R10W:
    case X86_REG_R10B:
        *container = X86_REG_R10;
        break;
    case X86_REG_R11:
    case X86_REG_R11D:
    case X86_REG_R11W:
    case X86_REG_R11B:
        *container = X86_REG_R11;
        break;
    case X86_REG_R12:
    case X86_REG_R12D:
    case X86_REG_R12W:
    case X86_REG_R12B:
        *container = X86_REG_R12;
        break;
    case X86_REG_R13:
    case X86_REG_R13D:
    case X86_REG_R13W:
    case X86_REG_R13B:
        *container = X86_REG_R13;
        break;
    case X86_REG_R14:
    case X86_REG_R14D:
    case X86_REG_R14W:
    case X86_REG_R14B:
        *container = X86_REG_R14;
        break;
    case X86_REG_R15:
    case X86_REG_R15D:
    case X86_REG_R15W:
    case X86_REG_R15B:
        *container = X86_REG_R15;
        break;
    default:
        return 0; /* vector, segment, EFLAGS, RIP, ... : raw-id fallback */
    }

    /* Same grouping as dfp_alias_shape's shift/width switch, reshaped to
     * offset/length: shift 0/width 8|4|2|1 becomes offset 0/length 8|4|2|1;
     * shift 8/width 1 (the AH/BH/CH/DH high-byte forms) becomes offset 1/
     * length 1. */
    switch (reg) {
    case X86_REG_RAX:
    case X86_REG_RBX:
    case X86_REG_RCX:
    case X86_REG_RDX:
    case X86_REG_RSI:
    case X86_REG_RDI:
    case X86_REG_RBP:
    case X86_REG_RSP:
    case X86_REG_R8:
    case X86_REG_R9:
    case X86_REG_R10:
    case X86_REG_R11:
    case X86_REG_R12:
    case X86_REG_R13:
    case X86_REG_R14:
    case X86_REG_R15:
        *off = 0;
        *len = 8;
        return 1;
    case X86_REG_EAX:
    case X86_REG_EBX:
    case X86_REG_ECX:
    case X86_REG_EDX:
    case X86_REG_ESI:
    case X86_REG_EDI:
    case X86_REG_EBP:
    case X86_REG_ESP:
    case X86_REG_R8D:
    case X86_REG_R9D:
    case X86_REG_R10D:
    case X86_REG_R11D:
    case X86_REG_R12D:
    case X86_REG_R13D:
    case X86_REG_R14D:
    case X86_REG_R15D:
        *off = 0;
        *len = 4;
        return 1;
    case X86_REG_AX:
    case X86_REG_BX:
    case X86_REG_CX:
    case X86_REG_DX:
    case X86_REG_SI:
    case X86_REG_DI:
    case X86_REG_BP:
    case X86_REG_SP:
    case X86_REG_R8W:
    case X86_REG_R9W:
    case X86_REG_R10W:
    case X86_REG_R11W:
    case X86_REG_R12W:
    case X86_REG_R13W:
    case X86_REG_R14W:
    case X86_REG_R15W:
        *off = 0;
        *len = 2;
        return 1;
    case X86_REG_AL:
    case X86_REG_BL:
    case X86_REG_CL:
    case X86_REG_DL:
    case X86_REG_SIL:
    case X86_REG_DIL:
    case X86_REG_BPL:
    case X86_REG_SPL:
    case X86_REG_R8B:
    case X86_REG_R9B:
    case X86_REG_R10B:
    case X86_REG_R11B:
    case X86_REG_R12B:
    case X86_REG_R13B:
    case X86_REG_R14B:
    case X86_REG_R15B:
        *off = 0;
        *len = 1;
        return 1;
    case X86_REG_AH:
    case X86_REG_BH:
    case X86_REG_CH:
    case X86_REG_DH:
        *off = 1;
        *len = 1;
        return 1;
    default:
        return 0; /* unreachable: every id above appears in the first switch */
    }
}

/* Collect up to 64 DISTINCT producing steps across [base, base+n) at `kind`
 * (mem-byte or reg-container-byte), deduplicated by a linear "seen" scan.
 * Shared by emit_read's memory path and its sub-register-alias path so a
 * cross-alias register read dedups identically to a partial memory overlap
 * read. Returns the count written to `prod` (caller-sized, >= n). */
static size_t collect_distinct(const lw_map *map, uint8_t kind, uint64_t base,
                               uint16_t n, uint32_t *prod) {
    size_t np = 0;
    for (uint16_t b = 0; b < n; b++) {
        uint32_t p;
        if (!lw_get(map, kind, base + b, &p))
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
    return np;
}

/* lw_put one entry per byte of [base, base+n) at `kind` — the write-side twin
 * of collect_distinct, shared by apply_write's memory path and its
 * sub-register-alias path. */
static void put_range(lw_map *map, uint8_t kind, uint64_t base, uint16_t n,
                      uint32_t step) {
    for (uint16_t b = 0; b < n; b++)
        lw_put(map, kind, base + b, step);
}

static void emit_read(const at_val_rec_t *rec, const lw_map *map, edgevec *ev,
                      uint32_t step) {
    if (rec->kind == AT_LOC_REG) {
        uint32_t container;
        uint16_t off, len;
        if (reg_slice(rec->reg, &container, &off, &len)) {
            /* one edge per DISTINCT producing step across the alias's bytes */
            uint32_t prod[64];
            size_t np = collect_distinct(
                map, 2, ((uint64_t)container << 8) | off, len, prod);
            for (size_t j = 0; j < np; j++)
                edge_push(ev, prod[j], step, rec);
            return;
        }
        uint32_t p;
        if (lw_get(map, 0, rec->reg, &p))
            edge_push(ev, p, step, rec);
        return;
    }
    /* memory: one edge per DISTINCT producing step across the touched bytes */
    uint32_t prod[64];
    size_t np = collect_distinct(map, 1, rec->addr, rec_bytes(rec), prod);
    for (size_t j = 0; j < np; j++)
        edge_push(ev, prod[j], step, rec);
}

static void apply_write(const at_val_rec_t *rec, lw_map *map, uint32_t step) {
    if (rec->kind == AT_LOC_REG) {
        uint32_t container;
        uint16_t off, len;
        if (reg_slice(rec->reg, &container, &off, &len)) {
            /* x86-64: writing a 32-bit GP form (len==4; reg_slice always gives it
             * off==0) implicitly zero-extends the FULL 64-bit container — bits
             * 32-63 become a DEFINED value (zero) as a side effect of THIS write,
             * unlike a 16/8-bit write, which leaves the untouched bytes exactly as
             * they were (the classic x86 partial-register behavior AMD64 kept for
             * those widths but deliberately dropped for 32-bit ones). Widen only
             * the WRITE side's range to the full container for this one case, so a
             * later full-width read resolves its upper-half producer to THIS step
             * instead of fabricating a stale edge to whatever last wrote the
             * container before it was zero-extended. emit_read's `len` for the
             * same alias stays 4 — reading EAX only ever consumes its own low 4
             * bytes, never the zero-extended upper half. */
            uint16_t write_len = (len == 4) ? 8 : len;
            put_range(map, 2, ((uint64_t)container << 8) | off, write_len,
                      step);
            return;
        }
        lw_put(map, 0, rec->reg, step);
        return;
    }
    put_range(map, 1, rec->addr, rec_bytes(rec), step);
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

asmtest_slice_t *asmtest_slice_forward_seed(const asmtest_defuse_t *g,
                                            const at_val_rec_t *seed) {
    return slice_dir(g, seed ? seed->step : 0, true);
}

asmtest_slice_t *asmtest_slice_backward_seed(const asmtest_defuse_t *g,
                                             const at_val_rec_t *seed) {
    return slice_dir(g, seed ? seed->step : 0, false);
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
