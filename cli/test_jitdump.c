/* test_jitdump.c — unit test for the binary jitdump reader + the two-tier JIT
 * resolve chain (asmspy_proc.c).
 *
 * Generates a synthetic-but-valid perf jitdump file (header + three
 * JIT_CODE_LOAD records + one JIT_CODE_MOVE + records of types the reader must
 * skip) plus an overlapping text perf-map, both keyed to OUR OWN pid so the
 * resolver's per-pid discovery finds them, then asserts:
 *
 *   - each CODE_LOAD resolves to its name with its EXACT size (extent-checked
 *     at first/last byte and one past the end);
 *   - CODE_MOVE relocates a method (old address dark, new address resolves);
 *   - unknown record types and the LOAD records' code bytes are skipped;
 *   - where jitdump and the text perf-map both name an address, jitdump WINS;
 *   - a text-map-only method still resolves (the tiers merge, not replace);
 *   - asmspy_resolve's rate-limited refresh-on-miss picks all of it up with no
 *     manual refresh (the engines' integration path);
 *   - a corrupt / foreign-endian dump is rejected without breaking the tier.
 *
 * No ptrace, no ncurses — runs anywhere `make cli-smoke` does.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "asmspy.h"

static int failures;

#define CHECK(cond, what)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* ---- tiny jitdump writer (the fixture generator) -------------------- */

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

static void wr_header(FILE *f, uint32_t magic) {
    put32(f, magic); /* 'JiTD' (or a deliberately wrong value)   */
    put32(f, 1);     /* version                                  */
    put32(f, 40);    /* total_size of this header                */
    put32(f, 62);    /* elf_mach = EM_X86_64                     */
    put32(f, 0);     /* pad1                                     */
    put32(f, (uint32_t)getpid());
    put64(f, 0x123456789abcull); /* timestamp */
    put64(f, 0);                 /* flags     */
}

/* JIT_CODE_LOAD (id 0): fixed part + name\0 + code_size code bytes. */
static void wr_load(FILE *f, uint64_t addr, uint64_t size, uint64_t index,
                    const char *name) {
    uint32_t total = (uint32_t)(16 + 40 + strlen(name) + 1 + size);
    put32(f, 0); /* id = JIT_CODE_LOAD */
    put32(f, total);
    put64(f, 111); /* timestamp */
    put32(f, (uint32_t)getpid());
    put32(f, 1);    /* tid */
    put64(f, addr); /* vma */
    put64(f, addr); /* code_addr (== vma by default, per the spec) */
    put64(f, size);
    put64(f, index);
    fwrite(name, strlen(name) + 1, 1, f);
    for (uint64_t i = 0; i < size; i++) /* the code bytes the reader skips */
        fputc(0x90, f);
}

/* JIT_CODE_MOVE (id 1). */
static void wr_move(FILE *f, uint64_t oldaddr, uint64_t newaddr, uint64_t size,
                    uint64_t index) {
    put32(f, 1); /* id = JIT_CODE_MOVE */
    put32(f, 16 + 48);
    put64(f, 222); /* timestamp */
    put32(f, (uint32_t)getpid());
    put32(f, 1);       /* tid */
    put64(f, newaddr); /* vma */
    put64(f, oldaddr);
    put64(f, newaddr);
    put64(f, size);
    put64(f, index);
}

/* A record of a type the reader does not decode (skipped via total_size). */
static void wr_other(FILE *f, uint32_t id, size_t payload) {
    put32(f, id);
    put32(f, (uint32_t)(16 + payload));
    put64(f, 333); /* timestamp */
    for (size_t i = 0; i < payload; i++)
        fputc(0xAB, f);
}

/* Resolve helper: name at `addr` via the full chain, or NULL. */
static const char *rname(asmspy_jitmap_t *j, uint64_t addr) {
    const asmspy_sym_t *s = asmspy_resolve(NULL, j, addr);
    return s ? s->name : NULL;
}

static int name_is(asmspy_jitmap_t *j, uint64_t addr, const char *want) {
    const char *got = rname(j, addr);
    return got && strcmp(got, want) == 0;
}

int main(void) {
    char dump[64], map[64];
    snprintf(dump, sizeof dump, "/tmp/jit-%d.dump", (int)getpid());
    snprintf(map, sizeof map, "/tmp/perf-%d.map", (int)getpid());
    unlink(dump); /* a stale run's leftovers would corrupt the assertions */
    unlink(map);

    asmspy_jitmap_t j;
    asmspy_jitmap_init(&j, getpid());

    /* no JIT source at all: refresh reports -1, nothing resolves */
    CHECK(asmspy_jitmap_refresh(&j) == -1, "refresh with no files -> -1");
    CHECK(rname(&j, 0x100008) == NULL, "empty map resolves nothing");

    /* a corrupt dump (foreign-endian magic) is rejected, not misparsed */
    FILE *f = fopen(dump, "wb");
    if (!f) {
        fprintf(stderr, "cannot create %s\n", dump);
        return 1;
    }
    wr_header(f, 0x4454694A); /* byteswapped 'JiTD' — a big-endian writer */
    wr_load(f, 0xdead0000, 0x10, 9, "bogus_endian");
    fclose(f);
    CHECK(asmspy_jitmap_refresh(&j) == -1, "foreign-endian dump -> no source");
    CHECK(rname(&j, 0xdead0008) == NULL, "foreign-endian dump not misparsed");

    /* ---- the real fixture ------------------------------------------ */
    f = fopen(dump, "wb");
    if (!f) {
        fprintf(stderr, "cannot recreate %s\n", dump);
        return 1;
    }
    wr_header(f, 0x4A695444);
    wr_load(f, 0x100000, 0x10, 1, "alpha");  /* plain method             */
    wr_other(f, 2, 24);                      /* DEBUG_INFO-ish: skip it  */
    wr_load(f, 0x200000, 0x18, 2, "beta");   /* will be MOVEd            */
    wr_load(f, 0x300000, 0x20, 3, "gamma");  /* text map overlaps this   */
    wr_move(f, 0x200000, 0x400000, 0x18, 2); /* tiered/GC code motion    */
    wr_other(f, 3, 0);                       /* CLOSE: empty payload     */
    fclose(f);

    f = fopen(map, "w");
    if (!f) {
        fprintf(stderr, "cannot create %s\n", map);
        return 1;
    }
    /* starts inside gamma's jitdump extent -> jitdump must win */
    fprintf(f, "%x %x %s\n", 0x300008, 0x8, "stale_text_gamma");
    /* text-only method -> must still resolve (tiers merge) */
    fprintf(f, "%x %x %s\n", 0x500000, 0x30, "textonly");
    fclose(f);

    /* THE INTEGRATION PATH: no manual refresh — asmspy_resolve's rate-limited
     * refresh-on-miss must discover + load both files by itself, within one
     * cooldown window (the map was left empty + rearmed by the failed refresh
     * above, exactly a non-JIT-looking engine attach). */
    const asmspy_sym_t *s = NULL;
    int tries = 0;
    for (; tries < 200 && !s; tries++)
        s = asmspy_resolve(NULL, &j, 0x100008);
    CHECK(s != NULL, "refresh-on-miss discovered the new files");
    CHECK(s && strcmp(s->name, "alpha") == 0, "refresh-on-miss found alpha");
    fprintf(stderr, "  refresh-on-miss resolved after %d calls\n", tries);

    /* sizes are exact: first byte, last byte, one past the end */
    CHECK(name_is(&j, 0x100000, "alpha"), "alpha first byte");
    CHECK(name_is(&j, 0x10000f, "alpha"), "alpha last byte (size 0x10)");
    CHECK(rname(&j, 0x100010) == NULL, "alpha extent ends at +size");
    const asmspy_sym_t *a = asmspy_resolve(NULL, &j, 0x100000);
    CHECK(a && a->size == 0x10, "alpha size carried from CODE_LOAD");
    CHECK(a && strcmp(a->module, "jit") == 0, "jitdump methods tagged [jit]");

    /* CODE_MOVE: beta lives at its NEW address only */
    CHECK(rname(&j, 0x200008) == NULL, "beta's old address is dark after MOVE");
    CHECK(name_is(&j, 0x400008, "beta"), "beta resolves at its MOVEd address");
    const asmspy_sym_t *b = asmspy_resolve(NULL, &j, 0x400000);
    CHECK(b && b->size == 0x18, "beta size preserved across MOVE");

    /* jitdump wins where both sources name an address */
    CHECK(name_is(&j, 0x300008, "gamma"),
          "jitdump beats the text map on overlap");
    CHECK(name_is(&j, 0x30001f, "gamma"), "gamma's jitdump extent (0x20) used");

    /* the text tier still contributes what jitdump doesn't cover */
    CHECK(name_is(&j, 0x500010, "textonly"), "text-only method resolves");

    /* a genuine miss stays a miss */
    CHECK(rname(&j, 0x900000) == NULL, "unmapped address stays unresolved");

    /* an explicit refresh re-reads both tiers consistently: 4 methods total
     * (alpha, beta, gamma, textonly — stale_text_gamma shadowed by jitdump) */
    CHECK(asmspy_jitmap_refresh(&j) == 4, "refresh counts merged methods");
    CHECK(name_is(&j, 0x400008, "beta"), "beta still resolves after refresh");

    asmspy_jitmap_free(&j);
    unlink(dump);
    unlink(map);

    if (failures) {
        fprintf(stderr, "test_jitdump: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_jitdump: PASS\n");
    return 0;
}
