/*
 * test_dataflow_method.c — Phase 4 (increment 1): the PC -> (method, version)
 * resolver over an L0 value trace, validated HOST-INDEPENDENTLY over a SYNTHETIC
 * method-map + value trace. No Capstone, no Unicorn, no .NET SDK — pure address
 * math, so these checks run and pass on EVERY CI host, exactly like
 * test_dataflow.c's pure spine.
 *
 * The synthetic method-map exercises both tiered-re-JIT shapes the resolver must
 * disambiguate by version:
 *   - an IN-PLACE recompile at a REUSED address (two records share a start;
 *     the greater version wins);
 *   - a re-JIT to a NEW address (a moved method body = a new version whose PCs
 *     attribute to the new version while the old body's PCs still resolve to the
 *     old one) — this is the "a PC that moved" case.
 * plus a size == 0 point-match method, half-open extent boundaries, unattributed
 * PCs, and stable method IDENTITY that survives a re-JIT (same `method` id,
 * different `version`). GC-move object identity is DEFERRED to a later increment.
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

/*
 * The synthetic method-map (unsorted on purpose — the resolver does not require
 * a sorted map). Record indices are load-bearing for the assertions below.
 *
 *   idx name   addr     size    ver   note
 *   ---------------------------------------------------------------------------
 *    0  Foo     0x1000   0x40     1    original tier-0 Foo
 *    1  Foo     0x1000   0x40     3    Foo recompiled IN PLACE (reused addr) ->
 *                                       greater version wins for [0x1000,0x1040)
 *    2  Bar     0x2000   0x50     1
 *    3  Foo     0x8000   0x20     5    Foo re-JITted to a NEW address (moved) =
 *                                       a new version; identity is still Foo
 *    4  Baz     0x3000   0        2    unknown-extent method (point match on addr)
 */
static const asmtest_method_t MAP[] = {
    {0x1000, 0x40, "Foo", 1}, {0x1000, 0x40, "Foo", 3},
    {0x2000, 0x50, "Bar", 1}, {0x8000, 0x20, "Foo", 5},
    {0x3000, 0x00, "Baz", 2},
};
enum { NMAP = (int)(sizeof MAP / sizeof MAP[0]) };

/* ---- the single-PC resolver: extent, tiered collision, point match ------- */
static void test_resolve_pc(void) {
    /* in-place tiered recompile at a reused address: v3 (idx 1) beats v1 (idx 0) */
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x1010) == 1,
          "resolve: reused-address tiered re-JIT -> greatest version (idx 1, "
          "v3)");
    CHECK(
        asmtest_method_resolve_pc(MAP, NMAP, 0x1000) == 1,
        "resolve: start address also resolves to the newest in-place version");

    /* moved method body: a distinct record at a new address */
    CHECK(
        asmtest_method_resolve_pc(MAP, NMAP, 0x8005) == 3,
        "resolve: moved (re-JIT to new address) -> the new record (idx 3, v5)");

    /* the neighbouring method */
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x2010) == 2,
          "resolve: Bar owns its own extent (idx 2)");

    /* size == 0 record: exact-start point match only */
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x3000) == 4,
          "resolve: size==0 record matches its exact start address (idx 4)");
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x3001) == -1,
          "resolve: size==0 record does NOT match one byte past its start");

    /* half-open extent: addr+size is NOT owned */
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x103f) == 1,
          "resolve: last byte of the extent is owned (0x103f)");
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x1040) == -1,
          "resolve: one past the extent (addr+size) is NOT owned (half-open)");

    /* a PC in no method at all */
    CHECK(asmtest_method_resolve_pc(MAP, NMAP, 0x9999) == -1,
          "resolve: a PC outside every method attributes to nothing (-1)");

    /* NULL / empty map guards */
    CHECK(asmtest_method_resolve_pc(NULL, 0, 0x1010) == -1,
          "resolve: NULL map -> -1");
    CHECK(asmtest_method_resolve_pc(MAP, 0, 0x1010) == -1,
          "resolve: empty map -> -1");
}

/* Build a value trace whose per-step instruction offsets are the PCs below.
 * Attribution reads only insn_off + steps_len, so each step carries no operand
 * records (n == 0). */
static const uint64_t PCS[] = {
    0x1010, /* step0: Foo v3 (idx 1) — newest in-place version   */
    0x2010, /* step1: Bar v1 (idx 2)                             */
    0x8005, /* step2: Foo v5 (idx 3) — moved re-JIT, same method */
    0x3000, /* step3: Baz v2 (idx 4) — point match               */
    0x3001, /* step4: unattributed (point-match miss)            */
    0x9999, /* step5: unattributed (outside every method)        */
    0x1040, /* step6: unattributed (half-open upper bound)       */
};
enum { NSTEPS = (int)(sizeof PCS / sizeof PCS[0]) };

static asmtest_valtrace_t *build_trace(void) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(NSTEPS, 1, 0);
    if (v == NULL)
        return NULL;
    for (int i = 0; i < NSTEPS; i++)
        asmtest_valtrace_append(v, PCS[i], NULL, 0);
    return v;
}

/* ---- full attribution over the value trace ------------------------------- */
static void test_attribute(void) {
    asmtest_valtrace_t *v = build_trace();
    if (v == NULL) {
        CHECK(0, "attribute: build_trace");
        return;
    }
    CHECK(v->steps_len == (size_t)NSTEPS, "attribute: trace has all steps");

    asmtest_method_attr_t attr[NSTEPS];
    int n = asmtest_method_attribute(MAP, NMAP, v, attr, NSTEPS);
    CHECK(n == NSTEPS, "attribute: writes one entry per step");

    /* per-step record + version — the core PC -> (method, version) mapping */
    CHECK(attr[0].record == 1 && attr[0].version == 3,
          "attribute: step0 -> Foo record 1, version 3 (newest in-place)");
    CHECK(attr[1].record == 2 && attr[1].version == 1,
          "attribute: step1 -> Bar record 2, version 1");
    CHECK(attr[2].record == 3 && attr[2].version == 5,
          "attribute: step2 -> Foo record 3, version 5 (moved re-JIT)");
    CHECK(attr[3].record == 4 && attr[3].version == 2,
          "attribute: step3 -> Baz record 4, version 2 (point match)");
    CHECK(attr[4].record == -1 && attr[4].method == -1 && attr[4].version == 0,
          "attribute: step4 unattributed (point-match miss)");
    CHECK(attr[5].record == -1 && attr[5].method == -1,
          "attribute: step5 unattributed (outside every method)");
    CHECK(attr[6].record == -1 && attr[6].method == -1,
          "attribute: step6 unattributed (half-open upper bound)");

    /* THE headline: method IDENTITY survives a tiered re-JIT. step0 (Foo v3,
     * in-place) and step2 (Foo v5, moved) are the SAME method — same `method`
     * id — but DISTINCT versions. */
    CHECK(attr[0].method >= 0 && attr[0].method == attr[2].method,
          "attribute: Foo keeps one identity across in-place + moved re-JIT");
    CHECK(attr[0].version != attr[2].version,
          "attribute: ... while the version distinguishes the two Foo bodies");

    /* distinct methods get distinct identities */
    CHECK(attr[0].method != attr[1].method,
          "attribute: Foo and Bar are different identities");
    CHECK(attr[1].method != attr[3].method && attr[0].method != attr[3].method,
          "attribute: Bar and Baz are distinct from Foo and each other");

    /* out_cap truncation: only the first out_cap steps are written */
    asmtest_method_attr_t few[3];
    int m = asmtest_method_attribute(MAP, NMAP, v, few, 3);
    CHECK(m == 3, "attribute: out_cap caps the number of steps written");
    CHECK(few[0].version == 3 && few[2].version == 5,
          "attribute: the capped prefix still attributes correctly");

    /* empty method-map: every step unattributed, count still honest */
    asmtest_method_attr_t none[NSTEPS];
    int e = asmtest_method_attribute(NULL, 0, v, none, NSTEPS);
    CHECK(e == NSTEPS && none[0].record == -1 && none[0].method == -1,
          "attribute: NULL/empty map attributes every step to -1");

    /* NULL guards */
    CHECK(asmtest_method_attribute(MAP, NMAP, NULL, attr, NSTEPS) == -1,
          "attribute: NULL trace -> -1");
    CHECK(asmtest_method_attribute(MAP, NMAP, v, NULL, NSTEPS) == -1,
          "attribute: NULL out -> -1");

    asmtest_valtrace_free(v);
}

int main(void) {
    test_resolve_pc();
    test_attribute();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
