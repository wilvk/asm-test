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

/* ------------------------------------------------------------------ */
/* The Loom's golden fixtures (docs/internal/gui/05-loom-day-one.md T7) */
/*                                                                     */
/* Unlike the corpus routines above these are BYTE LITERALS, because    */
/* the two walkthroughs the doc builds on are hand-derivable on paper   */
/* and must stay so: their listings are right here beside the bytes.    */
/* ------------------------------------------------------------------ */

/* examples/test_dataflow_emu.c:38 — the def-use chain the whole doc walks:
 *   0x00 mov rax, rdi          0x03 mov [rsp-8], rax     0x08 mov rcx, [rsp-8]
 *   0x0d lea rdx, [rcx+rsi]    0x11 mov rax, rdx         0x14 ret            */
static const uint8_t LOOM_DF_CHAIN[] = {
    0x48, 0x89, 0xf8, 0x48, 0x89, 0x44, 0x24, 0xf8, 0x48, 0x8b, 0x4c,
    0x24, 0xf8, 0x48, 0x8d, 0x14, 0x31, 0x48, 0x89, 0xd0, 0xc3,
};

/* fork_demo(a)  [rdi=a] — one dimmed, one hot, one control divergence:
 *   0x00  mov rdx, rdi      ; hot: the value differs across takes
 *   0x03  shr rdx, 63       ; DIMMED: 0 for both non-negative args
 *   0x07  mov rax, rdi      ; hot
 *   0x0a  cmp rax, 10       ; hot (EFLAGS value differs)
 *   0x0e  jle 0x13          ; last aligned step
 *   0x10  neg rax           ; runs only when a > 10
 *   0x13  ret
 * desktop/test/test_loom_forks.cpp assembles this listing with Keystone and
 * asserts the result is byte-identical to the table below — so the "hand-verify
 * the encodings once" step is a test rather than a promise. */
static const uint8_t LOOM_FORK_DEMO[] = {
    0x48, 0x89, 0xfa,       /* 0x00 mov rdx, rdi  */
    0x48, 0xc1, 0xea, 0x3f, /* 0x03 shr rdx, 63   */
    0x48, 0x89, 0xf8,       /* 0x07 mov rax, rdi  */
    0x48, 0x83, 0xf8, 0x0a, /* 0x0a cmp rax, 10   */
    0x7e, 0x03,             /* 0x0e jle 0x13      */
    0x48, 0xf7, 0xd8,       /* 0x10 neg rax       */
    0xc3,                   /* 0x13 ret           */
};

/*
 * Record `code[0..code_len)` under the producer and write <dir>/<out>.asmtrace.
 * `label` is what the recording's `note` calls the routine; `recs_cap` bounds
 * the operand buffer (a SMALL cap is how the truncated loom fixture is made —
 * the producer then flips `truncated` and the footer says so, which is a D7
 * dishonesty fixture rather than a hand-edited file).
 * Returns 0 on success, -1 on a setup/write failure.
 */
static int record_bytes(const char *dir, const char *out, const char *label,
                        const uint8_t *code, size_t code_len, const long *args,
                        int nargs, size_t recs_cap) {
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    asmtest_valtrace_t *vt = NULL;
    asmtest_defuse_t *g = NULL;
    char path[1024], body[65536];
    size_t nsteps, nrecs, cur = 0;
    int rc;

    vt = asmtest_valtrace_new(4096, recs_cap, 4096);
    if (!vt) {
        fprintf(stderr, "asmtrace_record: out of memory\n");
        return -1;
    }
    rc = asmtest_dataflow_emu_run(code, code_len, args, nargs, 0, vt);
    if (rc < 0) {
        /* The producer could not even set up (no Unicorn at run time). That is
         * a lane failure, not a recording: fail loudly rather than commit an
         * empty golden file. */
        fprintf(stderr, "asmtrace_record: emulator producer failed for %s\n",
                out);
        asmtest_valtrace_free(vt);
        return -1;
    }
    g = asmtest_defuse_build(vt);

    snprintf(path, sizeof path, "%s/%s.asmtrace", dir, out);
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
        int o = snprintf(text, sizeof text, "%s(", label);
        for (int i = 0; i < nargs; i++)
            o += snprintf(text + o, sizeof text - (size_t)o, "%s%ld",
                          i ? ", " : "", args[i]);
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
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
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
            emu_disas(EMU_ARCH_X86_64, code, code_len, REC_CODE_BASE,
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
        int trunc = vt->truncated;
        asmtest_defuse_free(g);
        asmtest_valtrace_free(vt);
        printf("  %-20s %zu steps, %zu records, %zu def-use edges%s\n", out,
               nsteps, nrecs, nedges, trunc ? "  [TRUNCATED]" : "");
    }
    return 0;
}

/* Record one CORPUS routine (host-arch assembly, captured as a fixed window). */
static int record_one(const char *dir, const rec_routine_t *r) {
    uint8_t code[REC_WINDOW];
    const void *fn = asmtest_corpus_routine(r->name);
    if (!fn) {
        fprintf(stderr, "asmtrace_record: no corpus routine '%s'\n", r->name);
        return -1;
    }
    memcpy(code, fn, sizeof code);
    return record_bytes(dir, r->name, r->name, code, sizeof code, r->args,
                        r->nargs, 65536);
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

    /* The Loom's four walkthrough-grade fixtures (05-loom-day-one.md T7). */
    {
        static const long df_args[2] = {7, 5};
        static const long fork_args[1] = {3};
        static const long rbx_args[2] = {2, 3};
        uint8_t rbx[REC_WINDOW];
        const void *fn;

        if (record_bytes(dir, "loom-df-chain", "df_chain", LOOM_DF_CHAIN,
                         sizeof LOOM_DF_CHAIN, df_args, 2, 65536) != 0)
            failed++;
        /* The D7 dishonesty fixture: a four-record operand buffer fills mid-run,
         * the producer flips `truncated`, and the footer declares it. Generated,
         * never hand-edited — a fixture nobody can produce is a fixture nobody
         * can trust. */
        if (record_bytes(dir, "loom-truncated", "df_chain (recs_cap = 4)",
                         LOOM_DF_CHAIN, sizeof LOOM_DF_CHAIN, df_args, 2,
                         4) != 0)
            failed++;
        if (record_bytes(dir, "loom-fork-demo", "fork_demo", LOOM_FORK_DEMO,
                         sizeof LOOM_FORK_DEMO, fork_args, 1, 65536) != 0)
            failed++;
        /* Walkthrough #2: a callee-save spill (a stack band), an EFLAGS knot and
         * a restore — examples/flags.s:45. HOST-ROUTINE bytes, so this entry is
         * exactly as x86-64-gated as the corpus loop above. */
        fn = asmtest_corpus_routine("sum_via_rbx");
        if (!fn) {
            fprintf(stderr, "asmtrace_record: no corpus routine "
                            "'sum_via_rbx'\n");
            failed++;
        } else {
            memcpy(rbx, fn, sizeof rbx);
            if (record_bytes(dir, "loom-sum-via-rbx", "sum_via_rbx", rbx,
                             sizeof rbx, rbx_args, 2, 65536) != 0)
                failed++;
        }
    }
    if (failed) {
        fprintf(stderr, "asmtrace_record: %d routine(s) failed\n", failed);
        return 1;
    }
    return 0;
}
