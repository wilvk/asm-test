/*
 * trace.c — engine-neutral execution-trace substrate (see asmtest_trace.h).
 *
 * The append/dedup/truncate logic, the opaque-handle FFI accessors, and the
 * coverage/report/source-line helpers used to live inside the Unicorn tier
 * (src/emu.c) and the FFI layer (src/ffi.c). They are extracted here so every
 * trace backend — Unicorn, the DynamoRIO drmgr client, and the Intel PT / ARM
 * CoreSight decoders — shares one sink with identical semantics. This file has
 * NO Unicorn dependency: it is linked into the lean core shared lib (so dynamic
 * bindings reach the accessors) and into every emulator/native/hardware tier.
 *
 * The historical emu_* spellings stay exported verbatim so ASSERT_BLOCK_COVERED,
 * the language bindings, and the ABI manifest are unaffected.
 */
#include "asmtest_trace.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Generic fill points                                                 */
/* ------------------------------------------------------------------ */

void trace_append_insn(asmtest_trace_t *t, uint64_t off) {
    if (t == NULL)
        return;
    if (t->insns != NULL) {
        if (t->insns_len < t->insns_cap)
            t->insns[t->insns_len++] = off;
        else
            t->truncated = true;
    }
    t->insns_total++;
}

void trace_append_block(asmtest_trace_t *t, uint64_t off) {
    if (t == NULL)
        return;
    t->blocks_total++;
    if (t->blocks != NULL) {
        /* The dedup scan is linear, fine for the small block counts of a
         * routine under test (loops re-enter the same block; counted only in
         * blocks_total). */
        for (size_t i = 0; i < t->blocks_len; i++)
            if (t->blocks[i] == off)
                return; /* already covered */
        if (t->blocks_len < t->blocks_cap)
            t->blocks[t->blocks_len++] = off;
        else
            t->truncated = true;
    }
}

/* ------------------------------------------------------------------ */
/* Opaque-handle allocate / free / accessors (moved from src/ffi.c)    */
/* ------------------------------------------------------------------ */

asmtest_trace_t *asmtest_trace_new(size_t insns_cap, size_t blocks_cap) {
    asmtest_trace_t *t = (asmtest_trace_t *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    if (insns_cap) {
        t->insns = (uint64_t *)calloc(insns_cap, sizeof(uint64_t));
        if (t->insns)
            t->insns_cap = insns_cap;
    }
    if (blocks_cap) {
        t->blocks = (uint64_t *)calloc(blocks_cap, sizeof(uint64_t));
        if (t->blocks)
            t->blocks_cap = blocks_cap;
    }
    return t;
}

void asmtest_trace_free(asmtest_trace_t *t) {
    if (!t)
        return;
    free(t->insns);
    free(t->blocks);
    free(t);
}

/* asmtest_emu_trace_new/free keep their historical names for binding ABI
 * stability; they are backend-neutral aliases of the canonical forms. */
asmtest_trace_t *asmtest_emu_trace_new(size_t insns_cap, size_t blocks_cap) {
    return asmtest_trace_new(insns_cap, blocks_cap);
}
void asmtest_emu_trace_free(asmtest_trace_t *t) { asmtest_trace_free(t); }

/* Instructions executed (counts past the insns buffer cap). */
unsigned long long asmtest_emu_trace_insns_total(const asmtest_trace_t *t) {
    return (unsigned long long)t->insns_total;
}
/* Distinct basic blocks recorded (<= blocks_cap). */
unsigned long long asmtest_emu_trace_blocks_len(const asmtest_trace_t *t) {
    return (unsigned long long)t->blocks_len;
}
/* Instruction offsets actually stored in insns[] (<= insns_cap). Distinct from
 * insns_total, which counts every executed instruction past the buffer cap; this
 * is the count an insn_at() reader should iterate. */
unsigned long long asmtest_emu_trace_insns_len(const asmtest_trace_t *t) {
    return (unsigned long long)t->insns_len;
}
/* Block entries total — a loop re-counts each pass. */
unsigned long long asmtest_emu_trace_blocks_total(const asmtest_trace_t *t) {
    return (unsigned long long)t->blocks_total;
}
/* True if a buffer filled and at least one entry was dropped. */
int asmtest_emu_trace_truncated(const asmtest_trace_t *t) {
    return t->truncated ? 1 : 0;
}
/* The i-th distinct block-start offset (byte offset from the routine entry). */
unsigned long long asmtest_emu_trace_block_at(const asmtest_trace_t *t,
                                              size_t i) {
    return (i < t->blocks_len) ? (unsigned long long)t->blocks[i] : 0;
}
/* The i-th instruction offset in the ordered execution-order stream — the insns[]
 * counterpart to block_at, so a binding can read the instruction stream through a
 * scalar accessor instead of laying out the struct. */
unsigned long long asmtest_emu_trace_insn_at(const asmtest_trace_t *t,
                                             size_t i) {
    return (i < t->insns_len) ? (unsigned long long)t->insns[i] : 0;
}
/* True if basic-block offset `off` is in the distinct block set (FFI shim). */
int asmtest_emu_trace_covered(const asmtest_trace_t *t,
                              unsigned long long off) {
    for (size_t i = 0; i < t->blocks_len; i++)
        if (t->blocks[i] == (uint64_t)off)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Coverage predicates / reporting (moved from src/emu.c)              */
/* ------------------------------------------------------------------ */

int asmtest_trace_covered(const asmtest_trace_t *t, uint64_t off) {
    if (t == NULL || t->blocks == NULL)
        return 0;
    for (size_t i = 0; i < t->blocks_len; i++)
        if (t->blocks[i] == off)
            return 1;
    return 0;
}

/* Compatibility shim: the bool-returning predicate behind ASSERT_BLOCK_COVERED. */
bool emu_trace_covered(const asmtest_trace_t *t, uint64_t off) {
    return asmtest_trace_covered(t, off) != 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Copy a trace's distinct block offsets into a freshly malloc'd, ascending
 * array (caller frees). *n receives the count; returns NULL on empty/oom. */
static uint64_t *sorted_blocks(const asmtest_trace_t *t, size_t *n) {
    *n = (t != NULL && t->blocks != NULL) ? t->blocks_len : 0;
    if (*n == 0)
        return NULL;
    uint64_t *s = (uint64_t *)malloc(*n * sizeof *s);
    if (s == NULL) {
        *n = 0;
        return NULL;
    }
    memcpy(s, t->blocks, *n * sizeof *s);
    qsort(s, *n, sizeof *s, cmp_u64);
    return s;
}

void emu_trace_report(const asmtest_trace_t *t, FILE *out) {
    if (t == NULL || out == NULL)
        return;
    fprintf(out,
            "coverage: %zu distinct blocks, %llu block entries, "
            "%llu instructions%s\n",
            t->blocks_len, (unsigned long long)t->blocks_total,
            (unsigned long long)t->insns_total,
            t->truncated ? " (truncated)" : "");
    size_t n;
    uint64_t *s = sorted_blocks(t, &n);
    if (s == NULL)
        return;
    fprintf(out, "  blocks:");
    for (size_t i = 0; i < n; i++)
        fprintf(out, " 0x%llx", (unsigned long long)s[i]);
    fprintf(out, "\n");
    free(s);
}

/* Canonical spelling of emu_trace_report. */
void asmtest_trace_report(const asmtest_trace_t *t, FILE *out) {
    emu_trace_report(t, out);
}

size_t emu_coverage_uncovered(const asmtest_trace_t *covered,
                              const asmtest_trace_t *universe, FILE *out) {
    if (universe == NULL || universe->blocks == NULL)
        return 0;
    size_t total = universe->blocks_len;
    uint64_t *miss = (uint64_t *)malloc((total ? total : 1) * sizeof *miss);
    if (miss == NULL)
        return 0;
    size_t nmiss = 0;
    for (size_t i = 0; i < total; i++)
        if (!emu_trace_covered(covered, universe->blocks[i]))
            miss[nmiss++] = universe->blocks[i];
    qsort(miss, nmiss, sizeof *miss, cmp_u64);
    if (out != NULL) {
        fprintf(out, "coverage: %zu/%zu blocks covered\n", total - nmiss,
                total);
        if (nmiss > 0) {
            fprintf(out, "  uncovered:");
            for (size_t i = 0; i < nmiss; i++)
                fprintf(out, " 0x%llx", (unsigned long long)miss[i]);
            fprintf(out, "\n");
        }
    }
    free(miss);
    return nmiss;
}

void emu_trace_lcov(const asmtest_trace_t *t, const char *name, FILE *out) {
    if (t == NULL || out == NULL)
        return;
    /* No debug info, so block byte-offsets stand in for source lines. */
    fprintf(out, "TN:\n");
    fprintf(out, "SF:%s\n", name != NULL ? name : "routine");
    size_t n;
    uint64_t *s = sorted_blocks(t, &n);
    for (size_t i = 0; i < n; i++)
        fprintf(out, "DA:%llu,1\n", (unsigned long long)s[i]);
    free(s);
    fprintf(out, "LF:%zu\nLH:%zu\n", n, n);
    fprintf(out, "end_of_record\n");
}

/* ------------------------------------------------------------------ */
/* Source-line coverage mapping (moved from src/emu.c)                 */
/* ------------------------------------------------------------------ */

const emu_line_entry_t *emu_line_lookup(const emu_line_map_t *map,
                                        uint64_t off) {
    if (map == NULL || map->entries == NULL || map->count == 0)
        return NULL;
    const emu_line_entry_t *hit = NULL;
    /* Rows are ascending by offset, so the last row with offset <= off owns it;
     * once a row starts past off, every later row does too. */
    for (size_t i = 0; i < map->count; i++) {
        if (map->entries[i].offset <= off)
            hit = &map->entries[i];
        else
            break;
    }
    return hit;
}

/* One source line plus whether a covered block landed on it. */
typedef struct {
    uint32_t line;
    int hit;
} line_cov_t;

static int cmp_line_cov(const void *a, const void *b) {
    uint32_t x = ((const line_cov_t *)a)->line,
             y = ((const line_cov_t *)b)->line;
    return (x > y) - (x < y);
}

/* Collect the distinct source lines named by `map`, flagging each line a
 * covered block-start offset resolves to. Returns a malloc'd, line-sorted array
 * (caller frees) with the count in *n; NULL on an empty map / oom. */
static line_cov_t *source_coverage(const asmtest_trace_t *covered,
                                   const emu_line_map_t *map, size_t *n) {
    *n = 0;
    if (map == NULL || map->entries == NULL || map->count == 0)
        return NULL;
    line_cov_t *lc = (line_cov_t *)malloc(map->count * sizeof *lc);
    if (lc == NULL)
        return NULL;
    size_t m = 0;
    for (size_t i = 0; i < map->count; i++) { /* distinct lines */
        uint32_t ln = map->entries[i].line;
        size_t j;
        for (j = 0; j < m; j++)
            if (lc[j].line == ln)
                break;
        if (j == m) {
            lc[m].line = ln;
            lc[m].hit = 0;
            m++;
        }
    }
    if (covered != NULL && covered->blocks != NULL) { /* mark hits */
        for (size_t b = 0; b < covered->blocks_len; b++) {
            const emu_line_entry_t *en =
                emu_line_lookup(map, covered->blocks[b]);
            if (en == NULL)
                continue;
            for (size_t j = 0; j < m; j++)
                if (lc[j].line == en->line) {
                    lc[j].hit = 1;
                    break;
                }
        }
    }
    qsort(lc, m, sizeof *lc, cmp_line_cov);
    *n = m;
    return lc;
}

size_t emu_trace_source_report(const asmtest_trace_t *covered,
                               const emu_line_map_t *map, FILE *out) {
    size_t n;
    line_cov_t *lc = source_coverage(covered, map, &n);
    if (lc == NULL)
        return 0;
    size_t hit = 0;
    for (size_t i = 0; i < n; i++)
        if (lc[i].hit)
            hit++;
    if (out != NULL) {
        fprintf(out, "source coverage: %zu/%zu lines covered\n", hit, n);
        if (hit < n) {
            fprintf(out, "  uncovered lines:");
            for (size_t i = 0; i < n; i++)
                if (!lc[i].hit)
                    fprintf(out, " %u", lc[i].line);
            fprintf(out, "\n");
        }
    }
    free(lc);
    return n - hit;
}

void emu_trace_lcov_source(const asmtest_trace_t *covered,
                           const emu_line_map_t *map, const char *source_file,
                           FILE *out) {
    if (out == NULL)
        return;
    size_t n;
    line_cov_t *lc = source_coverage(covered, map, &n);
    fprintf(out, "TN:\n");
    fprintf(out, "SF:%s\n", source_file != NULL ? source_file : "routine");
    size_t hit = 0;
    for (size_t i = 0; i < n; i++) {
        fprintf(out, "DA:%u,%d\n", lc[i].line, lc[i].hit ? 1 : 0);
        if (lc[i].hit)
            hit++;
    }
    fprintf(out, "LF:%zu\nLH:%zu\n", n, hit);
    fprintf(out, "end_of_record\n");
    free(lc);
}

/* ------------------------------------------------------------------ */
/* Widened attribution schema (asmtest_srcmap_*)                       */
/* ------------------------------------------------------------------ */

const asmtest_srcmap_entry_t *asmtest_srcmap_lookup(const asmtest_srcmap_t *m,
                                                    uint64_t off) {
    if (m == NULL || m->entries == NULL || m->count == 0)
        return NULL;
    const asmtest_srcmap_entry_t *hit = NULL;
    /* Rows are ascending by offset: the last row with offset <= off owns it
     * (identical loop shape to emu_line_lookup — the enclosing debug point). */
    for (size_t i = 0; i < m->count; i++) {
        if (m->entries[i].offset <= off)
            hit = &m->entries[i];
        else
            break;
    }
    return hit;
}

/* Render one row's kind-labelled value: `line 21`, `IL_0x1a`, `PROLOG`,
 * `bci 7`. The .NET pseudo-IL offsets keep their ICorDebugInfo names. */
static void srcmap_label(const asmtest_srcmap_entry_t *en, char *buf,
                         size_t cap) {
    switch (en->kind) {
    case ASMTEST_SRC_LINE:
        snprintf(buf, cap, "line %d", en->value);
        break;
    case ASMTEST_SRC_IL:
        if (en->value == ASMTEST_SRC_IL_NO_MAPPING)
            snprintf(buf, cap, "NO_MAPPING");
        else if (en->value == ASMTEST_SRC_IL_PROLOG)
            snprintf(buf, cap, "PROLOG");
        else if (en->value == ASMTEST_SRC_IL_EPILOG)
            snprintf(buf, cap, "EPILOG");
        else
            snprintf(buf, cap, "IL_0x%x", (unsigned)en->value);
        break;
    case ASMTEST_SRC_BCI:
        snprintf(buf, cap, "bci %d", en->value);
        break;
    default:
        snprintf(buf, cap, "value %d", en->value);
        break;
    }
}

size_t asmtest_srcmap_report(const asmtest_trace_t *covered,
                             const asmtest_srcmap_t *m, FILE *out) {
    if (covered == NULL || covered->blocks == NULL)
        return 0;
    size_t unattributed = 0;
    for (size_t b = 0; b < covered->blocks_len; b++) {
        uint64_t off = covered->blocks[b];
        const asmtest_srcmap_entry_t *en = asmtest_srcmap_lookup(m, off);
        if (en == NULL) {
            unattributed++;
            if (out != NULL)
                fprintf(out, "  +0x%llx  <unattributed>\n",
                        (unsigned long long)off);
            continue;
        }
        if (out != NULL) {
            char label[64];
            srcmap_label(en, label, sizeof label);
            const char *file = NULL;
            if (en->file_id != UINT32_MAX && m != NULL && m->files != NULL &&
                en->file_id < m->files_count)
                file = m->files[en->file_id];
            if (file != NULL && file[0] != '\0')
                fprintf(out, "  +0x%llx  %s:%s\n", (unsigned long long)off,
                        file, label);
            else
                fprintf(out, "  +0x%llx  %s\n", (unsigned long long)off, label);
        }
    }
    if (out != NULL)
        fprintf(out, "srcmap: %zu/%zu covered blocks attributed\n",
                covered->blocks_len - unattributed, covered->blocks_len);
    return unattributed;
}
