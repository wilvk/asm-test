/*
 * test_dataflow_gcmove.c — Phase 4 (increment 2): GC-move address
 * canonicalization, validated over SYNTHETIC value traces + synthetic move
 * ranges built by hand. PURE — no Capstone, no Unicorn, no runtime, no .NET SDK
 * — so it runs and passes on EVERY CI host, exactly like test_dataflow's pure
 * spine. It probes the plan's Phase 4 exit criterion for the compaction half:
 * "a managed value's def-use survives an induced GC compaction without aliasing
 * pre/post-move addresses".
 *
 *   canon:   asmtest_gcmove_canon forwards a pre-move address to the object's
 *            final resting place, leaves a post-move address in place, preserves
 *            the field offset ((object,field) identity), composes across two GCs,
 *            and leaves an unrelated address untouched.
 *   survive: a store-before-move / load-after-move pair (different raw addresses)
 *            is DISCONNECTED in the raw def-use graph and RECONNECTED after
 *            canonicalization — while an unrelated object reusing the vacated old
 *            slot is FALSELY linked raw and correctly SEPARATED after (no alias).
 *   canonicalize: rewrites only AT_LOC_MEM_ABS records, reports the change count,
 *            sorts an out-of-order move set, and guards the NULL trace.
 */
#include "asmtest_valtrace.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* A compacting heap: object O lives at OLD, is relocated to NEW by a GC that
 * takes effect at step boundary 1, and an UNRELATED object later reuses the
 * vacated OLD slot. F is a field offset inside O; LEN is O's extent. */
#define OLD 0x00100000ULL
#define NEW 0x00200000ULL
#define LEN 0x30ULL
#define F   0x10ULL
#define G   0x18ULL

static at_val_rec_t mem_abs(uint64_t a, uint16_t sz, bool w) {
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_MEM_ABS;
    r.addr = a;
    r.size = sz;
    r.is_write = w;
    return r;
}

static int has_edge(const asmtest_defuse_t *g, uint32_t from, uint32_t to) {
    for (size_t i = 0; i < g->n; i++)
        if (g->edges[i].from_step == from && g->edges[i].to_step == to)
            return 1;
    return 0;
}

/* ---- canon(): the pure forward-to-final address map ---------------------- */
static void test_canon(void) {
    /* One GC: [OLD,OLD+LEN) -> [NEW,...) at boundary 1. */
    asmtest_gcmove_t mv[1] = {{OLD, NEW, LEN, 1}};

    /* A pre-move access (step 0 < 1) is forwarded to the final address; the
     * field offset survives, giving (object, field) identity. */
    CHECK(asmtest_gcmove_canon(mv, 1, 0, OLD + F) == NEW + F,
          "canon: pre-move O.field forwards OLD+F -> NEW+F");
    CHECK(asmtest_gcmove_canon(mv, 1, 0, OLD + G) == NEW + G,
          "canon: a second field forwards independently (offset preserved)");
    CHECK(asmtest_gcmove_canon(mv, 1, 0, OLD + F) !=
              asmtest_gcmove_canon(mv, 1, 0, OLD + G),
          "canon: distinct fields stay distinct after the move");

    /* A post-move access (step 1 >= 1) already sits at the final address. */
    CHECK(asmtest_gcmove_canon(mv, 1, 1, NEW + F) == NEW + F,
          "canon: post-move O.field stays NEW+F (boundary is inclusive)");
    CHECK(asmtest_gcmove_canon(mv, 1, 2, NEW + F) == NEW + F,
          "canon: a later post-move access stays NEW+F");

    /* The vacated OLD slot, touched AFTER the move, is NOT forwarded — an
     * unrelated object there keeps its own identity. */
    CHECK(asmtest_gcmove_canon(mv, 1, 2, OLD + F) == OLD + F,
          "canon: unrelated post-move access at the vacated OLD slot stays "
          "OLD+F");

    /* An address outside every move range is untouched. */
    CHECK(asmtest_gcmove_canon(mv, 1, 0, 0xDEAD0000ULL) == 0xDEAD0000ULL,
          "canon: address outside all ranges is unchanged");
    /* No moves at all: identity. */
    CHECK(asmtest_gcmove_canon(NULL, 0, 0, OLD + F) == OLD + F,
          "canon: empty move set is the identity map");

    /* Two GCs: O moves OLD->NEW at boundary 1, then NEW->FINAL at boundary 3.
     * A pre-both access composes through both hops to FINAL. */
    uint64_t FINAL = 0x00300000ULL;
    asmtest_gcmove_t mv2[2] = {{OLD, NEW, LEN, 1}, {NEW, FINAL, LEN, 3}};
    CHECK(asmtest_gcmove_canon(mv2, 2, 0, OLD + F) == FINAL + F,
          "canon: composes two compactions OLD -> NEW -> FINAL");
    CHECK(asmtest_gcmove_canon(mv2, 2, 2, NEW + F) == FINAL + F,
          "canon: an access between the two GCs forwards through the 2nd only");
    CHECK(asmtest_gcmove_canon(mv2, 2, 4, FINAL + F) == FINAL + F,
          "canon: an access after both GCs is already final");
}

/* ---- the exit criterion: def-use survives a compaction, no false alias --- *
 *
 * Trace (physical addresses, one memory op per step):
 *   step0:  WRITE M[OLD+F]   O.field def, BEFORE the move
 *   step1:  READ  M[NEW+F]   O.field use, AFTER the move  (GC at boundary 1)
 *   step2:  READ  M[OLD+F]   unrelated load of the now-vacated OLD slot
 *
 * Raw (uncanonicalized) the memory last-writer map is DOUBLY wrong: the true
 * O def-use (0 -> 1) is LOST (OLD+F vs NEW+F never match) and a FALSE alias
 * (0 -> 2) is forged (both touch raw OLD+F). Canonicalization forwards step0's
 * write to NEW+F, so 0 -> 1 reconnects and 0 -> 2 disappears.
 */
static asmtest_valtrace_t *build_move_trace(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(8, 16, 0);
    if (!v)
        return NULL;
    at_val_rec_t s0[] = {mem_abs(OLD + F, 8, true)};
    at_val_rec_t s1[] = {mem_abs(NEW + F, 8, false)};
    at_val_rec_t s2[] = {mem_abs(OLD + F, 8, false)};
    asmtest_valtrace_append(v, 0x00, s0, 1);
    asmtest_valtrace_append(v, 0x08, s1, 1);
    asmtest_valtrace_append(v, 0x10, s2, 1);
    return v;
}

static void test_survives_move(void) {
    /* (a) RAW: the def-use graph before canonicalization. */
    asmtest_valtrace_t *v = build_move_trace();
    if (!v) {
        CHECK(0, "survive: build_move_trace (raw)");
        return;
    }
    asmtest_defuse_t *raw = asmtest_defuse_build(v);
    CHECK(
        raw && !has_edge(raw, 0, 1),
        "survive/raw: O def-use 0->1 is LOST across the move (OLD+F vs NEW+F)");
    CHECK(raw && has_edge(raw, 0, 2),
          "survive/raw: FALSE alias 0->2 forged (unrelated OLD+F collision)");
    asmtest_defuse_free(raw);

    /* (b) canonicalize in place, then rebuild the def-use graph. */
    asmtest_gcmove_t mv[1] = {{OLD, NEW, LEN, 1}};
    size_t changed = asmtest_gcmove_canonicalize(v, mv, 1);
    CHECK(changed == 1, "survive: canonicalize remaps exactly the pre-move "
                        "write (OLD+F->NEW+F)");
    CHECK(v->recs[0].addr == NEW + F,
          "survive: step0 write forwarded to the object's final address");
    CHECK(v->recs[1].addr == NEW + F, "survive: step1 read left at NEW+F");
    CHECK(v->recs[2].addr == OLD + F,
          "survive: step2 (unrelated) left at OLD+F");

    asmtest_defuse_t *can = asmtest_defuse_build(v);
    CHECK(
        can && has_edge(can, 0, 1),
        "survive: def-use 0->1 SURVIVES the compaction after canonicalization");
    CHECK(can && !has_edge(can, 0, 2),
          "survive: the false alias 0->2 is GONE (no pre/post-move aliasing)");
    asmtest_defuse_free(can);
    asmtest_valtrace_free(v);
}

/* ---- canonicalize(): record selectivity, ordering, NULL guard ------------ */
static void test_canonicalize_api(void) {
    /* Only AT_LOC_MEM_ABS records are rewritten; a routine-offset mem record and
     * a register record are left untouched (a GC move is absolute-heap only). */
    asmtest_valtrace_t *v = asmtest_valtrace_new(4, 8, 0);
    if (!v) {
        CHECK(0, "api: valtrace_new");
        return;
    }
    at_val_rec_t abs_rec = mem_abs(OLD + F, 8, false);
    at_val_rec_t off_rec;
    memset(&off_rec, 0, sizeof off_rec);
    off_rec.kind = AT_LOC_MEM_OFF;
    off_rec.addr = OLD + F; /* same numeric value, but a routine offset */
    off_rec.size = 8;
    at_val_rec_t reg_rec;
    memset(&reg_rec, 0, sizeof reg_rec);
    reg_rec.kind = AT_LOC_REG;
    reg_rec.reg = 3;
    at_val_rec_t s0[] = {abs_rec, off_rec, reg_rec};
    asmtest_valtrace_append(v, 0x00, s0, 3); /* all at step 0 (pre-move) */

    asmtest_gcmove_t mv[1] = {{OLD, NEW, LEN, 1}};
    size_t changed = asmtest_gcmove_canonicalize(v, mv, 1);
    CHECK(changed == 1, "api: only the absolute-memory record is remapped");
    CHECK(v->recs[0].addr == NEW + F, "api: AT_LOC_MEM_ABS addr forwarded");
    CHECK(v->recs[1].addr == OLD + F,
          "api: AT_LOC_MEM_OFF (routine offset) left untouched");
    CHECK(v->recs[2].kind == AT_LOC_REG && v->recs[2].reg == 3,
          "api: register record left untouched");
    asmtest_valtrace_free(v);

    /* An out-of-order move set is sorted internally: the two-hop composition
     * still resolves to the final address even when passed newest-first. */
    asmtest_valtrace_t *v2 = asmtest_valtrace_new(2, 4, 0);
    if (v2) {
        at_val_rec_t w[] = {mem_abs(OLD + F, 8, true)}; /* step 0, pre-both */
        asmtest_valtrace_append(v2, 0, w, 1);
        uint64_t FINAL = 0x00300000ULL;
        /* deliberately reversed: step 3 entry before the step 1 entry */
        asmtest_gcmove_t rev[2] = {{NEW, FINAL, LEN, 3}, {OLD, NEW, LEN, 1}};
        size_t ch = asmtest_gcmove_canonicalize(v2, rev, 2);
        CHECK(ch == 1 && v2->recs[0].addr == FINAL + F,
              "api: an out-of-order move set is sorted (OLD+F -> FINAL+F)");
        asmtest_valtrace_free(v2);
    } else {
        CHECK(0, "api: valtrace_new (v2)");
    }

    /* NULL trace -> (size_t)-1; empty move set -> 0 (no-op). */
    CHECK(asmtest_gcmove_canonicalize(NULL, mv, 1) == (size_t)-1,
          "api: NULL trace returns (size_t)-1");
    asmtest_valtrace_t *v3 = asmtest_valtrace_new(1, 1, 0);
    if (v3) {
        CHECK(asmtest_gcmove_canonicalize(v3, NULL, 0) == 0,
              "api: empty move set is a no-op returning 0");
        asmtest_valtrace_free(v3);
    } else {
        CHECK(0, "api: valtrace_new (v3)");
    }
}

int main(void) {
    test_canon();
    test_survives_move();
    test_canonicalize_api();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
