/*
 * asmtrace_ndjson.h — the `.asmtrace` NDJSON recording writer.
 *
 * Contract: docs/internal/gui/asmtrace-schema.md (draft; the v1 freeze is a
 * later checkpoint). ONE writer TU owns field order for the whole tree, which
 * is what makes the golden corpus byte-comparable: two producers cannot
 * disagree about how a `trace` event is spelled if they both go through here.
 *
 * PURE C11 + stdio by design — no ptrace, no ncurses, no Capstone — so the
 * Author-mode corpus recorder (tools/asmtrace_record.c) compiles it anywhere
 * the emulator runs, not just where asmspy does.
 *
 * The writer does NOT know the schema's per-kind field lists: callers hand it a
 * pre-formatted body (asmtrace_emit) or a printf format (asmtrace_emitf). What
 * it owns is the envelope — header line, the `{"k":...}` wrapper, escaping, the
 * event count, and the `end` footer.
 */
#ifndef ASMTRACE_NDJSON_H
#define ASMTRACE_NDJSON_H

#include <stdarg.h>
#include <stdio.h>

#include "asmtest_valtrace.h" /* at_val_rec_t / asmtest_defuse_edge_t (pure) */

/* Mandatory provenance: which backend produced this stream and how much it can
 * be trusted. Every string is BORROWED (valid for the header/close call only).
 * A statistical stream must set exact = 0 and trust = "statistical"; a reader
 * never merges one into an exact kind. */
typedef struct {
    const char *backend;     /* "ptrace-syscalls", "emu-l0", "ibs-op", ... */
    int exact;               /* 1 exact, 0 statistical                     */
    const char *trust;       /* "exact"|"statistical"|"weak"|"strong"      */
    int skip_code;           /* 0 = none; else the positive ASMSPY_* code  */
    const char *skip_reason; /* NULL = none (measured string, borrowed)    */
    int redacted;            /* 1 when payloads are withheld at record     */
} asmtrace_prov_t;

typedef struct {
    FILE *f;
    int deterministic;         /* omit ts/pid/cmd (golden mode)          */
    int owns_file;             /* 0 when bound to a caller's stream      */
    unsigned long long events; /* event lines emitted so far             */
    int truncated;             /* sticky; folded into `end`              */
    int err;                   /* sticky: an fprintf/fclose failed       */

    /* Optional `steps_total` footer field (28 R1 T2): the total step count the
     * producer SAW, counting past any ring cap. Set before asmtrace_close so a
     * truncation banner can read "N of M" instead of naming the gap. Absent
     * (have_steps_total == 0) keeps the older honest wording. */
    int have_steps_total;
    unsigned long long steps_total;

    /* Optional `code` header identity (28 R1 T1), set BEFORE asmtrace_header
     * by a producer that holds stable routine bytes. have_code == 0 (the
     * memset default) omits the object entirely — a live attach with no fixed
     * window never emits a zero hash. */
    int have_code;
    char code_name[128];         /* routine name (escaped at emit)         */
    char code_sha[65];           /* lowercase-hex SHA-256 of the bytes     */
    unsigned long long code_len; /* the byte length hashed               */
} asmtrace_writer_t;

/* Open `path` for writing and zero the rest of *w. Returns 0, or -1 (errno set
 * by fopen). `deterministic` = golden mode: the header omits ts/pid/cmd. */
int asmtrace_open(asmtrace_writer_t *w, const char *path, int deterministic);

/* Bind the writer to an already-open stream the CALLER owns (stdout for
 * `--json`). asmtrace_close writes the footer and flushes but does NOT fclose.
 * Always returns 0. */
int asmtrace_open_file(asmtrace_writer_t *w, FILE *f, int deterministic);

/* The header line (must be first). `producer` is "asmspy" or
 * "asmtrace_record"; `pid` <= 0 and `cmd` == NULL are omitted, as is everything
 * volatile in deterministic mode. When asmtrace_writer_set_code has armed the
 * `code` identity, the header carries it (in BOTH modes — routine identity is a
 * fact about the bytes, not the run). Returns 0 / -1 on I/O failure. */
int asmtrace_header(asmtrace_writer_t *w, const char *producer,
                    const asmtrace_prov_t *prov, long pid, const char *cmd);

/* Arm the optional `code` header object {"name","sha256","len"} (28 R1 T1): the
 * SHA-256 identity of the routine's bytes, so a consumer can prove two recordings
 * are (or refuse them as not) the same routine. Call AFTER asmtrace_open* and
 * BEFORE asmtrace_header. `name` and `sha256_hex` are copied (bounded); a NULL
 * `sha256_hex` disarms it (nothing is emitted) — the honest state for a producer
 * that lacks stable bytes. `sha256_hex` must be the 64-char lowercase-hex digest
 * (asmtrace_sha256_hex). */
void asmtrace_writer_set_code(asmtrace_writer_t *w, const char *name,
                              const char *sha256_hex, unsigned long long len);

/* Arm the optional `steps_total` footer field (28 R1 T2): the total number of
 * steps the producer saw, past any ring cap. Call before asmtrace_close. This
 * lets a truncation banner read "N of M" — the M a truncate-when-full (tail-drop)
 * ring cannot otherwise supply, since it passes drops.lost = 0. */
void asmtrace_writer_set_steps_total(asmtrace_writer_t *w,
                                     unsigned long long total);

/* One event line: {"k":"<kind>",<body>}\n. `body` is PRE-FORMATTED JSON fields
 * with NO leading comma, built from asmtrace_escape'd strings; NULL/"" emits a
 * bare {"k":"<kind>"}. Returns 0 / -1. */
int asmtrace_emit(asmtrace_writer_t *w, const char *kind, const char *body);

/* asmtrace_emit with a printf-formatted body. */
int asmtrace_emitf(asmtrace_writer_t *w, const char *kind, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

/* Escape `src` into `dst` as the BODY of a JSON double-quoted string (no
 * surrounding quotes): " and \ backslash-escaped, bytes < 0x20 as \u00xx
 * (lowercase hex), everything else verbatim. Truncates rather than overflows;
 * always NUL-terminates. A NULL `src` yields "". */
void asmtrace_escape(char *dst, size_t cap, const char *src);

/* ------------------------------------------------------------------ */
/* Body builders for the data-flow kinds                               */
/*                                                                     */
/* These format a BODY (no wrapper, no leading comma) rather than       */
/* emitting, so a caller teeing several writers hands the identical     */
/* string to each. They live here because operand field order must have */
/* ONE owner: asmspy's --dataflow record sink and the Author-mode       */
/* corpus recorder both go through them, so a golden recording and a    */
/* live one cannot spell the same operand differently.                  */
/* ------------------------------------------------------------------ */

/* {"step":N,"off":N,"disasm":"..."?,"ops":[...]} — `recs[0..n)` must be
 * exactly this step's operand records. `disasm` may be NULL/"" (D10: a
 * producer without Capstone omits the field and readers degrade to offsets).
 * `wide`/`wide_len` are the valtrace's side buffer (asmtest_valtrace_t.wide):
 * a >8-byte operand's `size` bytes at its wide_off are emitted as a lowercase-hex
 * `bytes` field (28 R1 T3). Pass NULL/0 when the producer has no side buffer, and
 * a wide operand degrades to `"wide":true` with no bytes.
 * Returns the body length written (truncated to fit `cap`). */
size_t asmtrace_df_step_body(char *dst, size_t cap, unsigned step, uint64_t off,
                             const char *disasm, const at_val_rec_t *recs,
                             size_t n, const uint8_t *wide, size_t wide_len);

/* {"from":N,"to":N,"loc":{...}} for one last-writer def-use edge. */
size_t asmtrace_df_edge_body(char *dst, size_t cap,
                             const asmtest_defuse_edge_t *e);

/* {"step":N,"ea":N,"size":N,"rw":"r"|"w","space":"abs"|"off"} — one memory access
 * for the `mem` address-stream (29 R2). `ea` is the resolved effective address (the
 * valtrace record's `addr`), `space` its normalization (from the record's kind:
 * AT_LOC_MEM_ABS -> "abs", AT_LOC_MEM_OFF -> "off"); a register kind is not a memory
 * access and must not be passed here. `size` is the access width in bytes. Field
 * order lives here so the emulator and live producers spell a `mem` event
 * identically. Returns the body length written. */
size_t asmtrace_mem_body(char *dst, size_t cap, unsigned step, uint64_t ea,
                         unsigned size, int is_write, at_loc_kind_t space);

/* {"desc":"user_regs@x86_64/sysv","values":{rax..r15,rip,rflags}} — one per-step
 * register-file snapshot for the LIVE ptrace-dataflow `regstate` ring (26). The 16
 * GPRs + rip + rflags as decimal u64, in the fixed name order the Scrubber keys on;
 * field-compatible with the emulator's emit_regstate (only the descriptor id
 * differs, naming the ptrace source). Returns the body length written. */
size_t asmtrace_regstate_body(char *dst, size_t cap,
                              const asmtest_regfile_t *r);

/* {"step":S,"off":O,"loc":{...}?,"cone":[{"step":N,"off":M,"kind":"insn"},...],
 * "born_untraced":bool} — one backward-attribution (`blame`) event (33 R6 T1): the
 * value produced at step `S` (offset `O`, identified by the optional `loc` operand)
 * attributed to the producing steps `cone` (the ascending backward def-use slice,
 * INCLUDING the sink step S). `cone_steps`/`cone_offs` are parallel arrays of length
 * `ncone` (each cone step and its instruction offset). `born_untraced` is 1 when the
 * value has NO traced producer inside the window (cone is the sink alone) — the
 * honest "provenance starts at instrumentation" verdict, never an empty cone. `loc`
 * NULL omits the field (blame is step-granular). Field order lives here. Returns the
 * body length written. */
size_t asmtrace_blame_body(char *dst, size_t cap, unsigned step, uint64_t off,
                           const at_val_rec_t *loc, const uint32_t *cone_steps,
                           const uint64_t *cone_offs, size_t ncone,
                           int born_untraced);

/* {"step":S,"changed":{"<reg>":<u64>,...},"computed":bool} — one step-to-step
 * architectural-state delta (`statediff`, 33 R6 T2): the registers whose value
 * differs between `prev` and `cur` (the previous and current held register files),
 * in descriptor order, each carrying its NEW value. `prev` NULL means there is no
 * known predecessor (the first held step, or across an evicted boundary): emit
 * `"changed":{},"computed":false` — a full delta there would be a D7 lie
 * ("everything changed"). With a real predecessor, `computed` is true. One
 * statediff pairs 1:1 with its `regstate`. Field order lives here. Returns the body
 * length written. */
size_t asmtrace_statediff_body(char *dst, size_t cap, unsigned step,
                               const asmtest_regfile_t *prev,
                               const asmtest_regfile_t *cur);

/* Write the `end` footer (events / truncated / drops, plus `skip` when
 * `skip_update` carries a non-zero skip_code), flush, and fclose when the
 * writer owns the file. A writer closed WITHOUT this leaves a TORN recording —
 * deliberate: a producer killed mid-record must be visibly incomplete, so there
 * is no atexit rescue. Returns 0 / -1; safe (no-op, 0) on a NULL/unopened
 * writer. */
int asmtrace_close(asmtrace_writer_t *w, unsigned long long lost, int throttled,
                   const asmtrace_prov_t *skip_update);

#endif /* ASMTRACE_NDJSON_H */
