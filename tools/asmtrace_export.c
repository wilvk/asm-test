/*
 * asmtrace_export.c — .asmtrace -> commodity render formats (GUI Phase 1).
 *
 * Recordings are the unit of collaboration: capture .asmtrace in CI or a
 * container, render anywhere. Industry viewers already exist for the two
 * commodity render shapes, so this exports into them rather than rebuilding
 * their views, and it regenerates OFFLINE the two capture-time passthrough
 * exports (lcov, --tree DOT) so "capture in CI, run genhtml / dot later" needs
 * no re-run of the traced program.
 *
 *   asmtrace_export --speedscope [--out=FILE] REC.asmtrace
 *   asmtrace_export --chrome     [--heat-cap=N] [--out=FILE] REC.asmtrace
 *   asmtrace_export --lcov       [--name=SF]    [--out=FILE] REC.asmtrace
 *   asmtrace_export --dot-tree   [--out=FILE]   REC.asmtrace
 *
 *   exit 0 = wrote output; 1 = I/O or parse error; 2 = honest refusal.
 *
 * Dependency-free by construction: one TU, C11, libc only — no engine objects,
 * no Capstone (the schema's optional per-event "disasm" string is why), no JSON
 * library. It therefore builds on any host, which is the point: the exporter
 * must run wherever the recording landed.
 *
 * THE TIME AXIS IS AN EVENT ORDINAL, NOT TIME. No producer feed in this tree
 * carries timestamps, so every "ts"/"at" below is a per-tid (call events) or
 * per-stream (trace events) event ordinal. speedscope is told so via
 * "unit":"none"; the Chrome/Perfetto output says so in otherData.ts_unit. A
 * viewer that reads those numbers as microseconds is reading a number we never
 * measured — hence the honest labels rather than a fabricated clock.
 *
 * HONESTY RULES, each with a test in scripts/test-asmtrace-export.sh:
 *   - `survey` events are STATISTICAL hot-edge histograms. They are never
 *     exported as stacks, in any mode: an edge is evidence an edge was seen,
 *     never that a call happened at a point in time. A survey-only recording is
 *     an exit-2 refusal naming that reason, never an empty profile.
 *   - Truncation, drops and a torn (unterminated) recording always surface:
 *     " (truncated)" on each speedscope profile name, truncated/lost in the
 *     Chrome otherData, a "# truncated recording" DOT trailer, a stderr warning
 *     for lcov (whose format has no comment syntax to keep pristine).
 *   - `basis` ("rel" = offsets from the routine entry, "abs" = absolute
 *     addresses) is never defaulted and never merged: a recording mixing both
 *     is an exit-2 refusal in --lcov, because merging them mis-attributes every
 *     line of the coverage record.
 *   - A newer format major, or the reserved "zstd-frames" container, is refused
 *     BY NAME (exit 2) rather than best-efforted. A misparse of a compressed
 *     file is exactly the silent-wrong-answer this format exists to prevent.
 *
 * Format contract: docs/internal/gui/asmtrace-schema.md (kinds `call`, `trace`,
 * `coverage`, `survey`, `end`; the event-kind selector is "k"). Unknown kinds
 * and unknown fields are ignored and counted — the schema's forward-compat rule.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* growable buffers                                                     */
/* ------------------------------------------------------------------ */

static void oom(void) {
    fprintf(stderr, "asmtrace_export: out of memory\n");
    exit(1);
}

/* Grow *v to hold at least n elements of `esz`, doubling `cap`. */
static void grow(void **v, size_t *cap, size_t n, size_t esz) {
    if (n <= *cap)
        return;
    size_t c = *cap ? *cap : 16;
    while (c < n)
        c *= 2;
    void *p = realloc(*v, c * esz);
    if (p == NULL)
        oom();
    *v = p;
    *cap = c;
}

#define PUSH(arr, len, cap)                                                    \
    (grow((void **)&(arr), &(cap), (len) + 1, sizeof *(arr)), &(arr)[(len)++])

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p == NULL)
        oom();
    memcpy(p, s, n);
    return p;
}

/* Read one line into a growable buffer, without its newline. Returns the
 * length, or -1 at EOF with nothing read. getline() and strdup() are POSIX,
 * not C11, and this tool promises to build anywhere with libc alone. */
static long read_line(FILE *f, char **buf, size_t *cap) {
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        grow((void **)buf, cap, n + 2, 1);
        (*buf)[n++] = (char)c;
    }
    if (c == EOF && n == 0)
        return -1;
    grow((void **)buf, cap, n + 1, 1);
    (*buf)[n] = '\0';
    return (long)n;
}

/* ------------------------------------------------------------------ */
/* minimal flat-JSON scanning                                           */
/* ------------------------------------------------------------------ */
/*
 * Just enough JSON to walk one NDJSON object's top-level members and pull the
 * fields this exporter knows, skipping every other value STRUCTURALLY (brace /
 * bracket depth with in-string state) so an unknown nested field is ignored
 * rather than misparsed. A malformed line is an error, never a guess.
 */

static const char *g_parse_err; /* set by the scanner, printed by the caller */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

/* Advance past a JSON string starting at the opening quote. NULL on malformed. */
static const char *skip_string(const char *p) {
    if (*p != '"')
        return NULL;
    for (p++; *p; p++) {
        if (*p == '\\') {
            if (p[1] == '\0')
                return NULL;
            p++;
        } else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

/* Advance past any JSON value. NULL on malformed. */
static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {
                const char *e = skip_string(p);
                if (e == NULL)
                    return NULL;
                p = e - 1;
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']') {
                if (--depth == 0)
                    return p + 1;
            }
        }
        return NULL;
    }
    /* number / true / false / null */
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' &&
           *p != '\t' && *p != '\r' && *p != '\n')
        p++;
    return p == s ? NULL : p;
}

/*
 * Iterate the top-level members of the object at *pp (which must point at '{'
 * on the first call). Returns 1 with `key` filled and [*vb,*ve) spanning the
 * raw value, 0 at the closing brace, -1 on malformed input.
 */
static int json_member(const char **pp, char *key, size_t keycap,
                       const char **vb, const char **ve) {
    const char *p = skip_ws(*pp);
    if (*p == '{') {
        p = skip_ws(p + 1);
        if (*p == '}') {
            *pp = p + 1;
            return 0;
        }
    } else if (*p == ',') {
        p = skip_ws(p + 1);
    } else if (*p == '}') {
        *pp = p + 1;
        return 0;
    } else {
        g_parse_err = "expected '{', ',' or '}'";
        return -1;
    }
    if (*p != '"') {
        g_parse_err = "expected a member name";
        return -1;
    }
    const char *ks = p + 1;
    const char *kend = skip_string(p);
    if (kend == NULL) {
        g_parse_err = "unterminated member name";
        return -1;
    }
    size_t klen = (size_t)(kend - 1 - ks);
    if (klen >= keycap)
        klen = keycap - 1;
    memcpy(key, ks, klen);
    key[klen] = '\0';
    p = skip_ws(kend);
    if (*p != ':') {
        g_parse_err = "expected ':' after a member name";
        return -1;
    }
    p = skip_ws(p + 1);
    *vb = p;
    const char *e = skip_value(p);
    if (e == NULL) {
        g_parse_err = "malformed value";
        return -1;
    }
    *ve = e;
    *pp = skip_ws(e);
    if (**pp != ',' && **pp != '}') {
        g_parse_err = "expected ',' or '}' after a value";
        return -1;
    }
    return 1;
}

/* Decode a JSON string value into buf. 0 on success, -1 if not a string or an
 * escape is not one the schema's minimal escaper emits (\" \\ \/ \b \f \n \r \t
 * \uXXXX) — an unknown escape is loud, never dropped. */
static int json_str(const char *b, const char *e, char *buf, size_t cap) {
    if (b >= e || *b != '"') {
        g_parse_err = "expected a string value";
        return -1;
    }
    size_t o = 0;
    for (const char *p = b + 1; p < e - 1; p++) {
        if (o + 1 >= cap)
            break; /* the field is longer than any name this tool renders */
        if (*p != '\\') {
            buf[o++] = *p;
            continue;
        }
        p++;
        switch (*p) {
        case '"':
        case '\\':
        case '/':
            buf[o++] = *p;
            break;
        case 'b':
            buf[o++] = '\b';
            break;
        case 'f':
            buf[o++] = '\f';
            break;
        case 'n':
            buf[o++] = '\n';
            break;
        case 'r':
            buf[o++] = '\r';
            break;
        case 't':
            buf[o++] = '\t';
            break;
        case 'u': {
            /* The writer escapes only bytes < 0x20 this way, but decode the
             * whole BMP so a hand-authored fixture is not silently mangled. */
            unsigned cp = 0;
            for (int i = 1; i <= 4; i++) {
                char c = p[i];
                unsigned d;
                if (c >= '0' && c <= '9')
                    d = (unsigned)(c - '0');
                else if (c >= 'a' && c <= 'f')
                    d = (unsigned)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F')
                    d = (unsigned)(c - 'A' + 10);
                else {
                    g_parse_err = "bad \\u escape";
                    return -1;
                }
                cp = cp * 16 + d;
            }
            p += 4;
            if (cp < 0x80) {
                buf[o++] = (char)cp;
            } else if (cp < 0x800) {
                if (o + 2 >= cap)
                    break;
                buf[o++] = (char)(0xC0 | (cp >> 6));
                buf[o++] = (char)(0x80 | (cp & 0x3F));
            } else {
                if (o + 3 >= cap)
                    break;
                buf[o++] = (char)(0xE0 | (cp >> 12));
                buf[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                buf[o++] = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            g_parse_err = "unsupported string escape";
            return -1;
        }
    }
    buf[o] = '\0';
    return 0;
}

/* Decode a JSON integer value. -1 if it is not one. */
static int json_int(const char *b, const char *e, int64_t *out) {
    char tmp[32];
    size_t n = (size_t)(e - b);
    if (n == 0 || n >= sizeof tmp) {
        g_parse_err = "expected an integer value";
        return -1;
    }
    memcpy(tmp, b, n);
    tmp[n] = '\0';
    char *end = NULL;
    long long v = strtoll(tmp, &end, 10);
    if (end == tmp || (end != NULL && *end != '\0')) {
        g_parse_err = "expected an integer value";
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

/* Decode a JSON boolean value. -1 if it is not one. */
static int json_bool(const char *b, const char *e, int *out) {
    size_t n = (size_t)(e - b);
    if (n == 4 && memcmp(b, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (n == 5 && memcmp(b, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    g_parse_err = "expected a boolean value";
    return -1;
}

/* Append every integer element of a JSON array value to (*v,*len). -1 if the
 * value is not an array of integers. */
static int json_int_array(const char *b, const char *e, uint64_t **v,
                          size_t *len, size_t *cap) {
    if (b >= e || *b != '[') {
        g_parse_err = "expected an array value";
        return -1;
    }
    const char *p = skip_ws(b + 1);
    if (*p == ']')
        return 0;
    for (;;) {
        const char *vs = skip_ws(p);
        const char *vend = skip_value(vs);
        if (vend == NULL) {
            g_parse_err = "malformed array element";
            return -1;
        }
        int64_t n;
        if (json_int(vs, vend, &n) != 0)
            return -1;
        *(uint64_t *)PUSH(*v, *len, *cap) = (uint64_t)n;
        p = skip_ws(vend);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']')
            return 0;
        g_parse_err = "expected ',' or ']' in an array";
        return -1;
    }
}

/* ------------------------------------------------------------------ */
/* the recording, as this exporter needs it                             */
/* ------------------------------------------------------------------ */

#define NAMEMAX 192

typedef struct { /* one `call` event (schema: k=call) */
    int64_t tid;
    int64_t depth;
    uint64_t addr;
    char name[NAMEMAX];
    char module[NAMEMAX];
} call_ev;

typedef struct { /* one `trace` event (schema: k=trace) */
    uint64_t off;
    char disasm[NAMEMAX];
} trace_ev;

typedef struct {
    /* header */
    int64_t major;
    char container[64];
    int64_t pid;
    int have_pid;

    /* events */
    call_ev *calls;
    size_t ncalls, ccalls;
    trace_ev *traces;
    size_t ntraces, ctraces;
    uint64_t *blocks; /* union of every `coverage` event's blocks[] */
    size_t nblocks, cblocks;
    size_t nsurvey;
    size_t nunknown; /* kinds outside this exporter's interest */

    /* basis, never defaulted: the first one seen wins, a second one is a
     * conflict the caller refuses rather than merging (schema rule). */
    char basis[16];
    char basis_other[16];
    int basis_mixed;

    /* honesty */
    int has_end;
    int truncated;
    int64_t lost;
    int throttled;
    int torn; /* no `end` event: the producer died mid-record */
} recording;

static void rec_free(recording *r) {
    free(r->calls);
    free(r->traces);
    free(r->blocks);
}

/* Record one event's basis tag. Never defaults; flags a mix for the caller. */
static void note_basis(recording *r, const char *basis) {
    if (basis[0] == '\0')
        return;
    if (r->basis[0] == '\0') {
        snprintf(r->basis, sizeof r->basis, "%s", basis);
        return;
    }
    if (strcmp(r->basis, basis) != 0) {
        r->basis_mixed = 1;
        if (r->basis_other[0] == '\0')
            snprintf(r->basis_other, sizeof r->basis_other, "%s", basis);
    }
}

enum { EXIT_OK = 0, EXIT_ERR = 1, EXIT_REFUSE = 2 };

static int fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("asmtrace_export: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return EXIT_ERR;
}

static int refuse(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("asmtrace_export: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return EXIT_REFUSE;
}

/* Parse the header line. Returns 0, or an exit code on refusal/error. */
static int parse_header(recording *r, const char *line, int *rc) {
    const char *p = line;
    char key[64];
    const char *vb, *ve;
    int n;
    r->major = -1;
    while ((n = json_member(&p, key, sizeof key, &vb, &ve)) == 1) {
        if (strcmp(key, "asmtrace") == 0) {
            if (json_int(vb, ve, &r->major) != 0)
                goto bad;
        } else if (strcmp(key, "container") == 0) {
            if (json_str(vb, ve, r->container, sizeof r->container) != 0)
                goto bad;
        } else if (strcmp(key, "pid") == 0) {
            if (json_int(vb, ve, &r->pid) != 0)
                goto bad;
            r->have_pid = 1;
        }
        /* every other header field is ignored (forward compat) */
    }
    if (n < 0)
        goto bad;
    if (r->major < 0) {
        *rc = fail("line 1 is not an .asmtrace header (no \"asmtrace\" field)");
        return -1;
    }
    if (r->major > 1) {
        *rc = refuse("this recording is .asmtrace major %" PRId64
                     "; this exporter reads major 1 only — upgrade asm-test",
                     r->major);
        return -1;
    }
    if (r->container[0] != '\0' && strcmp(r->container, "ndjson") != 0) {
        *rc = refuse("\"%s\" container is not supported by this exporter — "
                     "only \"ndjson\" is implemented in v1",
                     r->container);
        return -1;
    }
    return 0;
bad:
    *rc = fail("line 1: %s", g_parse_err ? g_parse_err : "malformed header");
    return -1;
}

/* Parse one event line into `r`. Returns 0, or -1 with *rc set. */
static int parse_event(recording *r, const char *line, size_t lineno, int *rc) {
    const char *p = line;
    char key[64];
    const char *vb, *ve;
    int n;
    char kind[32] = {0};

    /* pass 1: the kind selector ("k" is always first, but do not rely on it) */
    while ((n = json_member(&p, key, sizeof key, &vb, &ve)) == 1) {
        if (strcmp(key, "k") == 0) {
            if (json_str(vb, ve, kind, sizeof kind) != 0)
                goto bad;
            break;
        }
    }
    if (n < 0)
        goto bad;
    if (kind[0] == '\0') {
        *rc = fail("line %zu: event has no \"k\" kind selector", lineno);
        return -1;
    }

    /* pass 2: the fields of the kinds this exporter renders */
    p = line;
    if (strcmp(kind, "call") == 0) {
        call_ev c = {0};
        while ((n = json_member(&p, key, sizeof key, &vb, &ve)) == 1) {
            if (strcmp(key, "tid") == 0) {
                if (json_int(vb, ve, &c.tid) != 0)
                    goto bad;
            } else if (strcmp(key, "depth") == 0) {
                if (json_int(vb, ve, &c.depth) != 0)
                    goto bad;
            } else if (strcmp(key, "addr") == 0) {
                int64_t a;
                if (json_int(vb, ve, &a) != 0)
                    goto bad;
                c.addr = (uint64_t)a;
            } else if (strcmp(key, "name") == 0) {
                if (json_str(vb, ve, c.name, sizeof c.name) != 0)
                    goto bad;
            } else if (strcmp(key, "module") == 0) {
                if (json_str(vb, ve, c.module, sizeof c.module) != 0)
                    goto bad;
            }
        }
        if (n < 0)
            goto bad;
        if (c.depth < 0)
            c.depth = 0;
        *(call_ev *)PUSH(r->calls, r->ncalls, r->ccalls) = c;
    } else if (strcmp(kind, "trace") == 0) {
        trace_ev t = {0};
        char basis[16] = {0};
        int have_off = 0;
        while ((n = json_member(&p, key, sizeof key, &vb, &ve)) == 1) {
            if (strcmp(key, "off") == 0) {
                int64_t o;
                if (json_int(vb, ve, &o) != 0)
                    goto bad;
                t.off = (uint64_t)o;
                have_off = 1;
            } else if (strcmp(key, "basis") == 0) {
                if (json_str(vb, ve, basis, sizeof basis) != 0)
                    goto bad;
            } else if (strcmp(key, "disasm") == 0) {
                if (json_str(vb, ve, t.disasm, sizeof t.disasm) != 0)
                    goto bad;
            }
        }
        if (n < 0)
            goto bad;
        if (!have_off) {
            *rc = fail("line %zu: a trace event carries no \"off\"", lineno);
            return -1;
        }
        if (basis[0] == '\0') {
            /* schema: basis is mandatory and may NEVER be defaulted */
            *rc = fail("line %zu: a trace event carries no \"basis\" — the "
                       "schema forbids defaulting it",
                       lineno);
            return -1;
        }
        note_basis(r, basis);
        *(trace_ev *)PUSH(r->traces, r->ntraces, r->ctraces) = t;
    } else if (strcmp(kind, "coverage") == 0) {
        char basis[16] = {0};
        while ((n = json_member(&p, key, sizeof key, &vb, &ve)) == 1) {
            if (strcmp(key, "blocks") == 0) {
                if (json_int_array(vb, ve, &r->blocks, &r->nblocks,
                                   &r->cblocks) != 0)
                    goto bad;
            } else if (strcmp(key, "basis") == 0) {
                if (json_str(vb, ve, basis, sizeof basis) != 0)
                    goto bad;
            } else if (strcmp(key, "truncated") == 0) {
                int t;
                if (json_bool(vb, ve, &t) != 0)
                    goto bad;
                if (t)
                    r->truncated = 1;
            }
        }
        if (n < 0)
            goto bad;
        if (basis[0] == '\0') {
            *rc = fail("line %zu: a coverage event carries no \"basis\" — the "
                       "schema forbids defaulting it",
                       lineno);
            return -1;
        }
        note_basis(r, basis);
    } else if (strcmp(kind, "survey") == 0) {
        r->nsurvey++;
    } else if (strcmp(kind, "end") == 0) {
        r->has_end = 1;
        while ((n = json_member(&p, key, sizeof key, &vb, &ve)) == 1) {
            if (strcmp(key, "truncated") == 0) {
                int t;
                if (json_bool(vb, ve, &t) != 0)
                    goto bad;
                if (t)
                    r->truncated = 1;
            } else if (strcmp(key, "drops") == 0) {
                const char *dp = vb;
                char dk[32];
                const char *dvb, *dve;
                int dn;
                while ((dn = json_member(&dp, dk, sizeof dk, &dvb, &dve)) ==
                       1) {
                    if (strcmp(dk, "lost") == 0) {
                        if (json_int(dvb, dve, &r->lost) != 0)
                            goto bad;
                    } else if (strcmp(dk, "throttled") == 0) {
                        if (json_bool(dvb, dve, &r->throttled) != 0)
                            goto bad;
                    }
                }
                if (dn < 0)
                    goto bad;
            }
        }
        if (n < 0)
            goto bad;
    } else {
        r->nunknown++; /* ignore-unknown-kinds, counted */
    }
    return 0;
bad:
    *rc = fail("line %zu: %s", lineno,
               g_parse_err ? g_parse_err : "malformed event");
    return -1;
}

/* Read the whole recording. Returns 0, or -1 with *rc set. */
static int load(const char *path, recording *r, int *rc) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        *rc = fail("cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    /* Refuse a compressed container by its magic before parsing a byte of it. */
    unsigned char magic[4] = {0};
    size_t got = fread(magic, 1, sizeof magic, f);
    if (got == 4 && magic[0] == 0x28 && magic[1] == 0xB5 && magic[2] == 0x2F &&
        magic[3] == 0xFD) {
        fclose(f);
        *rc = refuse("compressed container: not supported by this exporter yet "
                     "— decompress first");
        return -1;
    }
    rewind(f);

    char *line = NULL;
    size_t cap = 0;
    long len;
    size_t lineno = 0;
    int rv = 0;
    while ((len = read_line(f, &line, &cap)) >= 0) {
        while (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';
        lineno++;
        if (len == 0)
            continue;
        if (lineno == 1) {
            if (parse_header(r, line, rc) != 0) {
                rv = -1;
                break;
            }
            continue;
        }
        if (parse_event(r, line, lineno, rc) != 0) {
            rv = -1;
            break;
        }
    }
    free(line);
    fclose(f);
    if (rv == 0 && lineno == 0) {
        *rc = fail("%s is empty — an empty recording is a producer bug", path);
        return -1;
    }
    if (rv == 0)
        r->torn = !r->has_end;
    return rv;
}

/* ------------------------------------------------------------------ */
/* shared refusal + honesty helpers                                     */
/* ------------------------------------------------------------------ */

/* The recording is less than it looks: buffers filled, samples lost, or the
 * producer died mid-record. Every mode surfaces this in its own syntax. */
static int rec_incomplete(const recording *r) {
    return r->truncated || r->torn || r->lost > 0 || r->throttled;
}

/* One line naming why, for the modes that can carry prose. */
static void incomplete_reason(const recording *r, char *buf, size_t cap) {
    const char *what = r->torn ? "torn — no end footer, the producer died "
                                 "mid-record"
                               : (r->truncated ? "truncated — buffers filled"
                                               : "samples dropped");
    if (r->lost > 0 || r->throttled)
        snprintf(buf, cap, "%s; lost=%" PRId64 "%s", what, r->lost,
                 r->throttled ? ", throttled" : "");
    else
        snprintf(buf, cap, "%s", what);
}

/* Refuse a stack-shaped export over a statistical stream, naming the reason. */
static int refuse_no_calls(const recording *r, const char *mode) {
    if (r->nsurvey > 0)
        return refuse(
            "refusing %s: no call-tree events in this recording; survey events "
            "are statistical histograms, not stacks (edges are not stacks)",
            mode);
    return refuse("refusing %s: no call-tree events in this recording", mode);
}

/* ------------------------------------------------------------------ */
/* JSON output escaping (the schema's minimal escaper, mirrored)         */
/* ------------------------------------------------------------------ */

static void jout(FILE *o, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            fprintf(o, "\\%c", c);
        else if (c < 0x20)
            fprintf(o, "\\u%04x", c);
        else
            fputc((int)c, o);
    }
}

/* ------------------------------------------------------------------ */
/* the enter-only -> open/close synthesis shared by speedscope + chrome */
/* ------------------------------------------------------------------ */
/*
 * The tree engine emits ENTRIES ONLY (cli/asmspy.h: asmspy_tree_sink has no
 * leave callback), so closes are synthesized from depth transitions using the
 * shadow-stack discipline the engine itself used to compute those depths:
 * per tid keep a stack of open frames (frame, depth); at a call event of depth
 * d and per-tid ordinal t, close every open frame with depth >= d (innermost
 * first), then open this one at t. A depth jumping deeper by more than 1 is
 * legal (--focus re-bases depths) — just push.
 */

typedef struct {
    int64_t tid;
    size_t *idx; /* indices into recording.calls, in order, for this tid */
    size_t n, cap;
} tidgroup;

typedef struct {
    tidgroup *g;
    size_t n, cap;
} tidgroups;

static tidgroup *tid_find(tidgroups *gs, int64_t tid) {
    for (size_t i = 0; i < gs->n; i++)
        if (gs->g[i].tid == tid)
            return &gs->g[i];
    tidgroup *g = PUSH(gs->g, gs->n, gs->cap);
    memset(g, 0, sizeof *g);
    g->tid = tid;
    return g;
}

static void tid_group(const recording *r, tidgroups *gs) {
    memset(gs, 0, sizeof *gs);
    for (size_t i = 0; i < r->ncalls; i++) {
        tidgroup *g = tid_find(gs, r->calls[i].tid);
        *(size_t *)PUSH(g->idx, g->n, g->cap) = i;
    }
}

static void tid_free(tidgroups *gs) {
    for (size_t i = 0; i < gs->n; i++)
        free(gs->g[i].idx);
    free(gs->g);
}

/* One synthesized open or close. */
typedef struct {
    int open;    /* 1 = open ("O"/"B"), 0 = close ("C"/"E") */
    size_t call; /* index into recording.calls */
    uint64_t at; /* the per-tid event ordinal */
} oc_ev;

/* Synthesize the open/close sequence for one tid group. */
static void synth_oc(const recording *r, const tidgroup *g, oc_ev **out,
                     size_t *n, size_t *cap) {
    struct {
        size_t call;
        int64_t depth;
    } stack[256];
    size_t sp = 0;
    *n = 0;
    for (size_t t = 0; t < g->n; t++) {
        size_t ci = g->idx[t];
        int64_t d = r->calls[ci].depth;
        while (sp > 0 && stack[sp - 1].depth >= d) { /* innermost first */
            sp--;
            oc_ev *e = PUSH(*out, *n, *cap);
            e->open = 0;
            e->call = stack[sp].call;
            e->at = (uint64_t)t;
        }
        oc_ev *e = PUSH(*out, *n, *cap);
        e->open = 1;
        e->call = ci;
        e->at = (uint64_t)t;
        if (sp < sizeof stack / sizeof stack[0]) {
            stack[sp].call = ci;
            stack[sp].depth = d;
            sp++;
        }
    }
    uint64_t end = (uint64_t)g->n; /* last ordinal + 1 */
    while (sp > 0) {
        sp--;
        oc_ev *e = PUSH(*out, *n, *cap);
        e->open = 0;
        e->call = stack[sp].call;
        e->at = end;
    }
}

/* ------------------------------------------------------------------ */
/* --speedscope                                                         */
/* ------------------------------------------------------------------ */

/* Frame table: one entry per distinct "name [module]". */
typedef struct {
    char **v;
    size_t n, cap;
} frames;

static size_t frame_id(frames *f, const call_ev *c) {
    char lbl[2 * NAMEMAX + 8];
    snprintf(lbl, sizeof lbl, "%s [%s]", c->name[0] ? c->name : "?",
             c->module[0] ? c->module : "?");
    for (size_t i = 0; i < f->n; i++)
        if (strcmp(f->v[i], lbl) == 0)
            return i;
    char **slot = PUSH(f->v, f->n, f->cap);
    *slot = xstrdup(lbl);
    return f->n - 1;
}

static const char *basename_of(const char *path) {
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int emit_speedscope(const recording *r, const char *path, FILE *o) {
    if (r->ncalls == 0)
        return refuse_no_calls(r, "--speedscope");

    tidgroups gs;
    tid_group(r, &gs);
    frames f = {0};
    oc_ev *ev = NULL;
    size_t nev = 0, cev = 0;

    /* profiles are built into memory first: shared.frames must be complete
     * before it is printed, and the frame table is filled while walking. */
    typedef struct {
        int64_t tid;
        oc_ev *ev;
        size_t n;
        uint64_t endv;
    } prof;
    prof *ps = calloc(gs.n ? gs.n : 1, sizeof *ps);
    if (ps == NULL)
        oom();
    for (size_t i = 0; i < gs.n; i++) {
        nev = 0;
        synth_oc(r, &gs.g[i], &ev, &nev, &cev);
        ps[i].tid = gs.g[i].tid;
        ps[i].n = nev;
        ps[i].endv = (uint64_t)gs.g[i].n;
        ps[i].ev = malloc(nev ? nev * sizeof *ev : 1);
        if (ps[i].ev == NULL)
            oom();
        memcpy(ps[i].ev, ev, nev * sizeof *ev);
        for (size_t k = 0; k < nev; k++)
            (void)frame_id(&f, &r->calls[ps[i].ev[k].call]);
    }

    const int trunc = rec_incomplete(r);
    fprintf(o, "{\"$schema\": \"https://www.speedscope.app/file-format-schema"
               ".json\",\n");
    fprintf(o, " \"name\": \"");
    jout(o, basename_of(path));
    fprintf(o, "\", \"exporter\": \"asmtrace_export\", "
               "\"activeProfileIndex\": 0,\n");
    fprintf(o, " \"shared\": {\"frames\": [");
    for (size_t i = 0; i < f.n; i++) {
        fprintf(o, "%s{\"name\": \"", i ? ", " : "");
        jout(o, f.v[i]);
        fprintf(o, "\"}");
    }
    fprintf(o, "]},\n");
    fprintf(o, " \"profiles\": [");
    for (size_t i = 0; i < gs.n; i++) {
        char pname[64];
        snprintf(pname, sizeof pname, "tid %" PRId64 "%s", ps[i].tid,
                 trunc ? " (truncated)" : "");
        fprintf(o, "%s\n  {\"type\": \"evented\", \"name\": \"", i ? "," : "");
        jout(o, pname);
        fprintf(o,
                "\", \"unit\": \"none\", \"startValue\": 0, "
                "\"endValue\": %" PRIu64 ",\n   \"events\": [",
                ps[i].endv);
        for (size_t k = 0; k < ps[i].n; k++)
            fprintf(o,
                    "%s{\"type\": \"%s\", \"frame\": %zu, \"at\": %" PRIu64 "}",
                    k ? ", " : "", ps[i].ev[k].open ? "O" : "C",
                    frame_id(&f, &r->calls[ps[i].ev[k].call]), ps[i].ev[k].at);
        fprintf(o, "]}");
    }
    fprintf(o, "%s]}\n", gs.n ? "\n " : "");

    for (size_t i = 0; i < gs.n; i++)
        free(ps[i].ev);
    free(ps);
    for (size_t i = 0; i < f.n; i++)
        free(f.v[i]);
    free(f.v);
    free(ev);
    tid_free(&gs);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* --chrome (Chrome Trace Event format; loads in Perfetto)              */
/* ------------------------------------------------------------------ */

static int emit_chrome(const recording *r, FILE *o, long heat_cap) {
    if (r->ncalls == 0 && r->ntraces == 0) {
        if (r->nsurvey > 0)
            return refuse("refusing --chrome: no call-tree or trace events in "
                          "this recording; survey events are statistical "
                          "histograms, not stacks (edges are not stacks)");
        return refuse("refusing --chrome: no call-tree or trace events in this "
                      "recording");
    }

    const int64_t pid = r->have_pid ? r->pid : 1;
    tidgroups gs;
    tid_group(r, &gs);
    oc_ev *ev = NULL;
    size_t nev = 0, cev = 0;
    int first = 1;

    fprintf(o, "{\"traceEvents\": [");
    for (size_t i = 0; i < gs.n; i++) {
        nev = 0;
        synth_oc(r, &gs.g[i], &ev, &nev, &cev);
        for (size_t k = 0; k < nev; k++) {
            const call_ev *c = &r->calls[ev[k].call];
            char lbl[2 * NAMEMAX + 8];
            snprintf(lbl, sizeof lbl, "%s [%s]", c->name[0] ? c->name : "?",
                     c->module[0] ? c->module : "?");
            fprintf(o, "%s\n  {\"name\": \"", first ? "" : ",");
            first = 0;
            jout(o, lbl);
            fprintf(o,
                    "\", \"cat\": \"tree\", \"ph\": \"%s\", \"ts\": %" PRIu64
                    ", \"pid\": %" PRId64 ", \"tid\": %" PRId64 "",
                    ev[k].open ? "B" : "E", ev[k].at, pid, c->tid);
            if (ev[k].open)
                fprintf(o,
                        ", \"args\": {\"addr\": \"0x%" PRIx64
                        "\", \"depth\": %" PRId64 "}",
                        c->addr, c->depth);
            fprintf(o, "}");
        }
    }
    free(ev);
    tid_free(&gs);

    /* counter track: cumulative per-offset heat, one series per offset. */
    uint64_t *hoff = NULL;
    uint64_t *hcnt = NULL;
    uint64_t *hdrop = NULL; /* offsets refused past the cap, for the count */
    size_t nh = 0, ndrop = 0;
    if (r->ntraces) {
        hoff = calloc(r->ntraces, sizeof *hoff);
        hcnt = calloc(r->ntraces, sizeof *hcnt);
        hdrop = calloc(r->ntraces, sizeof *hdrop);
        if (hoff == NULL || hcnt == NULL || hdrop == NULL)
            oom();
    }
    for (size_t i = 0; i < r->ntraces; i++) {
        uint64_t off = r->traces[i].off;
        size_t k;
        for (k = 0; k < nh && hoff[k] != off; k++)
            ;
        if (k == nh) {
            if (heat_cap > 0 && (long)nh >= heat_cap) {
                /* Past the cap. Dropped LOUDLY: otherData counts the DISTINCT
                 * offsets that never got a series, so a reader knows exactly
                 * how much of the heat track is missing. */
                size_t d;
                for (d = 0; d < ndrop && hdrop[d] != off; d++)
                    ;
                if (d == ndrop)
                    hdrop[ndrop++] = off;
                continue;
            }
            hoff[nh] = off;
            hcnt[nh] = 0;
            nh++;
        }
        hcnt[k]++;
        fprintf(o,
                "%s\n  {\"name\": \"heat\", \"cat\": \"trace\", \"ph\": \"C\", "
                "\"ts\": %zu, \"pid\": %" PRId64 ", \"args\": {\"0x%" PRIx64
                "\": %" PRIu64 "}}",
                first ? "" : ",", i, pid, off, hcnt[k]);
        first = 0;
    }
    const int64_t dropped = (int64_t)ndrop;
    free(hoff);
    free(hcnt);
    free(hdrop);

    fprintf(o, "%s],\n \"displayTimeUnit\": \"ms\",\n", first ? "" : "\n ");
    fprintf(o,
            " \"otherData\": {\"ts_unit\": \"event ordinal — the producers "
            "record no timestamps\", \"truncated\": %s, \"lost\": %" PRId64
            ", \"heat_offsets_dropped\": %" PRId64 "}}\n",
            (r->truncated || r->torn) ? "true" : "false", r->lost, dropped);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* --lcov                                                               */
/* ------------------------------------------------------------------ */

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static int emit_lcov(const recording *r, FILE *o, const char *name) {
    if (r->basis_mixed)
        return refuse(
            "refusing --lcov: this recording mixes address bases (\"%s\" and "
            "\"%s\") — region-relative offsets and absolute addresses must "
            "never merge into one coverage record; re-record, or split the "
            "streams",
            r->basis, r->basis_other);

    /* Distinct block offsets: the coverage events' union, else the trace
     * stream's offsets (a trace-only recording still yields block-granular
     * coverage of what it recorded). */
    uint64_t *v = NULL;
    size_t n = 0, cap = 0;
    if (r->nblocks) {
        grow((void **)&v, &cap, r->nblocks, sizeof *v);
        memcpy(v, r->blocks, r->nblocks * sizeof *v);
        n = r->nblocks;
    } else {
        for (size_t i = 0; i < r->ntraces; i++)
            *(uint64_t *)PUSH(v, n, cap) = r->traces[i].off;
    }
    if (n == 0) {
        free(v);
        return refuse(
            "refusing --lcov: this recording has neither coverage nor "
            "trace events — there is nothing to attribute");
    }
    qsort(v, n, sizeof *v, cmp_u64);
    size_t u = 0;
    for (size_t i = 0; i < n; i++)
        if (u == 0 || v[u - 1] != v[i])
            v[u++] = v[i];

    /* Byte-identical to emu_trace_lcov (src/trace.c) — one lcov writer shape. */
    fprintf(o, "TN:\n");
    fprintf(o, "SF:%s\n", name != NULL ? name : "routine");
    for (size_t i = 0; i < u; i++)
        fprintf(o, "DA:%" PRIu64 ",1\n", v[i]);
    fprintf(o, "LF:%zu\nLH:%zu\n", u, u);
    fprintf(o, "end_of_record\n");
    free(v);

    if (rec_incomplete(r)) {
        /* lcov has no comment syntax, so the honesty channel is stderr — the
         * format stays pristine for genhtml. */
        char why[128];
        incomplete_reason(r, why, sizeof why);
        fprintf(stderr,
                "asmtrace_export: WARNING: this recording is incomplete (%s) — "
                "the coverage below is a LOWER BOUND, not the full block set\n",
                why);
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* --dot-tree                                                           */
/* ------------------------------------------------------------------ */

/* Escape `s` for a Graphviz DOT double-quoted string (only " and \ matter).
 * Copied from cli/asmspy.c's dot_escape — asmspy.c is a program, not a
 * library, and must not be linked into this dependency-free TU. */
static void dot_escape(const char *s, char *out, size_t cap) {
    size_t o = 0;
    for (; *s && o + 2 < cap; s++) {
        if (*s == '"' || *s == '\\')
            out[o++] = '\\';
        out[o++] = *s;
    }
    out[o] = '\0';
}

/* Node fill colour, mirroring cli/asmspy.c's tree_fill: the tree records carry
 * no internal/external split, so this colours what they do know. */
static const char *tree_fill(const char *module) {
    if (strcmp(module, "jit") == 0)
        return "#fff3c4"; /* JIT/managed */
    if (strcmp(module, "?") == 0)
        return "#ffe0e0"; /* unresolved */
    return "#e8f0ff";     /* named (exe or library) */
}

static int emit_dot_tree(const recording *r, FILE *o) {
    if (r->ncalls == 0)
        return refuse_no_calls(r, "--dot-tree");

    typedef struct {
        uint64_t addr;
        const call_ev *rec;
        unsigned long long entered;
    } dnode;
    typedef struct {
        uint64_t from, to;
        unsigned long long count;
    } dedge;
    typedef struct {
        int64_t tid;
        uint64_t at[64];
    } dstack;
    dnode *nodes = calloc(r->ncalls, sizeof *nodes);
    dedge *edges = calloc(r->ncalls, sizeof *edges);
    dstack *stacks = calloc(r->ncalls, sizeof *stacks);
    if (nodes == NULL || edges == NULL || stacks == NULL)
        oom();
    size_t nn = 0, ne = 0, ns = 0;

    for (size_t i = 0; i < r->ncalls; i++) {
        const call_ev *c = &r->calls[i];
        size_t k;
        for (k = 0; k < nn && nodes[k].addr != c->addr; k++)
            ;
        if (k == nn) {
            nodes[nn].addr = c->addr;
            nn++;
        }
        nodes[k].rec = c; /* newest naming wins (a JIT may recompile) */
        nodes[k].entered++;

        for (k = 0; k < ns && stacks[k].tid != c->tid; k++)
            ;
        if (k == ns) {
            stacks[ns].tid = c->tid;
            ns++;
        }
        int d = c->depth < 63 ? (int)c->depth : 63;
        uint64_t parent = d > 0 ? stacks[k].at[d - 1] : 0;
        stacks[k].at[d] = c->addr;
        if (parent) {
            size_t e;
            for (e = 0;
                 e < ne && !(edges[e].from == parent && edges[e].to == c->addr);
                 e++)
                ;
            if (e == ne) {
                edges[ne].from = parent;
                edges[ne].to = c->addr;
                ne++;
            }
            edges[e].count++;
        }
    }

    fprintf(o, "digraph asmspy {\n  rankdir=LR;\n  node [shape=box, "
               "style=filled, fontname=monospace, fontsize=10];\n");
    for (size_t i = 0; i < nn; i++) {
        char lbl[4 * NAMEMAX];
        const char *mod = nodes[i].rec->module[0] ? nodes[i].rec->module : "?";
        dot_escape(nodes[i].rec->name[0] ? nodes[i].rec->name : "?", lbl,
                   sizeof lbl);
        fprintf(o,
                "  \"0x%" PRIx64 "\" [label=\"%s\\n[%s] entered=%llu\","
                " fillcolor=\"%s\"];\n",
                nodes[i].addr, lbl, mod, nodes[i].entered, tree_fill(mod));
    }
    for (size_t i = 0; i < ne; i++)
        fprintf(o,
                "  \"0x%" PRIx64 "\" -> \"0x%" PRIx64 "\" [label=\"%llu\"];\n",
                edges[i].from, edges[i].to, edges[i].count);
    fprintf(o, "}\n");
    if (rec_incomplete(r)) {
        char why[128];
        incomplete_reason(r, why, sizeof why);
        fprintf(o, "# truncated recording: %s — this graph is a LOWER BOUND\n",
                why);
    }
    free(nodes);
    free(edges);
    free(stacks);
    return EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

static void usage(FILE *o) {
    fprintf(o,
            "usage: asmtrace_export MODE [OPTIONS] REC.asmtrace\n"
            "  --speedscope        evented call profile (speedscope.app)\n"
            "  --chrome            Chrome Trace Event JSON (ui.perfetto.dev)\n"
            "  --lcov [--name=SF]  block-offset lcov record (genhtml)\n"
            "  --dot-tree          the --tree call graph as Graphviz DOT\n"
            "  --out=FILE          write there instead of stdout\n"
            "  --heat-cap=N        --chrome: distinct heat offsets, 0 = no cap"
            " (default 256)\n"
            "exit 0 = wrote output; 1 = I/O or parse error; 2 = honest "
            "refusal\n");
}

int main(int argc, char **argv) {
    const char *mode = NULL, *out = NULL, *name = NULL, *path = NULL;
    long heat_cap = 256;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--speedscope") == 0 || strcmp(a, "--chrome") == 0 ||
            strcmp(a, "--lcov") == 0 || strcmp(a, "--dot-tree") == 0) {
            if (mode != NULL) {
                usage(stderr);
                return fail("pick exactly one mode (%s and %s given)", mode, a);
            }
            mode = a;
        } else if (strncmp(a, "--out=", 6) == 0) {
            out = a + 6;
        } else if (strncmp(a, "--name=", 7) == 0) {
            name = a + 7;
        } else if (strncmp(a, "--heat-cap=", 11) == 0) {
            heat_cap = strtol(a + 11, NULL, 10);
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return EXIT_OK;
        } else if (a[0] == '-') {
            usage(stderr);
            return fail("unknown option %s", a);
        } else if (path == NULL) {
            path = a;
        } else {
            usage(stderr);
            return fail("one recording at a time (%s and %s given)", path, a);
        }
    }
    if (mode == NULL || path == NULL) {
        usage(stderr);
        return fail("a mode and one REC.asmtrace are required");
    }

    recording r = {0};
    int rc = EXIT_OK;
    if (load(path, &r, &rc) != 0) {
        rec_free(&r);
        return rc;
    }

    FILE *o = stdout;
    if (out != NULL && (o = fopen(out, "wb")) == NULL) {
        rec_free(&r);
        return fail("cannot write %s: %s", out, strerror(errno));
    }

    if (strcmp(mode, "--speedscope") == 0)
        rc = emit_speedscope(&r, path, o);
    else if (strcmp(mode, "--chrome") == 0)
        rc = emit_chrome(&r, o, heat_cap);
    else if (strcmp(mode, "--lcov") == 0)
        rc = emit_lcov(&r, o, name);
    else
        rc = emit_dot_tree(&r, o);

    if (o != stdout) {
        if (fclose(o) != 0 && rc == EXIT_OK)
            rc = fail("write failed on %s: %s", out, strerror(errno));
    } else if (fflush(o) != 0 && rc == EXIT_OK) {
        rc = fail("write failed: %s", strerror(errno));
    }
    rec_free(&r);
    return rc;
}
