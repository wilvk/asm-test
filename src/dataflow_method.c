/*
 * dataflow_method.c — Phase 4 (increment 1): the PC -> (method, version)
 * resolver over an L0 value trace. See asmtest_valtrace.h.
 *
 * This is the managed-taint PREREQUISITE: given a method-map (the shape the
 * asmspy jitdump reader, the text perf-map, and the §D3 addr-channel all
 * produce — {addr, size, name, version/code_index}) and a recorded
 * asmtest_valtrace_t, label each executed step with the method + version whose
 * JIT-compiled body owns its instruction PC. Tiered re-JIT is resolved by the
 * version field: an in-place recompile at a reused address is disambiguated by
 * "greatest version wins", and a re-JIT to a new address is a new record whose
 * PCs carry the new version (old-body PCs still resolve to the old one).
 *
 * PURE C — no Capstone, no Unicorn — the same dependency tier as dataflow.c, so
 * it compiles and unit-tests on every host. GC-move object canonicalization is
 * deliberately deferred to a later increment.
 */
#include "asmtest_valtrace.h"

#include <stdlib.h>
#include <string.h>

/* Does record `m` own `pc`? A sized record is the half-open span
 * [addr, addr+size); a size == 0 record (unknown extent, e.g. a bare perf-map
 * line) matches only its exact start address. addr+size is computed so a span
 * that wraps the 64-bit space still rejects cleanly. */
static bool method_owns(const asmtest_method_t *m, uint64_t pc) {
    if (m->size == 0)
        return pc == m->addr;
    uint64_t end = m->addr + m->size; /* half-open upper bound */
    if (end < m->addr) /* pathological wrap: treat as unbounded */
        return pc >= m->addr;
    return pc >= m->addr && pc < end;
}

int asmtest_method_resolve_pc(const asmtest_method_t *methods, size_t nmethods,
                              uint64_t pc) {
    if (methods == NULL)
        return -1;
    int best = -1;
    uint64_t best_ver = 0;
    for (size_t i = 0; i < nmethods; i++) {
        if (!method_owns(&methods[i], pc))
            continue;
        /* Greatest version wins on an in-place tiered-recompile collision;
         * an equal version resolves to the LATER record (the newest load),
         * mirroring the jitdump reader's "newest code_index wins". */
        if (best < 0 || methods[i].version >= best_ver) {
            best = (int)i;
            best_ver = methods[i].version;
        }
    }
    return best;
}

/* True iff two method names denote the same method. A NULL name matches nothing
 * (each NULL-named record is its own identity), so an unnamed region never
 * collapses distinct methods together. */
static bool names_equal(const char *a, const char *b) {
    if (a == NULL || b == NULL)
        return false;
    return strcmp(a, b) == 0;
}

int asmtest_method_attribute(const asmtest_method_t *methods, size_t nmethods,
                             const asmtest_valtrace_t *v,
                             asmtest_method_attr_t *out, size_t out_cap) {
    if (v == NULL || out == NULL)
        return -1;

    /* Assign each distinct method NAME a stable, compact identity id in
     * first-seen order, so all records naming one method share a `method` id
     * across tiered re-JITs (in-place or moved). Built once per call. */
    int *id = NULL;
    if (nmethods > 0) {
        id = (int *)malloc(nmethods * sizeof(int));
        if (id != NULL) {
            int next_id = 0;
            for (size_t i = 0; i < nmethods; i++)
                id[i] = -1;
            for (size_t i = 0; i < nmethods; i++) {
                if (id[i] >= 0)
                    continue;
                int this_id = next_id++;
                id[i] = this_id;
                for (size_t j = i + 1; j < nmethods; j++)
                    if (id[j] < 0 &&
                        names_equal(methods[i].name, methods[j].name))
                        id[j] = this_id;
            }
        }
        /* If the (tiny) id table could not be allocated we still attribute the
         * record + version below; only the grouped identity degrades to -1. */
    }

    size_t nsteps = v->steps_len;
    if (nsteps > out_cap)
        nsteps = out_cap;
    for (size_t s = 0; s < nsteps; s++) {
        uint64_t pc = v->insn_off != NULL ? v->insn_off[s] : 0;
        int rec = asmtest_method_resolve_pc(methods, nmethods, pc);
        out[s].record = rec;
        if (rec >= 0) {
            out[s].version = methods[rec].version;
            out[s].method = (id != NULL) ? id[rec] : -1;
        } else {
            out[s].version = 0;
            out[s].method = -1;
        }
    }

    free(id);
    return (int)nsteps;
}
