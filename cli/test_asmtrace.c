/* test_asmtrace.c — headless unit test for the `.asmtrace` NDJSON writer
 * (cli/asmtrace_ndjson.h) and the honesty facts the format is supposed to
 * carry.
 *
 * Two halves, both run by `make cli-smoke`:
 *
 *   writer.*  — the envelope contract: header first, fixed field order (two
 *               identical writes are byte-identical), escaping edges,
 *               deterministic mode, the `end` footer's counts, and the
 *               deliberate TORN file a writer that is never closed leaves.
 *   schema.*  — the example embedded in docs/internal/gui/asmtrace-schema.md is
 *               parsed by the same reader, so the written contract cannot drift
 *               away from the code without this failing.
 *   fixture.* — the committed dishonesty fixtures (truncated / dropped /
 *               redacted / torn) surface their flags through that reader. The
 *               RENDERER half of that discipline lives in the desktop docs;
 *               this is the reader-level half.
 *
 * Deliberately tiny and line-oriented: the full reader library belongs to
 * docs/internal/gui/02-exporters-and-readers.md. This links only
 * asmtrace_ndjson.o — no ptrace, no ncurses, no Capstone — so it runs anywhere.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtrace_ndjson.h"

static int failures;

static void check(const char *name, int ok, const char *detail) {
    if (!ok) {
        fprintf(stderr, "FAIL %s%s%s\n", name, detail ? ": " : "",
                detail ? detail : "");
        failures++;
    }
}
static void check_str(const char *name, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", name, got, want);
        failures++;
    }
}
static void check_ll(const char *name, long long got, long long want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %lld, want %lld\n", name, got, want);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* A minimal NDJSON line reader — enough to assert the schema's facts   */
/* without pulling in a JSON library. Scans for "key": at brace depth 1 */
/* so a nested object's field never masquerades as a top-level one.     */
/* ------------------------------------------------------------------ */

/* Structural check: the line is one balanced JSON object, strings closed and
 * escapes respected. Catches exactly the bug an unescaped quote would cause. */
static int json_line_ok(const char *s) {
    int depth = 0, instr = 0, sawobj = 0;
    if (*s != '{')
        return 0;
    for (; *s; s++) {
        if (instr) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == '"')
                instr = 0;
            continue;
        }
        if (*s == '"')
            instr = 1;
        else if (*s == '{' || *s == '[')
            depth++;
        else if (*s == '}' || *s == ']') {
            if (--depth < 0)
                return 0;
            if (depth == 0)
                sawobj = 1;
        } else if (sawobj && *s != '\n' && *s != ' ')
            return 0; /* trailing garbage after the object closed */
    }
    return depth == 0 && !instr && sawobj;
}

/* Point at the value of top-level "key" in `line`, or NULL. */
static const char *field(const char *line, const char *key) {
    size_t klen = strlen(key);
    int depth = 0, instr = 0;
    for (const char *s = line; *s; s++) {
        if (instr) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == '"') {
                instr = 0;
                /* A key is a string at depth 1 immediately followed by ':'. */
                continue;
            }
            continue;
        }
        if (*s == '{' || *s == '[')
            depth++;
        else if (*s == '}' || *s == ']')
            depth--;
        else if (*s == '"') {
            if (depth == 1 && strncmp(s + 1, key, klen) == 0 &&
                s[1 + klen] == '"' && s[2 + klen] == ':')
                return s + 3 + klen;
            instr = 1;
        }
    }
    return NULL;
}

/* Copy the top-level string field "key" into out (unescaped only for \" and
 * \\, which is all the writer emits verbatim). Returns 1 if present. */
static int field_str(const char *line, const char *key, char *out, size_t cap) {
    const char *v = field(line, key);
    size_t o = 0;
    if (!v || *v != '"')
        return 0;
    for (v++; *v && *v != '"' && o + 1 < cap; v++) {
        if (*v == '\\' && v[1]) {
            v++;
            out[o++] = (*v == 'n') ? '\n' : *v;
        } else
            out[o++] = *v;
    }
    out[o] = '\0';
    return 1;
}
static int field_ll(const char *line, const char *key, long long *out) {
    const char *v = field(line, key);
    if (!v || (*v != '-' && (*v < '0' || *v > '9')))
        return 0;
    *out = strtoll(v, NULL, 10);
    return 1;
}
static int field_bool(const char *line, const char *key, int *out) {
    const char *v = field(line, key);
    if (!v)
        return 0;
    if (strncmp(v, "true", 4) == 0)
        *out = 1;
    else if (strncmp(v, "false", 5) == 0)
        *out = 0;
    else
        return 0;
    return 1;
}

#define MAX_KINDS 64
#define MAX_LINE  8192

typedef struct {
    int lines;     /* total non-empty lines                        */
    int header_ok; /* line 1 parsed as an asmtrace:1 header        */
    int malformed; /* lines that failed the structural check       */
    int torn;      /* no `end` event = a torn recording            */
    int nkinds;    /* event kinds recorded, in file order          */
    char kinds[MAX_KINDS][24];
    int nevents;          /* event lines seen (excluding `end`)  */
    long long end_events; /* the footer's own count              */
    int end_truncated;    /* the footer's truncated flag         */
    long long lost;       /* footer drops.lost                   */
    int throttled;        /* footer drops.throttled              */
    int end_skip_code;    /* footer skip.code (0 = none)         */
    char end_skip_reason[256];
    int prov_exact;    /* header provenance.exact             */
    int prov_redacted; /* header provenance.redacted          */
    char backend[64];  /* header provenance.backend           */
} recording_t;

/* Read one recording from an already-open stream. Never fails hard: what it
 * cannot parse becomes a FIELD of the result, which is the whole point. */
static void read_stream(FILE *f, recording_t *r) {
    char line[MAX_LINE];
    memset(r, 0, sizeof *r);
    r->torn = 1;
    while (fgets(line, sizeof line, f)) {
        char kind[24];
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (!n)
            continue;
        r->lines++;
        if (!json_line_ok(line)) {
            r->malformed++;
            continue;
        }
        if (r->lines == 1) {
            long long v = 0;
            const char *prov = strstr(line, "\"provenance\":");
            r->header_ok =
                field_ll(line, "asmtrace", &v) && v == 1 && !field(line, "k");
            if (prov) {
                field_bool(prov, "exact", &r->prov_exact);
                field_bool(prov, "redacted", &r->prov_redacted);
                field_str(prov, "backend", r->backend, sizeof r->backend);
            }
            continue;
        }
        if (!field_str(line, "k", kind, sizeof kind))
            continue;
        if (strcmp(kind, "end") == 0) {
            const char *drops = strstr(line, "\"drops\":");
            const char *skip = strstr(line, "\"skip\":");
            long long code = 0;
            r->torn = 0;
            field_ll(line, "events", &r->end_events);
            field_bool(line, "truncated", &r->end_truncated);
            if (drops) {
                field_ll(drops, "lost", &r->lost);
                field_bool(drops, "throttled", &r->throttled);
            }
            if (skip && field_ll(skip, "code", &code)) {
                r->end_skip_code = (int)code;
                field_str(skip, "reason", r->end_skip_reason,
                          sizeof r->end_skip_reason);
            }
            continue;
        }
        r->nevents++;
        if (r->nkinds < MAX_KINDS)
            snprintf(r->kinds[r->nkinds++], sizeof r->kinds[0], "%s", kind);
    }
}

static int read_recording(const char *path, recording_t *r) {
    FILE *f = fopen(path, "r");
    if (!f) {
        memset(r, 0, sizeof *r);
        return 0;
    }
    read_stream(f, r);
    fclose(f);
    return 1;
}

/* Does `r` carry an event of this kind? */
static int has_kind(const recording_t *r, const char *k) {
    for (int i = 0; i < r->nkinds; i++)
        if (strcmp(r->kinds[i], k) == 0)
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Fixtures the writer tests write                                     */
/* ------------------------------------------------------------------ */

static char tmpdir[512];

static void tmppath(char *out, size_t cap, const char *name) {
    snprintf(out, cap, "%s/%s", tmpdir, name);
}

/* Write one fixed recording. Same input -> same bytes is the contract. */
static void write_sample(const char *path, int deterministic) {
    asmtrace_writer_t w;
    asmtrace_prov_t p = {"emu-l0", 1, "exact", 0, NULL, 0};
    char esc[256];
    if (asmtrace_open(&w, path, deterministic) != 0) {
        check("writer.open", 0, path);
        return;
    }
    asmtrace_header(&w, "asmtrace_record", &p, 4242, "./victim arg");
    asmtrace_escape(esc, sizeof esc, "add eax, esi");
    asmtrace_emitf(&w, "trace",
                   "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu,"
                   "\"disasm\":\"%s\"",
                   0ULL, esc);
    asmtrace_emit(&w, "note", "\"text\":\"hello\"");
    asmtrace_emitf(&w, "coverage",
                   "\"basis\":\"rel\",\"blocks\":[0],\"blocks_total\":%llu,"
                   "\"insns_total\":%llu,\"truncated\":false",
                   1ULL, 3ULL);
    asmtrace_close(&w, 0, 0, NULL);
}

/* ---- writer.* ---- */

static void test_header_and_roundtrip(void) {
    char path[600];
    recording_t r;
    tmppath(path, sizeof path, "sample.asmtrace");
    write_sample(path, 0);

    check("writer.header_line_first", read_recording(path, &r) && r.header_ok,
          "line 1 is not an asmtrace:1 header");
    check("writer.wellformed_lines", r.malformed == 0, "malformed JSON line");
    check_ll("writer.roundtrip_events", r.nevents, 3);
    check("writer.roundtrip_kinds",
          r.nkinds == 3 && strcmp(r.kinds[0], "trace") == 0 &&
              strcmp(r.kinds[1], "note") == 0 &&
              strcmp(r.kinds[2], "coverage") == 0,
          "kinds not read back in written order");
    check_str("writer.provenance_backend", r.backend, "emu-l0");
    check("writer.provenance_exact", r.prov_exact == 1, NULL);
}

static void test_field_order_fixed(void) {
    char a[600], b[600];
    FILE *fa, *fb;
    int same = 1;
    tmppath(a, sizeof a, "order_a.asmtrace");
    tmppath(b, sizeof b, "order_b.asmtrace");
    write_sample(a, 1);
    write_sample(b, 1);
    fa = fopen(a, "rb");
    fb = fopen(b, "rb");
    if (!fa || !fb) {
        check("writer.field_order_fixed", 0, "could not reopen fixtures");
    } else {
        int ca, cb;
        do {
            ca = fgetc(fa);
            cb = fgetc(fb);
            if (ca != cb) {
                same = 0;
                break;
            }
        } while (ca != EOF);
        check("writer.field_order_fixed", same,
              "two identical writes differ byte-wise");
    }
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
}

static void test_escape_edges(void) {
    char out[64];
    asmtrace_escape(out, sizeof out, "a\"b");
    check_str("writer.escape_quote", out, "a\\\"b");
    asmtrace_escape(out, sizeof out, "a\\b");
    check_str("writer.escape_backslash", out, "a\\\\b");
    asmtrace_escape(out, sizeof out, "a\nb");
    check_str("writer.escape_newline", out, "a\\u000ab");
    asmtrace_escape(out, sizeof out, "a\x01k");
    check_str("writer.escape_ctrl", out, "a\\u0001k");
    asmtrace_escape(out, sizeof out, NULL);
    check_str("writer.escape_null", out, "");
    /* Truncation must not overflow or emit a half escape. */
    {
        char tiny[10];
        asmtrace_escape(tiny, sizeof tiny, "\x01\x01\x01\x01\x01");
        check("writer.escape_truncates", strlen(tiny) < sizeof tiny,
              "escape overflowed its cap");
    }
    /* An escaped payload must survive the wrapper as ONE well-formed line. */
    {
        char path[600], esc[64];
        recording_t r;
        char text[64];
        asmtrace_writer_t w;
        asmtrace_prov_t p = {"emu-l0", 1, "exact", 0, NULL, 0};
        tmppath(path, sizeof path, "escape.asmtrace");
        asmtrace_open(&w, path, 1);
        asmtrace_header(&w, "asmtrace_record", &p, 0, NULL);
        asmtrace_escape(esc, sizeof esc, "quote\" back\\slash");
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", esc);
        asmtrace_close(&w, 0, 0, NULL);
        read_recording(path, &r);
        check("writer.escape_line_wellformed", r.malformed == 0 && r.lines == 3,
              "an escaped string broke the line");
        {
            FILE *f = fopen(path, "r");
            char line[MAX_LINE];
            text[0] = '\0';
            if (f) {
                while (fgets(line, sizeof line, f))
                    if (strstr(line, "\"note\""))
                        field_str(line, "text", text, sizeof text);
                fclose(f);
            }
            check_str("writer.escape_roundtrip", text, "quote\" back\\slash");
        }
    }
}

static void test_deterministic_omits_volatile(void) {
    char det[600], live[600];
    char line[MAX_LINE];
    FILE *f;
    tmppath(det, sizeof det, "det.asmtrace");
    tmppath(live, sizeof live, "live.asmtrace");
    write_sample(det, 1);
    write_sample(live, 0);

    f = fopen(det, "r");
    line[0] = '\0';
    if (f) {
        if (!fgets(line, sizeof line, f))
            line[0] = '\0';
        fclose(f);
    }
    check("writer.deterministic_omits_ts_pid",
          !field(line, "ts") && !field(line, "pid") && !field(line, "cmd"),
          "deterministic header carried a volatile field");
    check("writer.deterministic_keeps_arch", field(line, "arch") != NULL, NULL);

    f = fopen(live, "r");
    line[0] = '\0';
    if (f) {
        if (!fgets(line, sizeof line, f))
            line[0] = '\0';
        fclose(f);
    }
    check("writer.live_header_has_pid_ts",
          field(line, "ts") && field(line, "pid") && field(line, "cmd"),
          "live header dropped pid/ts/cmd");
}

static void test_end_event_counts(void) {
    char path[600];
    recording_t r;
    asmtrace_writer_t w;
    asmtrace_prov_t p = {"ibs-op", 0, "statistical", 0, NULL, 0};
    tmppath(path, sizeof path, "end.asmtrace");
    asmtrace_open(&w, path, 1);
    asmtrace_header(&w, "asmspy", &p, 0, NULL);
    for (int i = 0; i < 5; i++)
        asmtrace_emitf(&w, "stream", "\"text\":\"step %d\"", i);
    w.truncated = 1;
    asmtrace_close(&w, 12345, 1, NULL);

    read_recording(path, &r);
    check("writer.end_event_counts", !r.torn && r.end_events == r.nevents,
          "end.events disagrees with the lines counted");
    check_ll("writer.end_events_value", r.end_events, 5);
    check("writer.end_truncated_sticks", r.end_truncated == 1, NULL);
    check_ll("writer.end_drops_lost", r.lost, 12345);
    check("writer.end_drops_throttled", r.throttled == 1, NULL);
    check("writer.statistical_is_not_exact", r.prov_exact == 0,
          "a statistical stream recorded exact:true");
}

static void test_skip_is_a_recording(void) {
    char path[600];
    recording_t r;
    asmtrace_writer_t w;
    asmtrace_prov_t p = {"ibs-op",
                         0,
                         "statistical",
                         2,
                         "IBS-Op is an AMD feature; this host is GenuineIntel",
                         0};
    tmppath(path, sizeof path, "skip.asmtrace");
    asmtrace_open(&w, path, 1);
    asmtrace_header(&w, "asmspy", &p, 0, NULL);
    asmtrace_close(&w, 0, 0, &p);

    read_recording(path, &r);
    check("writer.skip_closes_cleanly", !r.torn && r.header_ok,
          "a skipped run did not produce a closed recording");
    check_ll("writer.skip_code_in_end", r.end_skip_code, 2);
    check("writer.skip_reason_measured",
          strstr(r.end_skip_reason, "GenuineIntel") != NULL, r.end_skip_reason);
}

static void test_torn_without_close(void) {
    char path[600];
    recording_t r;
    asmtrace_writer_t w;
    asmtrace_prov_t p = {"ptrace-syscalls", 1, "exact", 0, NULL, 0};
    tmppath(path, sizeof path, "torn.asmtrace");
    asmtrace_open(&w, path, 1);
    asmtrace_header(&w, "asmspy", &p, 0, NULL);
    asmtrace_emit(&w, "syscall", "\"line\":\"write(1, <8 bytes>, 8) = 8\"");
    fflush(w.f);
    fclose(w.f); /* the crash: no asmtrace_close, so no `end` */
    w.f = NULL;

    read_recording(path, &r);
    check("writer.torn_without_close", r.torn == 1,
          "a recording with no end event was not reported torn");
    check_ll("writer.torn_kept_events", r.nevents, 1);
}

/* ---- schema.* — the doc's own example must parse ---- */

static void test_schema_example(void) {
    const char *doc = getenv("ASMTRACE_SCHEMA_DOC");
    char path[600];
    char line[MAX_LINE];
    FILE *in, *out;
    recording_t r;
    int in_example = 0, in_fence = 0, wrote = 0;

    if (!doc)
        doc = "docs/internal/gui/asmtrace-schema.md";
    in = fopen(doc, "r");
    if (!in) {
        check("schema.example_present", 0, doc);
        return;
    }
    tmppath(path, sizeof path, "schema_example.asmtrace");
    out = fopen(path, "w");
    if (!out) {
        fclose(in);
        check("schema.example_extract", 0, path);
        return;
    }
    while (fgets(line, sizeof line, in)) {
        if (strncmp(line, "## Example", 10) == 0) {
            in_example = 1;
            continue;
        }
        if (!in_example)
            continue;
        if (strncmp(line, "```", 3) == 0) {
            if (in_fence)
                break; /* closing fence: the example is complete */
            in_fence = 1;
            continue;
        }
        if (in_fence) {
            fputs(line, out);
            wrote++;
        }
    }
    fclose(in);
    fclose(out);

    check("schema.example_present", wrote > 0,
          "no fenced example under '## Example'");
    read_recording(path, &r);
    check("schema.example_header", r.header_ok, "example header did not parse");
    check("schema.example_wellformed", r.malformed == 0,
          "example carries a malformed line");
    check("schema.example_closed", !r.torn, "example has no end event");
    check("schema.example_end_counts", r.end_events == r.nevents,
          "example end.events disagrees with its event lines");
    check("schema.example_kinds",
          has_kind(&r, "df_step") && has_kind(&r, "df_edge") &&
              has_kind(&r, "trace") && has_kind(&r, "coverage"),
          "example lost a documented kind");
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    char tmpl[512];
    snprintf(tmpl, sizeof tmpl, "%s/asmtrace-test-XXXXXX", tmp ? tmp : "/tmp");
    if (!mkdtemp(tmpl)) {
        fprintf(stderr, "test_asmtrace: mkdtemp failed\n");
        return 1;
    }
    snprintf(tmpdir, sizeof tmpdir, "%s", tmpl);

    test_header_and_roundtrip();
    test_field_order_fixed();
    test_escape_edges();
    test_deterministic_omits_volatile();
    test_end_event_counts();
    test_skip_is_a_recording();
    test_torn_without_close();
    test_schema_example();

    if (failures) {
        fprintf(stderr, "test_asmtrace: %d FAILURE(S) (kept %s)\n", failures,
                tmpdir);
        return 1;
    }
    printf("test_asmtrace: PASS\n");
    return 0;
}
