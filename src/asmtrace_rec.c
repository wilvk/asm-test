/*
 * asmtrace_rec.c — record-mode producer glue (asmtest_rec.h).
 *
 * One job: turn an emulator run that a test just did into a `.asmtrace` file at
 * the path the runner armed, and tell the runner it exists. Everything about
 * the format lives in the shared writer TU, so this file owns no field order.
 *
 * Contract: docs/internal/gui/asmtrace-schema.md.
 */
#include "asmtest_rec.h"

#include <stdio.h>
#include <string.h>

#include "asmtest.h"         /* asmtest_record_path / asmtest_note_recording */
#include "asmtrace_ndjson.h" /* the shared writer (field order lives there)  */

int asmtest_rec_emu(const emu_trace_t *tr, const emu_result_t *res,
                    const void *code, size_t code_len) {
    const char *path = asmtest_record_path();
    if (path == NULL)
        return 0; /* not armed, or no test running — the honest no-op */

    /* The emulator tier is an isolated guest replaying bytes: exact about what
     * it executed, and explicitly NOT silicon. `trust` is the tier vocabulary
     * the schema fixes; a reader badges the run from it. */
    asmtrace_prov_t prov = {"emu-l0", 1, "exact", 0, NULL, 0};
    asmtrace_writer_t w;
    char body[8192], dis[160];

    /* NOT deterministic mode: a test recording is a measurement of one run on
     * one machine, and its ts/pid are part of what makes it useful when it is
     * the evidence attached to a CI failure. The golden corpus is the thing
     * that must be byte-stable, and it uses deterministic mode. */
    if (asmtrace_open(&w, path, 0) != 0)
        return -1;
    if (asmtrace_header(&w, "asmtest", &prov, 0, NULL) != 0) {
        asmtrace_close(&w, 0, 0, NULL);
        return -1;
    }

    if (tr != NULL) {
        for (size_t i = 0; i < tr->insns_len; i++) {
            dis[0] = '\0';
            if (code != NULL && emu_disas_available())
                emu_disas(EMU_ARCH_X86_64, code, code_len, EMU_CODE_BASE,
                          tr->insns[i], dis, sizeof dis);
            if (dis[0]) {
                asmtrace_escape(body, sizeof body, dis);
                asmtrace_emitf(
                    &w, "trace",
                    "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu"
                    ",\"disasm\":\"%s\"",
                    (unsigned long long)tr->insns[i], body);
            } else {
                asmtrace_emitf(
                    &w, "trace",
                    "\"basis\":\"rel\",\"kind\":\"insn\",\"off\":%llu",
                    (unsigned long long)tr->insns[i]);
            }
        }
        if (tr->blocks_len > 0 || tr->blocks_total > 0) {
            /* The emulator DOES measure basic blocks (its block hook fires on
             * every one), so unlike the L0 value producer it can write a
             * `coverage` event honestly — including the totals that make
             * truncation legible. */
            int n =
                snprintf(body, sizeof body, "\"basis\":\"rel\",\"blocks\":[");
            for (size_t i = 0;
                 i < tr->blocks_len && n > 0 && (size_t)n < sizeof body; i++)
                n += snprintf(body + n, sizeof body - (size_t)n, "%s%llu",
                              i ? "," : "", (unsigned long long)tr->blocks[i]);
            if (n > 0 && (size_t)n < sizeof body)
                snprintf(body + n, sizeof body - (size_t)n,
                         "],\"blocks_total\":%llu,\"insns_total\":%llu,"
                         "\"truncated\":%s",
                         (unsigned long long)tr->blocks_total,
                         (unsigned long long)tr->insns_total,
                         tr->truncated ? "true" : "false");
            asmtrace_emit(&w, "coverage", body);
        }
        if (tr->truncated)
            w.truncated = 1;
    }

    if (res != NULL) {
        /* A fault is DATA, not an error: the recording says what happened and
         * a viewer renders a fault card from it. */
        const char *kind = res->fault_kind == EMU_FAULT_READ    ? "read"
                           : res->fault_kind == EMU_FAULT_WRITE ? "write"
                           : res->fault_kind == EMU_FAULT_FETCH ? "fetch"
                                                                : "none";
        asmtrace_emitf(&w, "result",
                       "\"tier\":\"emu\",\"backend\":\"unicorn\","
                       "\"arch\":\"x86_64\",\"scope\":\"routine\","
                       "\"available\":true,\"fidelity\":\"virtual\","
                       "\"ok\":%s,\"faulted\":%s,\"fault_kind\":\"%s\","
                       "\"fault_addr\":%llu",
                       res->ok ? "true" : "false",
                       res->faulted ? "true" : "false", kind,
                       (unsigned long long)res->fault_addr);
    }

    if (asmtrace_close(&w, 0, 0, NULL) != 0)
        return -1;

    /* Only now — a recording nobody could write must not be named in a failure
     * report. Step id stays -1 in v1: "this test wrote this file", not "this
     * step is to blame". */
    asmtest_note_recording(path, -1);
    return 1;
}
