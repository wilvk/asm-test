/*
 * asmtrace_record.c — the Author-mode conformance-corpus recorder.
 *
 * Runs each routine of the cross-language conformance corpus under the
 * DETERMINISTIC emulator L0 value producer and writes one `.asmtrace`
 * recording per routine: `trace` (the ordered executed-instruction stream, the
 * trace canvas's input), `df_step` (per executed step, with its operand
 * read/write values and — where Capstone is linked — its disassembly, D10)
 * plus `df_edge` (the L1 last-writer def-use graph).
 *
 * Deterministic by construction, which is what makes the output a GOLDEN
 * corpus rather than a sample: asmtest_dataflow_emu_run zeroes the guest GP
 * file and maps at fixed bases, the argument table below is fixed, and the
 * writer runs in deterministic mode (no ts/pid/cmd). Regenerating must be
 * byte-identical — see `make asmtrace-golden-check`.
 *
 * Contract: docs/internal/gui/asmtrace-schema.md.
 * Usage: asmtrace_record <output-directory>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asmtest_emu.h"      /* emu_disas (D10) */
#include "asmtest_valtrace.h" /* the L0 sink + L1 def-use graph */
#include "asmtrace_ndjson.h"  /* the shared writer (field order lives there) */

/* The emulator L0 producer. It is a TIER producer, not part of the pure public
 * sink surface, so it ships no public header and consumers re-declare it (the
 * examples/test_dataflow_emu.c precedent). */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);

/* name -> routine pointer, from the conformance corpus. */
void *asmtest_corpus_routine(const char *name);

/* The address the producer maps the routine bytes at (src/dataflow_emu.c), so
 * PC-relative operands disassemble to the same absolute targets the run saw. */
#define REC_CODE_BASE 0x00100000UL

/* The conformance emulator convention: a corpus routine is captured as a fixed
 * 64-byte window from its entry (bindings/conformance/conformance.c). */
#define REC_WINDOW 64

/* Fixed routines and arguments. INTEGER-ARG routines only — the emulator L0
 * producer marshals integer arguments (rdi/rsi/rdx/rcx/r8/r9); widening the
 * corpus to FP/vector routines needs a producer change, not a table entry. */
typedef struct {
    const char *name;
    long args[3];
    int nargs;
} rec_routine_t;

static const rec_routine_t ROUTINES[] = {
    {"add_signed", {40, 2, 0}, 2},   {"sum_via_rbx", {40, 2, 0}, 2},
    {"clobbers_rbx", {40, 2, 0}, 2}, {"sum3", {1, 2, 3}, 3},
    {"set_carry", {0, 0, 0}, 0},     {"clear_carry", {0, 0, 0}, 0},
};
#define N_ROUTINES ((int)(sizeof ROUTINES / sizeof ROUTINES[0]))

/* Record one routine. Returns 0 on success, -1 on a setup/write failure. */
static int record_one(const char *dir, const rec_routine_t *r) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    asmtest_defuse_t *g = NULL;
    uint8_t code[REC_WINDOW];
    char path[1024], body[65536];
    const void *fn = asmtest_corpus_routine(r->name);
    size_t nsteps, nrecs, cur = 0;
    int rc;

    if (!fn) {
        fprintf(stderr, "asmtrace_record: no corpus routine '%s'\n", r->name);
        return -1;
    }
    memcpy(code, fn, sizeof code);

    vt = asmtest_valtrace_new(4096, 65536, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run(code, sizeof code, r->args, r->nargs, 0, vt);
    if (rc < 0) {
        /* The producer could not even set up (no Unicorn at run time). That is
         * a lane failure, not a recording: fail loudly rather than commit an
         * empty golden file. */
        fprintf(stderr, "asmtrace_record: emulator producer failed for %s\n",
                r->name);
        asmtest_valtrace_free(vt);
        return -1;
    }
    g = asmtest_defuse_build(vt);

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, r->name);
    if (asmtrace_open(&w, path, 1 /* deterministic */) != 0) {
        fprintf(stderr, "asmtrace_record: cannot write %s\n", path);
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        return -1;
    }
    asmtrace_header(&w, "asmtrace_record", &prov, 0, NULL);

    /* A note naming the routine and its arguments, so the recording explains
     * itself without a sidecar: a walkthrough is a recording (schema `note`). */
    {
        char text[256];
        int o = snprintf(text, sizeof text, "%s(", r->name);
        for (int i = 0; i < r->nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", r->args[i]);
        snprintf(text + o, sizeof text - (size_t)o,
                 ") under the deterministic emulator L0 producer");
        asmtrace_escape(body, sizeof body, text);
        asmtrace_emitf(&w, "note", "\"text\":\"%s\"", body);
    }

    nsteps = vt->steps_len;
    nrecs = vt->recs_len;

    /* The ordered executed-instruction stream, as `trace` events. This is the
     * SAME measurement the df_step events carry (vt->insn_off[]), written in
     * the kind the trace canvas reads — so the golden corpus feeds the viewer's
     * heat map with real recorded data rather than a hand-authored imitation.
     *
     * basis is "rel": the producer maps the routine's 64-byte window at a fixed
     * base and these offsets are relative to its entry (the asmtest_trace_t
     * contract, include/asmtest_trace.h:41).
     *
     * There is deliberately NO `coverage` event. The L0 value producer records
     * executed STEPS, not basic blocks, and block starts cannot be recovered
     * from an offset stream without instruction lengths — reconstructing them
     * would be a guess wearing a measurement's clothes. A producer that
     * measures blocks writes the kind; this one does not, so it stays silent. */
    for (size_t s = 0; s < nsteps; s++) {
        char dis[160] = "";
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, sizeof code, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        if (dis[0]) {
            asmtrace_escape(body, sizeof body, dis);
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu,"
                           "\"disasm\":\"%s\"",
                           (unsigned long long)vt->insn_off[s], body);
        } else {
            asmtrace_emitf(&w, "trace",
                           "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu",
                           (unsigned long long)vt->insn_off[s]);
        }
    }

    for (size_t s = 0; s < nsteps; s++) {
        char dis[160] = "";
        size_t first;
        if (emu_disas_available())
            emu_disas(EMU_ARCH_X86_64, code, sizeof code, REC_CODE_BASE,
                      vt->insn_off[s], dis, sizeof dis);
        while (cur < nrecs && vt->recs[cur].step < s)
            cur++;
        first = cur;
        while (cur < nrecs && vt->recs[cur].step == s)
            cur++;
        asmtrace_df_step_body(body, sizeof body, (unsigned)s, vt->insn_off[s],
                              dis, &vt->recs[first], cur - first);
        asmtrace_emit(&w, "df_step", body);
    }
    for (size_t i = 0; g && i < g->n; i++) {
        asmtrace_df_edge_body(body, sizeof body, &g->edges[i]);
        asmtrace_emit(&w, "df_edge", body);
    }
    if (vt->truncated)
        w.truncated = 1;

    /* rc == 1 means the guest faulted or errored: a PARTIAL trace was still
     * produced, so the recording is real — but it must say so rather than look
     * like a clean run. */
    if (rc > 0) {
        asmtrace_prov_t skip = {"emu-l0",
                                1,
                                "exact",
                                1,
                                "guest faulted or errored; trace is partial",
                                0};
        asmtrace_close(&w, 0, 0, &skip);
    } else {
        asmtrace_close(&w, 0, 0, NULL);
    }

    {
        size_t nedges = g ? g->n : (size_t)0; /* read BEFORE the free */
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        printf("  %-14s %zu steps, %zu records, %zu def-use edges\n", r->name,
               nsteps, nrecs, nedges);
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = argc >= 2 ? argv[1] : "tests/golden-asmtrace";
    int failed = 0;
    if (!emu_disas_available())
        fprintf(
            stderr,
            "asmtrace_record: WARNING — no Capstone, so `disasm` fields are "
            "omitted (D10 degradation). These bytes are NOT the golden "
            "corpus; regenerate in the docker-cli image.\n");
    printf("asmtrace_record -> %s\n", dir);
    for (int i = 0; i < N_ROUTINES; i++)
        if (record_one(dir, &ROUTINES[i]) != 0)
            failed++;
    if (failed) {
        fprintf(stderr, "asmtrace_record: %d routine(s) failed\n", failed);
        return 1;
    }
    return 0;
}
