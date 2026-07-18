/*
 * test_dataflow_objid.c — Phase 4 (increment 4): real OBJECT identity,
 * validated over SYNTHETIC value traces + synthetic heap-snapshot nodes +
 * synthetic move ranges built by hand. PURE — no Capstone, no Unicorn, no
 * runtime, no .NET SDK — so it runs and passes on EVERY CI host, exactly like
 * test_dataflow_gcmove's pure spine. It probes the increment-4 exit criterion:
 * "a managed value can no longer be attributed to the WRONG object across a
 * compaction", the residual address identity alone cannot express.
 *
 *   locate:      asmtest_objid_locate inverts asmtest_gcmove_canon —
 *                locate(canon(x,s),s) == x for every x that is an object's
 *                location at step s, over disjoint and chained move sets.
 *   owner:       asmtest_objid_owner names the node that CONTAINED a raw byte
 *                at a step (offset preserved), declines a vacated/foreign byte,
 *                and canonicalize forwards an owned byte to node.addr+off, which
 *                is byte-equal to the forward canon.
 *   false alias: the store-into-doomed / load-of-live pair that address
 *                identity FALSELY links (the suite-level negative control) is
 *                SEVERED by object identity — the store re-keyed | NOOBJ, the
 *                load keyed to the live node, no def-use edge between them.
 *   degrade:     with no nodes (or nodes disjoint from every access) the result
 *                is byte-identical to asmtest_gcmove_canonicalize.
 *   overlap:     a composed batch with overlapping NEW spans yields "no owner"
 *                (a conservative miss), never a guessed identity.
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

static uint32_t lcg(uint32_t *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s;
}

/* ---- test 1: locate is the exact inverse of the forward canon ------------ */
static void test_locate_roundtrip(void) {
    /* Two INDEPENDENT objects: object1 A1->B1 at boundary 2, object2 A2->B2 at
     * boundary 5. Disjoint old AND new spans. */
    asmtest_gcmove_t d[2] = {{0x100000, 0x180000, 0x40, 2},
                             {0x200000, 0x280000, 0x40, 5}};
    /* ONE object chained through two hops: A -> M at 2, then M -> F at 5. */
    asmtest_gcmove_t c[2] = {{0x100000, 0x300000, 0x40, 2},
                             {0x300000, 0x500000, 0x40, 5}};

    /* Each row is (move set, nmoves, object's LOCATION base at that step, step)
     * — locate(canon(x,s),s) must round-trip for x anywhere inside that object,
     * tested both before and after each boundary the object is still live at. */
    struct {
        const asmtest_gcmove_t *mv;
        size_t n;
        uint64_t base;
        uint32_t step;
    } cs[] = {
        {d, 2, 0x100000, 0}, {d, 2, 0x100000, 1}, /* obj1 pre-move  (s < 2) */
        {d, 2, 0x180000, 2}, {d, 2, 0x180000, 9}, /* obj1 post-move (final)  */
        {d, 2, 0x200000, 0}, {d, 2, 0x200000, 4}, /* obj2 pre-move  (s < 5) */
        {d, 2, 0x280000, 5}, {d, 2, 0x280000, 9}, /* obj2 post-move (final)  */
        {c, 2, 0x100000, 0}, {c, 2, 0x100000, 1}, /* chain origin   (s < 2) */
        {c, 2, 0x300000, 2}, {c, 2, 0x300000, 4}, /* chain midpoint (2..5)  */
        {c, 2, 0x500000, 5},                      /* chain final            */
    };
    uint64_t offs[] = {0, 8, 0x10, 0x38};
    int ok = 1;
    for (size_t r = 0; r < sizeof cs / sizeof cs[0]; r++)
        for (size_t o = 0; o < sizeof offs / sizeof offs[0]; o++) {
            uint64_t x = cs[r].base + offs[o];
            uint64_t fwd =
                asmtest_gcmove_canon(cs[r].mv, cs[r].n, cs[r].step, x);
            if (asmtest_objid_locate(cs[r].mv, cs[r].n, cs[r].step, fwd) != x)
                ok = 0;
        }
    CHECK(ok,
          "locate: locate(canon(x,s),s) == x round-trips (disjoint+chained)");

    /* An address outside every range round-trips as the identity. */
    CHECK(asmtest_objid_locate(
              d, 2, 0, asmtest_gcmove_canon(d, 2, 0, 0xDEAD0)) == 0xDEAD0,
          "locate: an address outside all ranges round-trips (identity)");
    CHECK(asmtest_objid_locate(NULL, 0, 0, 0xABCD) == 0xABCD,
          "locate: empty move set is the identity map");
}

/* ---- test 2: owner naming + the owner branch of canonicalize ------------- */
static void test_owner(void) {
    asmtest_gcnode_t nd[1] = {
        {0x200000, 0x20, 7}}; /* object at B in snapshot */
    asmtest_gcmove_t mv[1] = {{0x100000, 0x200000, 0x20, 5}}; /* A -> B at 5 */
    size_t owner = 99;
    uint64_t off = 99;

    CHECK(asmtest_objid_owner(nd, 1, mv, 1, 4, 0x100000, &owner, &off) == 0 &&
              owner == 0 && off == 0,
          "owner: A at step 4 (pre-move) is the node, offset 0");
    CHECK(asmtest_objid_owner(nd, 1, mv, 1, 4, 0x100010, &owner, &off) == 0 &&
              owner == 0 && off == 0x10,
          "owner: field offset preserved through the inverse walk");
    CHECK(asmtest_objid_owner(nd, 1, mv, 1, 5, 0x100000, &owner, &off) == -1,
          "owner: A at step 5 (post-move, vacated) has no owner");
    CHECK(asmtest_objid_owner(nd, 1, mv, 1, 5, 0x200000, &owner, &off) == 0 &&
              owner == 0 && off == 0,
          "owner: B at step 5 (post-move) is the node");
    CHECK(asmtest_objid_owner(nd, 1, mv, 1, 4, 0xDEAD0000, NULL, NULL) == -1,
          "owner: an unrelated address has no owner");

    /* The owner branch of canonicalize forwards an owned byte to node.addr+off,
     * and that equals the forward canon for the same byte (the invariant that a
     * true edge surviving gcmove_canonicalize survives objid unchanged). */
    asmtest_valtrace_t *vo = asmtest_valtrace_new(2, 2, 0);
    if (vo) {
        at_val_rec_t rr[] = {
            mem_abs(0x100010, 8, false)}; /* A+0x10, pre-move */
        asmtest_valtrace_append(vo, 0, rr, 1);
        asmtest_valtrace_append(vo, 8, NULL, 0);
        size_t ch = asmtest_objid_canonicalize(vo, nd, 1, mv, 1);
        uint64_t canon = asmtest_gcmove_canon(mv, 1, 0, 0x100010);
        CHECK(ch == 1 && vo->recs[0].addr == 0x200010 &&
                  vo->recs[0].addr == canon,
              "owner: canonicalize keys an owned byte to node.addr+off (== "
              "canon)");
        asmtest_valtrace_free(vo);
    } else {
        CHECK(0, "owner: valtrace_new");
    }
}

/* ---- test 3: the false alias, forged by address identity, killed by objid  *
 *
 * Object O lives at L at step 2, is slid L->X by a GC at boundary 5, and sits
 * at X in the snapshot (node O = {X, 8}). A raw trace:
 *   step2: WRITE M[X]   into the memory O will later occupy — NOT O yet
 *   step7: READ  M[X]   O.field, AFTER the slide — this IS O
 * Under ADDRESS identity both key on X (canon(X,2)==canon(X,7)==X) and a FALSE
 * def-use edge 2->7 is forged (the suite-level negative control). Under OBJECT
 * identity the write is re-keyed X|NOOBJ (no owner, but canon(X) hits O's range)
 * and the read keys O.addr (owner), so the edge is gone.
 */
#define AX 0x200000ULL /* O's snapshot address == the aliasing address X */
#define AL 0x100000ULL /* O's location at step 2 */

static asmtest_valtrace_t *build_alias_trace(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(8, 4, 0);
    if (v == NULL)
        return NULL;
    at_val_rec_t w[] = {mem_abs(AX, 8, true)};
    at_val_rec_t rd[] = {mem_abs(AX, 8, false)};
    asmtest_valtrace_append(v, 0x00, NULL, 0); /* step 0 */
    asmtest_valtrace_append(v, 0x08, NULL, 0); /* step 1 */
    asmtest_valtrace_append(v, 0x10, w, 1); /* step 2: WRITE X (doomed mem) */
    asmtest_valtrace_append(v, 0x18, NULL, 0); /* step 3 */
    asmtest_valtrace_append(v, 0x20, NULL, 0); /* step 4 */
    asmtest_valtrace_append(v, 0x28, NULL, 0); /* step 5 (GC slides O to X) */
    asmtest_valtrace_append(v, 0x30, NULL, 0); /* step 6 */
    asmtest_valtrace_append(v, 0x38, rd, 1);   /* step 7: READ X (now O)      */
    return v;
}

static void test_false_alias_killed(void) {
    asmtest_gcnode_t o[1] = {{AX, 8, 7}};      /* O at X in the snapshot */
    asmtest_gcmove_t mv[1] = {{AL, AX, 8, 5}}; /* O slid L -> X at boundary 5 */

    /* (a) NEGATIVE CONTROL: address identity forges the false edge. */
    asmtest_valtrace_t *va = build_alias_trace();
    if (va == NULL) {
        CHECK(0, "alias: build (control)");
        return;
    }
    size_t chg = asmtest_gcmove_canonicalize(va, mv, 1);
    asmtest_defuse_t *ga = asmtest_defuse_build(va);
    CHECK(va->recs[0].addr == va->recs[1].addr,
          "alias/raw: address identity keys the store and load equal (X==X)");
    CHECK(ga && has_edge(ga, 2, 7),
          "alias/raw: address identity FORGES the false def-use edge 2->7");
    (void)chg;
    asmtest_defuse_free(ga);
    asmtest_valtrace_free(va);

    /* (b) THE POSITIVE: object identity severs it. */
    asmtest_valtrace_t *vb = build_alias_trace();
    if (vb == NULL) {
        CHECK(0, "alias: build (objid)");
        return;
    }
    size_t ch = asmtest_objid_canonicalize(vb, o, 1, mv, 1);
    CHECK(ch >= 1 && vb->recs[0].addr == (AX | ASMTEST_OBJID_NOOBJ),
          "alias: the store (no owner, canon hits O) is re-keyed X|NOOBJ");
    CHECK(vb->recs[1].addr == AX,
          "alias: the load (owner O) keys the object at node.addr+0");
    asmtest_defuse_t *gb = asmtest_defuse_build(vb);
    CHECK(gb && !has_edge(gb, 2, 7),
          "alias: object identity SEVERS the false edge (store X|NOOBJ != X)");
    asmtest_defuse_free(gb);
    asmtest_valtrace_free(vb);
}

/* ---- test 4: degradation — objid refines, never replaces, address identity */
static asmtest_valtrace_t *build_rand_trace(uint32_t seed) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(160, 160, 0);
    if (v == NULL)
        return NULL;
    for (int i = 0; i < 150; i++) {
        uint32_t k = lcg(&seed) % 10;
        at_val_rec_t rec;
        memset(&rec, 0, sizeof rec);
        if (k == 0) { /* a register record — must be left untouched */
            rec.kind = AT_LOC_REG;
            rec.reg = 1 + (lcg(&seed) % 16);
        } else if (k == 1) { /* a routine-offset record — must be untouched */
            rec.kind = AT_LOC_MEM_OFF;
            rec.addr = 0x100 + (lcg(&seed) % 4096);
            rec.size = 8;
        } else { /* an absolute-memory record */
            rec.kind = AT_LOC_MEM_ABS;
            if (lcg(&seed) & 1) /* often inside the move region -> forwarded */
                rec.addr = 0x10000000ULL +
                           (uint64_t)(lcg(&seed) % 4096) * 0x1000 +
                           (lcg(&seed) % 0x40);
            else
                rec.addr = 0x20000000ULL + (uint64_t)(lcg(&seed) % 0x100000);
            rec.size = (uint16_t)(1 + (lcg(&seed) % 16));
        }
        rec.is_write = (lcg(&seed) & 1) != 0;
        asmtest_valtrace_append(v, (uint64_t)i * 4, &rec, 1);
    }
    return v;
}

static int addrs_identical(const asmtest_valtrace_t *a,
                           const asmtest_valtrace_t *b) {
    if (a->recs_len != b->recs_len)
        return 0;
    for (size_t r = 0; r < a->recs_len; r++)
        if (a->recs[r].addr != b->recs[r].addr)
            return 0;
    return 1;
}

static void test_degradation(void) {
    /* One range per batch (unique, strictly increasing steps) so the forward
     * canon is order-independent and gcmove vs objid must agree exactly. */
    asmtest_gcmove_t mv[16];
    uint32_t mseed = 0x0BADF00Du, stepv = 1;
    for (size_t i = 0; i < 16; i++) {
        stepv += 1 + (lcg(&mseed) % 5);
        mv[i].old_base =
            0x10000000ULL + (uint64_t)(lcg(&mseed) % 4096) * 0x1000;
        mv[i].new_base =
            0x50000000ULL + (uint64_t)(lcg(&mseed) % 4096) * 0x1000;
        mv[i].len = 0x40 + (uint64_t)(lcg(&mseed) % 16) * 8;
        mv[i].step = stepv;
    }

    /* (a) nnodes == 0: exactly asmtest_gcmove_canonicalize. */
    asmtest_valtrace_t *va = build_rand_trace(0xABCDEF01u);
    asmtest_valtrace_t *vb = build_rand_trace(0xABCDEF01u);
    if (va && vb) {
        size_t ca = asmtest_objid_canonicalize(va, NULL, 0, mv, 16);
        size_t cb = asmtest_gcmove_canonicalize(vb, mv, 16);
        CHECK(addrs_identical(va, vb),
              "degrade: nnodes==0 is byte-identical to gcmove_canonicalize");
        CHECK(ca == cb, "degrade: nnodes==0 reports the same change count");
    } else {
        CHECK(0, "degrade: build (a)");
        CHECK(0, "degrade: build (a) count");
    }
    asmtest_valtrace_free(va);
    asmtest_valtrace_free(vb);

    /* (b) nodes present but DISJOINT from every access AND every canon output:
     * the real per-record loop runs (owner always declines, collision never
     * fires) and still degrades to the address floor. */
    asmtest_gcnode_t far[2] = {{0x90000000ULL, 0x40, 1},
                               {0x91000000ULL, 0x40, 2}};
    asmtest_valtrace_t *vc = build_rand_trace(0x13572468u);
    asmtest_valtrace_t *vd = build_rand_trace(0x13572468u);
    if (vc && vd) {
        size_t cc = asmtest_objid_canonicalize(vc, far, 2, mv, 16);
        size_t cd = asmtest_gcmove_canonicalize(vd, mv, 16);
        CHECK(addrs_identical(vc, vd),
              "degrade: nodes disjoint from all accesses == address floor");
        CHECK(cc == cd, "degrade: disjoint-node change count matches gcmove");
    } else {
        CHECK(0, "degrade: build (b)");
        CHECK(0, "degrade: build (b) count");
    }
    asmtest_valtrace_free(vc);
    asmtest_valtrace_free(vd);
}

/* ---- test 5: composed batch, overlapping NEW spans -> conservative miss --- */
static void test_new_overlap_drop(void) {
    uint64_t A = 0x100000, B = 0x200000, M = 0x500000;
    /* Composed batch (same step): P slid A->M, then Q slid B->M into P's freed
     * image. Only Q survives to the snapshot, a live node at M. */
    asmtest_gcmove_t mv[2] = {{A, M, 0x20, 1}, {B, M, 0x20, 1}};
    asmtest_gcnode_t q[1] = {{M, 0x20, 7}};
    size_t owner = 99;
    uint64_t off = 99;

    CHECK(asmtest_objid_owner(q, 1, mv, 2, 0, A, &owner, &off) == -1,
          "overlap: raw A at step 0 is NOT claimed (both news dropped)");
    CHECK(asmtest_objid_owner(q, 1, mv, 2, 0, B, &owner, &off) == -1,
          "overlap: raw B at step 0 is NOT claimed (conservative miss)");

    /* Control: with NO overlap (Q's move alone), Q's step-0 location DOES invert
     * to B, so the machinery is not simply always declining. */
    asmtest_gcmove_t solo[1] = {{B, M, 0x20, 1}};
    CHECK(asmtest_objid_owner(q, 1, solo, 1, 0, B, &owner, &off) == 0 &&
              owner == 0 && off == 0,
          "overlap: control — without the collision, B at step 0 IS Q");
}

/* ---- API guards: NULL trace, NULL nodes, record selectivity -------------- */
static void test_api(void) {
    asmtest_gcnode_t nd[1] = {{0x200000, 0x20, 7}};
    asmtest_gcmove_t mv[1] = {{0x100000, 0x200000, 8, 1}};

    CHECK(asmtest_objid_canonicalize(NULL, nd, 1, mv, 1) == (size_t)-1,
          "api: NULL trace returns (size_t)-1");
    CHECK(asmtest_objid_owner(NULL, 0, mv, 1, 0, 0x100000, NULL, NULL) == -1,
          "api: owner over an empty node set returns -1");

    /* NULL nodes -> pure address identity (delegates to gcmove_canonicalize). */
    asmtest_valtrace_t *v = asmtest_valtrace_new(2, 2, 0);
    if (v) {
        at_val_rec_t w[] = {mem_abs(0x100000, 8, true)};
        asmtest_valtrace_append(v, 0, w, 1);
        asmtest_valtrace_append(v, 8, NULL, 0);
        size_t ch = asmtest_objid_canonicalize(v, NULL, 0, mv, 1);
        CHECK(ch == 1 && v->recs[0].addr == 0x200000,
              "api: NULL nodes == address identity (OLD forwarded to NEW)");
        asmtest_valtrace_free(v);
    } else {
        CHECK(0, "api: valtrace_new");
    }

    /* Only AT_LOC_MEM_ABS is rewritten; a routine-offset and a register record
     * with the same numeric addr are left untouched. */
    asmtest_valtrace_t *v2 = asmtest_valtrace_new(1, 3, 0);
    if (v2) {
        at_val_rec_t abs_rec = mem_abs(0x200000, 8, false);
        at_val_rec_t off_rec;
        memset(&off_rec, 0, sizeof off_rec);
        off_rec.kind = AT_LOC_MEM_OFF;
        off_rec.addr = 0x200000;
        off_rec.size = 8;
        at_val_rec_t reg_rec;
        memset(&reg_rec, 0, sizeof reg_rec);
        reg_rec.kind = AT_LOC_REG;
        reg_rec.reg = 3;
        at_val_rec_t s0[] = {abs_rec, off_rec, reg_rec};
        asmtest_valtrace_append(v2, 0, s0, 3);
        asmtest_gcnode_t n2[1] = {{0x200000, 0x20, 5}};
        size_t ch = asmtest_objid_canonicalize(v2, n2, 1, NULL, 0);
        CHECK(v2->recs[1].kind == AT_LOC_MEM_OFF &&
                  v2->recs[1].addr == 0x200000,
              "api: AT_LOC_MEM_OFF (routine offset) left untouched");
        CHECK(v2->recs[2].kind == AT_LOC_REG && v2->recs[2].reg == 3,
              "api: register record left untouched");
        CHECK(ch == 0 && v2->recs[0].addr == 0x200000,
              "api: owned abs record keys to node.addr (no move -> no change)");
        asmtest_valtrace_free(v2);
    } else {
        CHECK(0, "api: valtrace_new (v2)");
    }
}

int main(void) {
    test_locate_roundtrip();
    test_owner();
    test_false_alias_killed();
    test_degradation();
    test_new_overlap_drop();
    test_api();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
