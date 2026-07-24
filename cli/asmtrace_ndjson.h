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
 * volatile in deterministic mode. Returns 0 / -1 on I/O failure. */
int asmtrace_header(asmtrace_writer_t *w, const char *producer,
                    const asmtrace_prov_t *prov, long pid, const char *cmd);

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

/* Write the `end` footer (events / truncated / drops, plus `skip` when
 * `skip_update` carries a non-zero skip_code), flush, and fclose when the
 * writer owns the file. A writer closed WITHOUT this leaves a TORN recording —
 * deliberate: a producer killed mid-record must be visibly incomplete, so there
 * is no atexit rescue. Returns 0 / -1; safe (no-op, 0) on a NULL/unopened
 * writer. */
int asmtrace_close(asmtrace_writer_t *w, unsigned long long lost, int throttled,
                   const asmtrace_prov_t *skip_update);

#endif /* ASMTRACE_NDJSON_H */
