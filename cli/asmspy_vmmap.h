/* asmspy_vmmap.h — /proc/<pid>/maps -> ranked, capped spans -> a JSON body.
 *
 * Header-only and PURE (it takes a FILE*, not a pid), which is the whole point:
 * the part that can be wrong is the parse and the cap discipline, and both are
 * then testable from a string fixture with no /proc, no ptrace and no victim.
 * Same extraction rationale as asmspy_graphsort.h and asmspy_autoregion.h.
 *
 * NOT reusable from scan_modules (asmspy_proc.c). That walks the same file and
 * then drops exactly the rows this needs:
 *     if (path[0] != '/')   continue;   / * skip [heap],[stack],[vdso],anon * /
 *     if (off != 0)         continue;   / * only the offset-0 mapping        * /
 * plus a dedup by path. It resolves module BASES for the symbol resolver; this
 * needs the TABLE, and the rows it discards are most of what a data trace
 * actually touches.
 */
#ifndef ASMSPY_VMMAP_H
#define ASMSPY_VMMAP_H

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtrace_ndjson.h" /* asmtrace_escape */
#include "asmtrace_sha256.h" /* the change gate's digest */

/* Rank-then-cap. Measured 97-118 JSON bytes per span, so 256 spans is ~30 KB of
 * body; a browser-class 5618-row map would be ~550 KB, against golden
 * recordings of 1.3-5.5 KB. The cap is mandatory, not tidiness. */
#define ASMSPY_VMMAP_CAP 256

typedef struct {
    uint64_t base, len;
    char perms[8];
    char name[256]; /* basename, or "[heap]"/"[stack]", or "" for anonymous */
    char path[256]; /* full pathname, or "" */
} asmspy_vmspan_t;

/* Rank: executable first, then descending length, then ascending base for a
 * total order (so the output is deterministic run to run, which the schema's
 * determinism rules want).
 *
 * Applied over the FULL table BEFORE the cap. Capping first is what "dropped
 * libc itself while keeping dozens of zero-symbol rows" in the procinfo modules
 * case, and the same failure here would drop the code the reader came to look at
 * in favour of a 1 GiB anonymous reservation. */
static int asmspy_vmmap_rank(const void *a, const void *b) {
    const asmspy_vmspan_t *x = (const asmspy_vmspan_t *)a;
    const asmspy_vmspan_t *y = (const asmspy_vmspan_t *)b;
    int xe = x->perms[2] == 'x', ye = y->perms[2] == 'x';
    if (xe != ye)
        return ye - xe;
    if (x->len != y->len)
        return x->len < y->len ? 1 : -1;
    return x->base < y->base ? -1 : (x->base > y->base);
}

/* Parse every row of an open /proc/<pid>/maps. Returns rows KEPT (<= the cap),
 * or -1 on allocation failure. *n_total is rows SEEN, so the truncation
 * magnitude stays recoverable — the stated v1 gap in procinfo's `modules`, fixed
 * here rather than copied.
 *
 * The caller owns *out and frees it. An unparseable line is skipped and not
 * counted: a maps file with one odd row is not a reason to report nothing. */
static int asmspy_vmmap_parse(FILE *f, asmspy_vmspan_t **out, size_t *n_total) {
    size_t cap = 128, n = 0;
    asmspy_vmspan_t *sp;
    char line[4096];

    *out = NULL;
    *n_total = 0;
    if (!f)
        return -1;
    sp = (asmspy_vmspan_t *)malloc(cap * sizeof *sp);
    if (!sp)
        return -1;

    while (fgets(line, sizeof line, f)) {
        uint64_t lo = 0, hi = 0, off = 0;
        /* A line longer than the buffer would come back in pieces, and the
         * TAIL of a long pathname can itself parse as "%llx-%llx %s ..." —
         * inventing a span from the middle of a filename. Drain and drop the
         * whole line instead: a maps row we cannot read entirely is one we do
         * not report. (4096 is far above the kernel's own line length; this is
         * the defensive path, not the common one.) */
        if (strchr(line, '\n') == NULL && !feof(f)) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n')
                ;
            continue;
        }
        char perms[8];
        int pathpos = 0;
        const char *p;
        asmspy_vmspan_t v;
        char buf[256];
        size_t k;

        /* Same sscanf shape scan_modules uses — and then NONE of its filters. */
        if (sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s %" SCNx64 " %*x:%*x %*u %n",
                   &lo, &hi, perms, &off, &pathpos) < 4)
            continue;
        if (hi <= lo)
            continue;
        (*n_total)++;

        memset(&v, 0, sizeof v);
        v.base = lo;
        v.len = hi - lo;
        snprintf(v.perms, sizeof v.perms, "%s", perms);

        p = pathpos > 0 ? line + pathpos : "";
        while (*p == ' ')
            p++;
        k = strcspn(p, "\n");
        if (k >= sizeof buf)
            k = sizeof buf - 1;
        memcpy(buf, p, k);
        buf[k] = '\0';
        if (buf[0] == '/') {
            const char *slash = strrchr(buf, '/');
            snprintf(v.path, sizeof v.path, "%s", buf);
            snprintf(v.name, sizeof v.name, "%s", slash ? slash + 1 : buf);
        } else if (buf[0] == '[') {
            snprintf(v.name, sizeof v.name, "%s", buf);
        } /* else: anonymous — name and path stay "" */

        if (n == cap) {
            size_t nc = cap * 2;
            asmspy_vmspan_t *nv =
                (asmspy_vmspan_t *)realloc(sp, nc * sizeof *sp);
            if (!nv)
                break; /* keep what we have; the total still counts the rest */
            sp = nv;
            cap = nc;
        }
        sp[n++] = v;
    }

    qsort(sp, n, sizeof *sp, asmspy_vmmap_rank);
    if (n > (size_t)ASMSPY_VMMAP_CAP)
        n = (size_t)ASMSPY_VMMAP_CAP;
    *out = sp;
    return (int)n;
}

/* Hash what the address space IS, for the caller's change gate.
 *
 * Deliberately NOT a hash of the emitted body: the body carries `version`, an
 * ordinal that increments on every emit, so digesting it would make every
 * snapshot differ from the last and the gate would never suppress anything —
 * measured, on a target whose 23 mappings had not moved at all.
 *
 * This is not the "lossy canonical form" the design forbids either. That warning
 * is against COALESCING rows before digesting (which drops evidence: a
 * name-coalesced digest was measured reporting "unchanged" while the raw rows
 * moved +2/-6). Every span's full tuple goes in here, in emission order, plus
 * the two fields that qualify the set. The only thing left out is the serial
 * number, which is not a fact about the process. */
static void asmspy_vmmap_digest(const asmspy_vmspan_t *sp, size_t n,
                                size_t total, int readable, char out[65]) {
    asmtrace_sha256_t h;
    size_t i;
    unsigned char hdr[17];
    asmtrace_sha256_init(&h);
    for (i = 0; i < 8; i++) {
        hdr[i] = (unsigned char)((total >> (i * 8)) & 0xff);
        hdr[8 + i] = 0;
    }
    hdr[16] = (unsigned char)(readable ? 1 : 0);
    asmtrace_sha256_update(&h, hdr, sizeof hdr);
    for (i = 0; i < n; i++) {
        unsigned char row[16];
        size_t k;
        for (k = 0; k < 8; k++) {
            row[k] = (unsigned char)((sp[i].base >> (k * 8)) & 0xff);
            row[8 + k] = (unsigned char)((sp[i].len >> (k * 8)) & 0xff);
        }
        asmtrace_sha256_update(&h, row, sizeof row);
        /* Include the NUL so "ab"+"c" and "a"+"bc" cannot collide. */
        asmtrace_sha256_update(&h, sp[i].perms, strlen(sp[i].perms) + 1);
        asmtrace_sha256_update(&h, sp[i].name, strlen(sp[i].name) + 1);
        asmtrace_sha256_update(&h, sp[i].path, strlen(sp[i].path) + 1);
    }
    {
        uint8_t d[32];
        int j;
        asmtrace_sha256_final(&h, d);
        for (j = 0; j < 32; j++)
            snprintf(out + j * 2, 3, "%02x", d[j]);
        out[64] = '\0';
    }
}

/* Build the event BODY (no leading comma — asmtrace_emit prepends
 * {"k":"vmmap",). Returns 0, or -1 when it will not fit.
 *
 * The caller must REFUSE loudly on -1, never emit a partial body: a truncated
 * token is syntactically invalid JSON that a reader reports as a corrupt
 * recording rather than as our bug. This is why the emitter cannot use
 * rec_emitf, whose 16 KB stack buffer discards vsnprintf's return and would do
 * exactly that silently.
 *
 * Addresses are hex STRINGS (a JSON number is a double in many readers and
 * silently rounds a 64-bit pointer); lengths and counts stay numbers. An
 * anonymous row OMITS `name`/`path` rather than carrying empty ones. */
static int asmspy_vmmap_body(const asmspy_vmspan_t *sp, size_t n, size_t total,
                             int readable, unsigned version, char *buf,
                             size_t cap) {
    size_t o = 0, i;
    int w;

#define ASMSPY_VM_APP(...)                                                     \
    do {                                                                       \
        w = snprintf(buf + o, cap - o, __VA_ARGS__);                           \
        if (w < 0 || (size_t)w >= cap - o)                                     \
            return -1;                                                         \
        o += (size_t)w;                                                        \
    } while (0)

    if (!buf || cap == 0)
        return -1;
    ASMSPY_VM_APP("\"version\":%u,\"maps_readable\":%s,\"spans_total\":%llu,"
                  "\"spans_truncated\":%s,\"spans\":[",
                  version, readable ? "true" : "false",
                  (unsigned long long)total, total > n ? "true" : "false");
    for (i = 0; i < n; i++) {
        char en[256 * 6 + 16], ep[256 * 6 + 16], epr[8 * 6 + 16];
        asmtrace_escape(en, sizeof en, sp[i].name);
        asmtrace_escape(ep, sizeof ep, sp[i].path);
        /* `perms` is escaped too. It is normally four fixed characters from
         * the kernel, but a maps line longer than the read buffer splits under
         * fgets and its TAIL is then parsed as a fresh line — so this field can
         * carry arbitrary path bytes, including a quote or a backslash, and an
         * unescaped %s would emit malformed JSON. Escaping every string field
         * without exception is cheaper than reasoning about which one can be
         * trusted. */
        asmtrace_escape(epr, sizeof epr, sp[i].perms);
        ASMSPY_VM_APP("%s{\"base\":\"0x%llx\",\"len\":%llu,\"perms\":\"%s\"",
                      i ? "," : "", (unsigned long long)sp[i].base,
                      (unsigned long long)sp[i].len, epr);
        if (en[0])
            ASMSPY_VM_APP(",\"name\":\"%s\"", en);
        if (ep[0])
            ASMSPY_VM_APP(",\"path\":\"%s\"", ep);
        ASMSPY_VM_APP("}");
    }
    ASMSPY_VM_APP("]");
#undef ASMSPY_VM_APP
    return 0;
}

#endif /* ASMSPY_VMMAP_H */
