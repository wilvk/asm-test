/*
 * sde_mix_report.c — fold an Intel SDE `-mix` histogram into the repo's canonical
 * asmtest_trace_t report shape.
 *
 * SDE's mix tool (`sde64 -mix -omix <file>`) writes a process-wide dynamic-
 * instruction histogram: a global `# $dynamic-counts` block of per-opcode counts,
 * then per-ISA-extension / per-ISA-set / per-category summary lines and a
 * `*total`. This tool parses that GLOBAL block's `*total` into
 * asmtest_trace_t.insns_total and prints it via asmtest_trace_report() — the
 * shared-sink shape every trace backend reports in — then echoes the `*isa-set-*`
 * breakdown verbatim (the per-extension histogram is the part nothing else in the
 * tree can produce).
 *
 * The global block is emitted first, so its `*isa-set-*` lines and `*total` are
 * everything up to and including the FIRST `*total` in the file (per-function
 * blocks, each with their own `*total`, follow). Text is parsed; the kit also
 * offers `-mix-format json`, but the text layout is trivial here.
 *
 * HONESTY LIMIT: `-mix` counts are PROCESS-WIDE dynamic totals, not region-scoped
 * offsets, so insns/blocks offset arrays stay empty and blocks_total stays 0 —
 * the total is dominated by loader/libc startup. Scoping mix to a marker region
 * is future work for the Pin tracing tier, not this lane.
 *
 * DOC CORRECTION (empirical, SDE 10.8.0): the mix output carries NO `*isa-set-APX`
 * line for this lane's fixtures. SDE's XED classifies the REX2/NDD-promoted legacy
 * MOV/ADD/LEA that reach r16-r31 as their BASE isa-set (I86/I386), not a distinct
 * APX set — matching the pin-xed-trace-tier sibling's XED finding (a REX2-promoted
 * MOV is iclass MOV, ext BASE). The proof that APX executed under emulation is T4
 * (bare `# SKIP` vs `-future ok`), not an APX mix line. What the ISA-set histogram
 * DOES prove under `-future` is future-chip emulation process-wide: on an AVX2-only
 * host it still shows AVX512F/AVX512BW sets (the emulated future chip enabled
 * AVX-512 in libc's ifunc-selected routines), which the bare host could not run.
 *
 * Usage: sde_mix_report <mix-file>
 */
#include "asmtest_trace.h"

#include <stdio.h>
#include <string.h>

#define MAX_ISASET  256
#define ISASET_LINE 256

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <sde-mix-file>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        fprintf(stderr, "sde_mix_report: cannot open mix file '%s'\n", path);
        return 1;
    }

    char line[512];
    unsigned long long total = 0;
    int have_total = 0;
    char isaset[MAX_ISASET][ISASET_LINE];
    int n_isaset = 0;
    while (fgets(line, sizeof line, f) != NULL) {
        if (strncmp(line, "*isa-set-", 9) == 0) {
            if (n_isaset < MAX_ISASET) {
                size_t len = strcspn(line, "\r\n"); /* drop the newline */
                if (len >= ISASET_LINE)
                    len = ISASET_LINE - 1;
                memcpy(isaset[n_isaset], line, len);
                isaset[n_isaset][len] = '\0';
                n_isaset++;
            }
        } else if (strncmp(line, "*total", 6) == 0) {
            if (sscanf(line, "*total %llu", &total) == 1) {
                have_total = 1;
                break; /* first *total is the global one; stop here */
            }
        }
    }
    fclose(f);

    if (!have_total) {
        fprintf(stderr,
                "sde_mix_report: no '*total' dynamic-count found in '%s' "
                "(is it an SDE -mix output?)\n",
                path);
        return 1;
    }

    /* Fold the process-wide dynamic total into the canonical trace-report shape. */
    asmtest_trace_t t = {0};
    t.insns_total = total;
    asmtest_trace_report(&t, stdout);

    /* The per-ISA-set histogram — the part nothing else in the tree produces. */
    printf("sde-mix ISA-set breakdown (process-wide dynamic, %d sets):\n",
           n_isaset);
    for (int i = 0; i < n_isaset; i++)
        printf("  %s\n", isaset[i]);
    return 0;
}
