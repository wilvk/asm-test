/*
 * asmspy_dataview.h — pure render/analysis helpers for asmspy's data-flow view.
 *
 * The interactive "Data flow" window (asmspy.c, TUI mode 9) and the headless
 * `--dataflow` renderer both need the same three pieces of NON-ncurses logic:
 *
 *   1. VALUE ANNOTATION — turn a captured step's operand records into the
 *      "->0x2a" / "[0x..]<-0x.." tokens shown after the disassembly.
 *   2. SLICE STYLING    — given a backward/forward slice, decide whether a step's
 *      row is highlighted (in the slice) or dimmed (outside it).
 *   3. DEF-USE PARTITION — for a selected step, how many values it READS (edges
 *      arriving from their last writers) vs WRITES that are later read (edges
 *      leaving it), and a printable name for the carried location.
 *
 * All three are pure functions over asmtest_valtrace_t / asmtest_defuse_t /
 * asmtest_slice_t — no ncurses, no ptrace, no Capstone — so the Increment-7
 * payoff logic (the slicer navigation + annotation the TUI can't be driven to
 * exercise headlessly) is unit-tested in cli/test_view.c, the same header-
 * extraction discipline as asmspy_logview.h / asmspy_graphsort.h.
 */
#ifndef ASMSPY_DATAVIEW_H
#define ASMSPY_DATAVIEW_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asmtest_valtrace.h" /* asmtest_valtrace_t / _defuse_t / _slice_t */

/* Append " tok" (or "tok" first) to the NUL-terminated `ann`, bounded by `cap`. */
static inline void asmspy_df_tok_append(char *ann, size_t cap,
                                        const char *tok) {
    size_t l = strlen(ann);
    if (l + 1 < cap)
        snprintf(ann + l, cap - l, "%s%s", l ? " " : "", tok);
}

/* Build step `s`'s captured-VALUE annotation into `ann[cap]` (always NUL-
 * terminated; empty when the step carried nothing worth showing). Records in
 * `vt` are appended grouped and in ascending step order, so `*cur` threads a
 * top-down walk across steps (pass a pointer to a local 0 for a one-off lookup;
 * it is advanced past step `s`'s records). The token grammar mirrors what the
 * disassembly cannot already show:
 *   - a register READ is skipped (it is the disasm's own source operand);
 *   - a register WRITE shows "->0xVAL" (or "->[wide]" for an XMM/YMM result);
 *   - a memory operand shows "[0xEA]<-0xVAL" (store) or "[0xEA]->0xVAL" (load),
 *     with "?" when the value was not captured and "[wide]" when it is >8 bytes.
 * At most `max_tokens` tokens are appended; an overflow appends a trailing "...".
 * Returns the number of operand records step `s` carried (shown or not). */
static inline size_t asmspy_df_annotate(const asmtest_valtrace_t *vt, size_t s,
                                        size_t *cur, int max_tokens, char *ann,
                                        size_t cap) {
    if (cap)
        ann[0] = '\0';
    if (vt == NULL)
        return 0;
    size_t nrecs = vt->recs_len;
    size_t c = cur ? *cur : 0;
    while (c < nrecs && vt->recs[c].step < s)
        c++; /* defensive: skip any records that precede this step */
    int shown = 0;
    size_t seen = 0;
    while (c < nrecs && vt->recs[c].step == s) {
        const at_val_rec_t *r = &vt->recs[c++];
        seen++;
        char tok[64];
        if (r->kind == AT_LOC_REG) {
            if (!r->is_write)
                continue; /* a source operand — already named in the disasm */
            if (r->wide)
                snprintf(tok, sizeof tok, "->[wide]");
            else if (r->value_valid)
                snprintf(tok, sizeof tok, "->0x%llx",
                         (unsigned long long)r->value);
            else
                continue;
        } else {
            const char *arrow = r->is_write ? "<-" : "->";
            if (r->wide)
                snprintf(tok, sizeof tok, "[0x%llx]%s[wide]",
                         (unsigned long long)r->addr, arrow);
            else if (r->value_valid)
                snprintf(tok, sizeof tok, "[0x%llx]%s0x%llx",
                         (unsigned long long)r->addr, arrow,
                         (unsigned long long)r->value);
            else
                snprintf(tok, sizeof tok, "[0x%llx]%s?",
                         (unsigned long long)r->addr, arrow);
        }
        if (shown < max_tokens)
            asmspy_df_tok_append(ann, cap, tok);
        shown++;
    }
    if (shown > max_tokens)
        asmspy_df_tok_append(ann, cap, "...");
    if (cur)
        *cur = c;
    return seen;
}

/* Row emphasis for the data-flow disassembly pane. With no slice active every
 * row is NORMAL; with a slice active a step IN the slice is IN-SLICE and one
 * outside it is DIMMED. (The selected-row reverse bar is layered on top by the
 * renderer; this only decides the slice highlight/dim.) */
typedef enum {
    ASMSPY_DF_ROW_NORMAL = 0,  /* no slice active — ordinary row      */
    ASMSPY_DF_ROW_INSLICE = 1, /* slice active, step is in the slice  */
    ASMSPY_DF_ROW_DIMMED = 2,  /* slice active, step is outside it    */
} asmspy_df_rowstyle_t;

static inline asmspy_df_rowstyle_t
asmspy_df_rowstyle(const asmtest_slice_t *slice, uint32_t step) {
    if (slice == NULL)
        return ASMSPY_DF_ROW_NORMAL;
    return asmtest_slice_contains(slice, step) ? ASMSPY_DF_ROW_INSLICE
                                               : ASMSPY_DF_ROW_DIMMED;
}

/* Count the def-use edges INTO `step` (values it READS, each arriving from its
 * last writer) and OUT of `step` (values it WRITES that are later read). Either
 * counter pointer may be NULL. A NULL graph yields zero for both. This is the
 * direction-sensitive core of the bottom def-use pane, so it is unit-tested. */
static inline void asmspy_df_defuse_counts(const asmtest_defuse_t *g,
                                           uint32_t step, size_t *n_in,
                                           size_t *n_out) {
    size_t in = 0, out = 0;
    for (size_t i = 0; g != NULL && i < g->n; i++) {
        if (g->edges[i].to_step == step)
            in++;
        if (g->edges[i].from_step == step)
            out++;
    }
    if (n_in)
        *n_in = in;
    if (n_out)
        *n_out = out;
}

/* Printable name for a def-use edge's carried location (the consumer's read
 * record): "reg#<id>" for a register, "[0xADDR]" for memory. Register ids are
 * Capstone numeric ids (naming them needs Capstone, which this pure header
 * avoids); the disassembly of the endpoint steps names them for the reader. */
static inline void asmspy_df_loc_str(const at_val_rec_t *loc, char *buf,
                                     size_t cap) {
    if (cap == 0)
        return;
    if (loc == NULL) {
        buf[0] = '\0';
        return;
    }
    if (loc->kind == AT_LOC_REG)
        snprintf(buf, cap, "reg#%u", loc->reg);
    else
        snprintf(buf, cap, "[0x%llx]", (unsigned long long)loc->addr);
}

#endif /* ASMSPY_DATAVIEW_H */
