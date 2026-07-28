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

void asmtrace_writer_set_steps_total(asmtrace_writer_t *w,
                                     unsigned long long total) {
    if (!w)
        return;
    w->steps_total = total;
    w->have_steps_total = 1;
}

void asmtrace_writer_set_code(asmtrace_writer_t *w, const char *name,
                              const char *sha256_hex, unsigned long long len) {
    if (!w)
        return;
    if (!sha256_hex) {
        w->have_code = 0;
        return;
    }
    snprintf(w->code_name, sizeof w->code_name, "%s", name ? name : "");
    snprintf(w->code_sha, sizeof w->code_sha, "%s", sha256_hex);
    w->code_len = len;
    w->have_code = 1;
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
    /* `code` routine identity, when armed — an optional object right after the
     * required identity cluster (producer/provenance/arch), emitted in both
     * deterministic and live mode because a routine's bytes are not volatile. */
    if (w->have_code) {
        asmtrace_escape(esc, sizeof esc, w->code_name);
        fprintf(w->f,
                ",\"code\":{\"name\":\"%s\",\"sha256\":\"%s\",\"len\":%llu}",
                esc, w->code_sha, w->code_len);
    }
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
 * value_valid, [wide], [bytes], [value]. Memory terms are omitted for a register
 * and `value` is omitted when it was not captured (or spilled wide) — "not
 * measured" must not render as a zero. A >8-byte operand carries `"wide":true`
 * and, when the side buffer holds its bytes, a `bytes` hex string (28 R1 T3);
 * `wide`/`wide_len` are that side buffer (NULL when the producer has none). */
static size_t op_body(char *b, size_t cap, size_t o, const at_val_rec_t *r,
                      const uint8_t *wide, size_t wide_len) {
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
    if (r->wide) {
        o = bp(b, cap, o, ",\"wide\":true");
        /* The bytes, when the side buffer carries them: a fixed lowercase-hex
         * string of the operand's `size` bytes at wide_off. Bounds-checked, so a
         * wide flag with no (or a short) buffer degrades to bytes-less [wide]. */
        if (r->value_valid && wide != NULL && r->size > 0 &&
            (size_t)r->wide_off + r->size <= wide_len) {
            o = bp(b, cap, o, ",\"bytes\":\"");
            for (uint16_t k = 0; k < r->size; k++)
                o = bp(b, cap, o, "%02x", wide[r->wide_off + k]);
            o = bp(b, cap, o, "\"");
        }
    } else if (r->value_valid) {
        o = bp(b, cap, o, ",\"value\":%llu", (unsigned long long)r->value);
    }
    return bp(b, cap, o, "}");
}

size_t asmtrace_df_step_body(char *dst, size_t cap, unsigned step, uint64_t off,
                             const char *disasm, const at_val_rec_t *recs,
                             size_t n, const uint8_t *wide, size_t wide_len) {
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
        o = op_body(dst, cap, o, &recs[i], wide, wide_len);
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
    /* An edge's loc is a location identity, not a captured value: no side buffer,
     * so a wide loc degrades to bytes-less [wide]. */
    return op_body(dst, cap, o, &e->loc, NULL, 0);
}

size_t asmtrace_mem_body(char *dst, size_t cap, unsigned step, uint64_t ea,
                         unsigned size, int is_write, at_loc_kind_t space) {
    size_t o = 0;
    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    /* `space` reuses the operand loc token so a reader normalizes a `mem` EA the
     * same way it does a df_step memory operand's `addr` — abs vs routine-off. A
     * register kind is not a memory access; loc_space folds an accidental one to
     * "reg", but the caller filters memory records before reaching here. */
    return bp(
        dst, cap, o,
        "\"step\":%u,\"ea\":%llu,\"size\":%u,\"rw\":\"%s\",\"space\":\"%s\"",
        step, (unsigned long long)ea, size, is_write ? "w" : "r",
        loc_space(space));
}

size_t asmtrace_regstate_body(char *dst, size_t cap,
                              const asmtest_regfile_t *r) {
    size_t o = 0;
    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    /* Same {"desc","values"} shape and field order as the emulator's
     * emit_regstate (tools/asmtrace_record.c) so ONE Scrubber deck renders both
     * producers; only the descriptor id differs — user_regs@x86_64/sysv names the
     * ptrace source (struct user_regs_struct), more authoritative than emulation
     * yet captured perturbingly (single-step). The consumer keys on field NAMES,
     * not the id (desktop/src/analysis/stepindex.cpp). */
    return bp(dst, cap, o,
              "\"desc\":\"user_regs@x86_64/sysv\",\"values\":{"
              "\"rax\":%llu,\"rbx\":%llu,\"rcx\":%llu,\"rdx\":%llu,"
              "\"rsi\":%llu,\"rdi\":%llu,\"rbp\":%llu,\"rsp\":%llu,"
              "\"r8\":%llu,\"r9\":%llu,\"r10\":%llu,\"r11\":%llu,"
              "\"r12\":%llu,\"r13\":%llu,\"r14\":%llu,\"r15\":%llu,"
              "\"rip\":%llu,\"rflags\":%llu}",
              (unsigned long long)r->rax, (unsigned long long)r->rbx,
              (unsigned long long)r->rcx, (unsigned long long)r->rdx,
              (unsigned long long)r->rsi, (unsigned long long)r->rdi,
              (unsigned long long)r->rbp, (unsigned long long)r->rsp,
              (unsigned long long)r->r8, (unsigned long long)r->r9,
              (unsigned long long)r->r10, (unsigned long long)r->r11,
              (unsigned long long)r->r12, (unsigned long long)r->r13,
              (unsigned long long)r->r14, (unsigned long long)r->r15,
              (unsigned long long)r->rip, (unsigned long long)r->rflags);
}

size_t asmtrace_blame_body(char *dst, size_t cap, unsigned step, uint64_t off,
                           const at_val_rec_t *loc, const uint32_t *cone_steps,
                           const uint64_t *cone_offs, size_t ncone,
                           int born_untraced) {
    size_t o = 0;
    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    o = bp(dst, cap, o, "\"step\":%u,\"off\":%llu", step,
           (unsigned long long)off);
    /* The blamed value's identity, when supplied — the sink step's write record,
     * spelled through the SAME op_body df_step/df_edge use, so a blame `loc` reads
     * identically to a df_edge one. A loc is a location identity, not a captured
     * value, so no wide side buffer (like df_edge). */
    if (loc != NULL) {
        o = bp(dst, cap, o, ",\"loc\":");
        o = op_body(dst, cap, o, loc, NULL, 0);
    }
    /* The attributed cone: the ascending backward def-use slice, each producing
     * step with its instruction offset and an `insn` kind tag (mirroring the
     * `srcmap` {off,kind} row). The sink step S is the first entry — a value with
     * no traced producer carries the sink ALONE (born_untraced below), never an
     * empty cone. */
    o = bp(dst, cap, o, ",\"cone\":[");
    for (size_t i = 0; i < ncone; i++) {
        if (i)
            o = bp(dst, cap, o, ",");
        o = bp(dst, cap, o, "{\"step\":%u,\"off\":%llu,\"kind\":\"insn\"}",
               cone_steps[i], (unsigned long long)cone_offs[i]);
    }
    o = bp(dst, cap, o, "]");
    /* The honesty verdict: 1 when the value is born of untraced state (its
     * ancestry ends at instrumentation — no producer inside the window). The
     * consumer renders the lineage wording; the wire carries the machine-readable
     * fact so the cone-of-one is never mistaken for "nothing happened". */
    return bp(dst, cap, o, ",\"born_untraced\":%s",
              born_untraced ? "true" : "false");
}

/* The register file's 18 fields, in descriptor order (rax..r15, rip, rflags) —
 * the SAME order both regstate emitters use — as name + struct member so
 * statediff can diff and emit a NAMED subset. */
#define REGFILE_FIELD(f)                                                       \
    { #f, offsetof(asmtest_regfile_t, f) }
static const struct {
    const char *name;
    size_t off;
} kRegfileFields[] = {
    REGFILE_FIELD(rax), REGFILE_FIELD(rbx), REGFILE_FIELD(rcx),
    REGFILE_FIELD(rdx), REGFILE_FIELD(rsi), REGFILE_FIELD(rdi),
    REGFILE_FIELD(rbp), REGFILE_FIELD(rsp), REGFILE_FIELD(r8),
    REGFILE_FIELD(r9),  REGFILE_FIELD(r10), REGFILE_FIELD(r11),
    REGFILE_FIELD(r12), REGFILE_FIELD(r13), REGFILE_FIELD(r14),
    REGFILE_FIELD(r15), REGFILE_FIELD(rip), REGFILE_FIELD(rflags),
};
#undef REGFILE_FIELD

static uint64_t regfield(const asmtest_regfile_t *r, size_t off) {
    uint64_t v;
    memcpy(&v, (const char *)r + off, sizeof v);
    return v;
}

size_t asmtrace_statediff_body(char *dst, size_t cap, unsigned step,
                               const asmtest_regfile_t *prev,
                               const asmtest_regfile_t *cur) {
    size_t o = 0;
    int first = 1;
    if (!dst || !cap)
        return 0;
    dst[0] = '\0';
    o = bp(dst, cap, o, "\"step\":%u,\"changed\":{", step);
    /* No known predecessor (the first held step, or across an evicted boundary):
     * an empty delta with computed=false. The consumer must NOT read "nothing
     * changed" as truth here — the predecessor's state is unknown, so a full
     * delta would be a D7 lie. */
    if (prev != NULL) {
        for (size_t i = 0; i < sizeof kRegfileFields / sizeof kRegfileFields[0];
             i++) {
            uint64_t pv = regfield(prev, kRegfileFields[i].off);
            uint64_t cv = regfield(cur, kRegfileFields[i].off);
            if (pv == cv)
                continue;
            o = bp(dst, cap, o, "%s\"%s\":%llu", first ? "" : ",",
                   kRegfileFields[i].name, (unsigned long long)cv);
            first = 0;
        }
    }
    return bp(dst, cap, o, "},\"computed\":%s", prev ? "true" : "false");
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
    /* The total step count (28 R1 T2), when the producer supplied it: additive,
     * so a reader that predates it ignores it. It rides after drops, in the
     * honesty cluster, because it is what turns "truncated" into "N of M". */
    if (w->have_steps_total)
        fprintf(w->f, ",\"steps_total\":%llu", w->steps_total);
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
