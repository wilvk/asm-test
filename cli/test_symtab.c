/* test_symtab.c — headless unit test for the symbol REVERSE lookup
 * (asmspy_symtab_at, cli/asmspy_proc.c).
 *
 * Every view that names an address goes through this one function: the stream's
 * "func+0x1c", the graph's nodes, the tree's entries, --watch's faulting PC,
 * --sample's endpoints. It is a binary search for the greatest addr <= query
 * followed by a containment test, and its edges are exactly the places a tracer
 * lies quietly rather than fails:
 *
 *   - one byte past a function's end must be NOTHING, not that function;
 *   - an address in the GAP between two functions (padding, a linker stub) must
 *     be NOTHING, not the one before it;
 *   - a zero-SIZE symbol (an asm label, a stripped import) covers exactly its
 *     own address and cannot be allowed to swallow everything after it;
 *   - an address below the first symbol must be NOTHING, not the first symbol.
 *
 * Each of those returns a plausible NAME if it is wrong, which is worse than
 * returning nothing — the same "confidently wrong" reasoning that governs the
 * separate-debug verification and the 32-bit refusal.
 *
 * Built + run by `make cli-smoke` before the ptrace smoke (no ncurses, no
 * ptrace). Links asmspy_proc.o directly, like test_jitdump.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asmspy.h"

static int failures;

/* Names/modules are char* (owned) in the real loader; static buffers here keep
 * the table hand-buildable without asmspy_symtab_free ever seeing it. */
static char n_alpha[] = "alpha";
static char n_beta[] = "beta";
static char n_gamma[] = "gamma";
static char n_label[] = "zero_size_label";
static char m_mod[] = "mod";

/* alpha [0x1000,0x1010)  beta [0x1010,0x1020)   <- adjacent, no gap
 * (gap 0x1020..0x1100)                          <- padding: no symbol
 * gamma [0x1100,0x1108)
 * label @0x2000 size 0                          <- covers exactly one address
 * Sorted by address, which is what asmspy_symtab_load guarantees. */
static asmspy_sym_t V[] = {
    {0x1000, 0x10, n_alpha, m_mod},
    {0x1010, 0x10, n_beta, m_mod},
    {0x1100, 0x08, n_gamma, m_mod},
    {0x2000, 0x00, n_label, m_mod},
};
static asmspy_symtab_t T = {V, sizeof V / sizeof V[0]};

static void want(uint64_t addr, const char *expect) {
    const asmspy_sym_t *s = asmspy_symtab_at(&T, addr);
    const char *got = s ? s->name : "(none)";
    const char *exp = expect ? expect : "(none)";
    if (strcmp(got, exp) != 0) {
        fprintf(stderr, "FAIL at 0x%llx: got '%s', want '%s'\n",
                (unsigned long long)addr, got, exp);
        failures++;
        return;
    }
    printf("ok   0x%-6llx -> %s\n", (unsigned long long)addr, got);
}

int main(void) {
    /* --- below everything: must not latch onto the first symbol ---------- */
    want(0x0, NULL);
    want(0xfff, NULL);

    /* --- inside a sized symbol: first byte, middle, LAST byte ------------ */
    want(0x1000, "alpha"); /* entry                     */
    want(0x1008, "alpha"); /* middle (the "+0x8" case)  */
    want(0x100f, "alpha"); /* last byte still inside    */

    /* --- the boundary: one past alpha's end IS beta, not alpha ----------- */
    want(0x1010, "beta");
    want(0x101f, "beta");

    /* --- THE GAP: one past beta's end is nothing at all ------------------ */
    /* This is the assertion that matters most. An off-by-one in the
     * containment test (< vs <=) hands back "beta" for an address beta does
     * not contain, and every view then labels padding as a real function. */
    want(0x1020, NULL);
    want(0x1080, NULL); /* deep in the gap */
    want(0x10ff, NULL); /* one byte before gamma */

    want(0x1100, "gamma");
    want(0x1107, "gamma");
    want(0x1108, NULL); /* one past gamma */

    /* --- ZERO-SIZE symbol: covers its own address and nothing else ------- */
    /* A size-0 symbol is common (asm labels, some stripped imports). Treated
     * as "covers everything until the next symbol" it would swallow the whole
     * rest of the address space — here, the last entry, so literally all of it. */
    want(0x2000, "zero_size_label");
    want(0x2001, NULL);
    want(0xffffffffffffffffULL, NULL);

    /* --- degenerate tables ---------------------------------------------- */
    {
        asmspy_symtab_t empty = {NULL, 0};
        if (asmspy_symtab_at(&empty, 0x1000) != NULL) {
            fprintf(stderr, "FAIL empty table returned a symbol\n");
            failures++;
        } else {
            printf("ok   empty table -> (none)\n");
        }
    }
    { /* n == 1 exercises the binary search's lo/hi collapse */
        asmspy_sym_t one[] = {{0x500, 0x10, n_alpha, m_mod}};
        asmspy_symtab_t t1 = {one, 1};
        const asmspy_sym_t *a = asmspy_symtab_at(&t1, 0x4ff);
        const asmspy_sym_t *b = asmspy_symtab_at(&t1, 0x500);
        const asmspy_sym_t *c = asmspy_symtab_at(&t1, 0x50f);
        const asmspy_sym_t *d = asmspy_symtab_at(&t1, 0x510);
        if (a || !b || !c || d) {
            fprintf(stderr,
                    "FAIL single-entry table: below=%s at=%s last=%s "
                    "past=%s\n",
                    a ? a->name : "-", b ? b->name : "-", c ? c->name : "-",
                    d ? d->name : "-");
            failures++;
        } else {
            printf("ok   single-entry table: below/past miss, inside hits\n");
        }
    }

    /* --- forward lookup, the same table --------------------------------- */
    {
        const asmspy_sym_t *s = asmspy_symtab_by_name(&T, "gamma");
        if (!s || s->addr != 0x1100) {
            fprintf(stderr, "FAIL by_name(gamma) -> %s\n",
                    s ? "wrong addr" : "NULL");
            failures++;
        } else {
            printf("ok   by_name(gamma) -> 0x1100\n");
        }
        /* exact match only: a prefix must not match */
        if (asmspy_symtab_by_name(&T, "gam") != NULL) {
            fprintf(stderr,
                    "FAIL by_name is matching a PREFIX, not the name\n");
            failures++;
        } else {
            printf("ok   by_name('gam') -> (none) (exact match, not prefix)\n");
        }
        if (asmspy_symtab_by_name(&T, "nosuch") != NULL) {
            fprintf(stderr, "FAIL by_name(nosuch) returned a symbol\n");
            failures++;
        }
    }

    if (failures) {
        fprintf(stderr, "test_symtab: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_symtab: all checks passed\n");
    return 0;
}
