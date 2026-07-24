/*
 * asmtrace_ndjson.c — the `.asmtrace` NDJSON writer (see asmtrace_ndjson.h).
 *
 * Contract: docs/internal/gui/asmtrace-schema.md.
 */
#include "asmtrace_ndjson.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "asmtest.h" /* ASMTEST_VERSION (macro only; no link dependency) */

/* The RECORDING HOST's architecture, for the header's "arch" field. This is a
 * fact about where the bytes were produced, so it is compiled in rather than
 * passed by a caller that could get it wrong. */
#if defined(__x86_64__)
#define ASMTRACE_ARCH "x86_64"
#elif defined(__aarch64__)
#define ASMTRACE_ARCH "aarch64"
#elif defined(__riscv) && __riscv_xlen == 64
#define ASMTRACE_ARCH "riscv64"
#elif defined(__arm__)
#define ASMTRACE_ARCH "arm32"
#elif defined(__i386__)
#define ASMTRACE_ARCH "i386"
#else
#define ASMTRACE_ARCH "unknown"
#endif

void asmtrace_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (!dst || cap == 0)
        return;
    /* Reserve room for the widest single expansion (\u00xx = 6) + NUL. */
    for (; src && *src && o + 7 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

int asmtrace_open_file(asmtrace_writer_t *w, FILE *f, int deterministic) {
    memset(w, 0, sizeof *w);
    w->f = f;
    w->deterministic = deterministic;
    w->owns_file = 0;
    return 0;
}

int asmtrace_open(asmtrace_writer_t *w, const char *path, int deterministic) {
    FILE *f = fopen(path, "w");
    memset(w, 0, sizeof *w);
    if (!f)
        return -1;
    w->f = f;
    w->deterministic = deterministic;
    w->owns_file = 1;
    return 0;
}

/* Emit the provenance object (no surrounding braces omitted — this writes the
 * whole {...}). Field order is normative; optional fields are OMITTED, never
 * null. */
static void prov_line(FILE *f, const asmtrace_prov_t *p) {
    char esc[512];
    fputs("{\"backend\":\"", f);
    asmtrace_escape(esc, sizeof esc, p->backend ? p->backend : "unknown");
    fputs(esc, f);
    fprintf(f, "\",\"exact\":%s,\"trust\":\"", p->exact ? "true" : "false");
    asmtrace_escape(esc, sizeof esc, p->trust ? p->trust : "exact");
    fputs(esc, f);
    fputc('"', f);
    if (p->skip_code) {
        asmtrace_escape(esc, sizeof esc, p->skip_reason ? p->skip_reason : "");
        fprintf(f, ",\"skip\":{\"code\":%d,\"reason\":\"%s\"}", p->skip_code,
                esc);
    }
    if (p->redacted)
        fputs(",\"redacted\":true", f);
    fputc('}', f);
}

int asmtrace_header(asmtrace_writer_t *w, const char *producer,
                    const asmtrace_prov_t *prov, long pid, const char *cmd) {
    static const asmtrace_prov_t unknown = {"unknown", 1, "exact", 0, NULL, 0};
    char esc[1024];
    if (!w || !w->f)
        return -1;
    fputs("{\"asmtrace\":1,\"container\":\"ndjson\",\"producer\":{\"name\":\"",
          w->f);
    asmtrace_escape(esc, sizeof esc, producer ? producer : "asmspy");
    fputs(esc, w->f);
    fputs("\",\"version\":\"" ASMTEST_VERSION "\"},\"provenance\":", w->f);
    prov_line(w->f, prov ? prov : &unknown);
    fputs(",\"arch\":\"" ASMTRACE_ARCH "\"", w->f);
    if (!w->deterministic) {
        if (pid > 0)
            fprintf(w->f, ",\"pid\":%ld", pid);
        if (cmd) {
            asmtrace_escape(esc, sizeof esc, cmd);
            fprintf(w->f, ",\"cmd\":\"%s\"", esc);
        }
        fprintf(w->f, ",\"ts\":%lld", (long long)time(NULL));
    }
    if (fputs("}\n", w->f) == EOF)
        w->err = 1;
    return w->err ? -1 : 0;
}

int asmtrace_emit(asmtrace_writer_t *w, const char *kind, const char *body) {
    if (!w || !w->f)
        return -1;
    if (body && *body)
        fprintf(w->f, "{\"k\":\"%s\",%s}\n", kind, body);
    else
        fprintf(w->f, "{\"k\":\"%s\"}\n", kind);
    if (ferror(w->f))
        w->err = 1;
    w->events++;
    return w->err ? -1 : 0;
}

int asmtrace_emitf(asmtrace_writer_t *w, const char *kind, const char *fmt,
                   ...) {
    va_list ap;
    if (!w || !w->f)
        return -1;
    fprintf(w->f, "{\"k\":\"%s\",", kind);
    va_start(ap, fmt);
    vfprintf(w->f, fmt, ap);
    va_end(ap);
    fputs("}\n", w->f);
    if (ferror(w->f))
        w->err = 1;
    w->events++;
    return w->err ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Data-flow body builders (field order lives HERE and nowhere else)    */
/* ------------------------------------------------------------------ */

/* bounded append; returns the new length, never past cap-1. */
static size_t bp(char *b, size_t cap, size_t o, const char *fmt, ...) {
    va_list ap;
    int k;
    if (o + 1 >= cap)
        return cap ? cap - 1 : 0;
    va_start(ap, fmt);
    k = vsnprintf(b + o, cap - o, fmt, ap);
    va_end(ap);
    if (k < 0)
        return o;
    o += (size_t)k;
    return o > cap - 1 ? cap - 1 : o;
}

/* The at_loc_kind_t enum as the schema's token: a reader must never have to
 * know this project's enum NUMBERING to tell an absolute address from a
 * routine-relative one. */
static const char *loc_space(at_loc_kind_t k) {
    switch (k) {
    case AT_LOC_REG:
        return "reg";
    case AT_LOC_MEM_ABS:
        return "abs";
    case AT_LOC_MEM_OFF:
        return "off";
    default:
        return "reg";
    }
}

/* One operand record: space, [memory addressing terms], size, write,
 * value_valid, [wide], [value]. Memory terms are omitted for a register and
 * `value` is omitted when it was not captured (or spilled wide) — "not
 * measured" must not render as a zero. */
static size_t op_body(char *b, size_t cap, size_t o, const at_val_rec_t *r) {
    o = bp(b, cap, o, "{\"space\":\"%s\"", loc_space(r->kind));
    if (r->kind == AT_LOC_REG) {
        o = bp(b, cap, o, ",\"reg\":%u", r->reg);
    } else {
        o = bp(b, cap, o,
               ",\"reg\":%u,\"base\":%u,\"index\":%u,\"scale\":%d,"
               "\"disp\":%lld,\"addr\":%llu",
               r->reg, r->base, r->index, r->scale, (long long)r->disp,
               (unsigned long long)r->addr);
    }
    o = bp(b, cap, o, ",\"size\":%u,\"write\":%s,\"value_valid\":%s", r->size,
           r->is_write ? "true" : "false", r->value_valid ? "true" : "false");
    if (r->wide)
        o = bp(b, cap, o, ",\"wide\":true");
    else if (r->value_valid)
        o = bp(b, cap, o, ",\"value\":%llu", (unsigned long long)r->value);
    return bp(b, cap, o, "}");
}

size_t asmtrace_df_step_body(char *dst, size_t cap, unsigned step, uint64_t off,
                             const char *disasm, const at_val_rec_t *recs,
                             size_t n) {
    size_t o = 0;
    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    o = bp(dst, cap, o, "\"step\":%u,\"off\":%llu", step,
           (unsigned long long)off);
    if (disasm && *disasm) {
        char esc[512];
        asmtrace_escape(esc, sizeof esc, disasm);
        o = bp(dst, cap, o, ",\"disasm\":\"%s\"", esc);
    }
    o = bp(dst, cap, o, ",\"ops\":[");
    for (size_t i = 0; i < n; i++) {
        if (i)
            o = bp(dst, cap, o, ",");
        o = op_body(dst, cap, o, &recs[i]);
    }
    return bp(dst, cap, o, "]");
}

size_t asmtrace_df_edge_body(char *dst, size_t cap,
                             const asmtest_defuse_edge_t *e) {
    size_t o = 0;
    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    o = bp(dst, cap, o, "\"from\":%u,\"to\":%u,\"loc\":", e->from_step,
           e->to_step);
    return op_body(dst, cap, o, &e->loc);
}

int asmtrace_close(asmtrace_writer_t *w, unsigned long long lost, int throttled,
                   const asmtrace_prov_t *skip_update) {
    char esc[512];
    int rc;
    if (!w || !w->f)
        return 0;
    fprintf(w->f,
            "{\"k\":\"end\",\"events\":%llu,\"truncated\":%s,\"drops\":{"
            "\"lost\":%llu,\"throttled\":%s}",
            w->events, w->truncated ? "true" : "false", lost,
            throttled ? "true" : "false");
    if (skip_update && skip_update->skip_code) {
        asmtrace_escape(esc, sizeof esc,
                        skip_update->skip_reason ? skip_update->skip_reason
                                                 : "");
        fprintf(w->f, ",\"skip\":{\"code\":%d,\"reason\":\"%s\"}",
                skip_update->skip_code, esc);
    }
    fputs("}\n", w->f);
    if (ferror(w->f))
        w->err = 1;
    if (w->owns_file)
        rc = fclose(w->f);
    else
        rc = fflush(w->f);
    w->f = NULL;
    if (rc != 0)
        w->err = 1;
    return w->err ? -1 : 0;
}
