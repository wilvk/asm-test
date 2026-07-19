/*
 * emu_bench.c — the EMU_BENCH performance producer (Phase 2).
 *
 * Runs each cross-ISA fixture once inside the Unicorn emulator with tracing on,
 * and reports the DETERMINISTIC work metrics — dynamic instruction count
 * (asmtest_trace_t.insns_total) and distinct basic blocks — per (routine, guest
 * ISA). Unlike the native BENCH tier (real cycles, host arch only), these
 * numbers are a function of the guest code + inputs, so they are identical on
 * any host and directly comparable across x86-64, AArch64, RISC-V, ARM32, and
 * the Win64 ABI. Deterministic ⇒ no calibration and a single run is the answer.
 *
 *   ./emu-bench                 human-readable table
 *   ./emu-bench --format=json   one asmtest_bench_result_t-shaped row per case
 *
 * Emits `insn` rows (kind=insns, deterministic=true, complete=!truncated) with a
 * `blocks` companion field and, when built with Capstone, a sibling `model_cost`
 * row per case (kind=model_cost, unit=model-cyc) carrying the Capstone-weighted
 * BM_MODEL_COST proxy — an honest cross-ISA cost model, not silicon cycles;
 * consumed by scripts/bench-report.sh into the report.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asmbench_fixtures.h"
#include "asmtest_emu.h"
#include "asmtest_trace.h"

/* Which guest engine a case runs on. */
typedef enum { G_X86, G_WIN64, G_ARM64, G_RISCV, G_ARM32 } guest_t;

typedef struct {
    const char *name; /* suite.name */
    const char *arch; /* reported ISA label */
    guest_t guest;
    const unsigned char *code;
    size_t len;
    long args[6];
    int nargs;
} emu_case_t;

/* add3 across every guest ISA (same algorithm) exposes the cross-ISA count
 * difference; sum_to_n scales the count with the trip count. */
static const emu_case_t CASES[] = {
    {"math.add3",
     "x86_64",
     G_X86,
     FIX_X86_ADD3,
     sizeof FIX_X86_ADD3,
     {2, 3, 4},
     3},
    {"math.add3",
     "win64",
     G_WIN64,
     FIX_WIN64_ADD3,
     sizeof FIX_WIN64_ADD3,
     {2, 3, 4},
     3},
    {"math.add3",
     "arm64",
     G_ARM64,
     FIX_A64_ADD3,
     sizeof FIX_A64_ADD3,
     {2, 3, 4},
     3},
    {"math.add3",
     "riscv64",
     G_RISCV,
     FIX_RV_ADD3,
     sizeof FIX_RV_ADD3,
     {2, 3, 4},
     3},
    {"math.add3",
     "arm32",
     G_ARM32,
     FIX_A32_ADD3,
     sizeof FIX_A32_ADD3,
     {2, 3, 4},
     3},
    {"loop.sum_to_10",
     "x86_64",
     G_X86,
     FIX_X86_SUMTON,
     sizeof FIX_X86_SUMTON,
     {10},
     1},
    {"loop.sum_to_100",
     "x86_64",
     G_X86,
     FIX_X86_SUMTON,
     sizeof FIX_X86_SUMTON,
     {100},
     1},
};

/* THE MODEL — a cost proxy comparable across ISAs BY CONSTRUCTION, not silicon.
 * Weight each executed instruction by its class and sum. Keep in sync with
 * docs/guides/cross-system-benchmarking.md. */
static const double MODEL_W[] = {
    [ASMTEST_INSN_OTHER] = 1.0,
    [ASMTEST_INSN_MEM] = 3.0,
    [ASMTEST_INSN_BRANCH] = 2.0,
    [ASMTEST_INSN_MULDIV] = 8.0,
};

/* Map a bench guest to the disassembler's guest ISA (win64 shares x86-64 bytes). */
static asmtest_arch_t guest_disas_arch(guest_t g) {
    switch (g) {
    case G_X86:
    case G_WIN64:
        return ASMTEST_ARCH_X86_64;
    case G_ARM64:
        return ASMTEST_ARCH_ARM64;
    case G_RISCV:
        return ASMTEST_ARCH_RISCV64;
    case G_ARM32:
        return ASMTEST_ARCH_ARM32;
    }
    return ASMTEST_ARCH_X86_64;
}

/* Sum MODEL_W over the RECORDED instruction offsets (the truncated prefix if the
 * trace overflowed), classifying each distinct byte offset once — loop bodies
 * repeat, so a tiny per-offset memo avoids re-decoding. Requires Capstone; the
 * caller gates on asmtest_disas_available(). */
static double model_cost(const emu_case_t *c, const asmtest_trace_t *t) {
    asmtest_arch_t arch = guest_disas_arch(c->guest);
    /* Offsets index into the fixture bytes; the largest fixture (FIX_X86_TRI) is
     * 20 bytes, so 64 slots memoize every offset any fixture can produce. */
    signed char memo[64];
    memset(memo, -1, sizeof memo);
    unsigned long long n = asmtest_emu_trace_insns_len(t);
    double sum = 0.0;
    for (unsigned long long i = 0; i < n; i++) {
        uint64_t off = asmtest_emu_trace_insn_at(t, i);
        asmtest_insn_class_t cls;
        if (off < sizeof memo && memo[off] >= 0) {
            cls = (asmtest_insn_class_t)memo[off];
        } else {
            cls = asmtest_disas_class(arch, c->code, c->len, off);
            if (off < sizeof memo)
                memo[off] = (signed char)cls;
        }
        sum += MODEL_W[cls];
    }
    return sum;
}

/* Run one case traced; fill insns/blocks/complete and, when Capstone is linked,
 * the weighted model cost (*have_model set, *mcost summed). Returns 1 on success.
 * The trace records per-instruction offsets (cap 4096 >> the 302 insns the
 * largest fixture loop.sum_to_100 retires) so model_cost can classify each. */
static int run_case(const emu_case_t *c, unsigned long long *insns,
                    unsigned long long *blocks, int *complete, double *mcost,
                    int *have_model) {
    asmtest_trace_t *t =
        asmtest_trace_new(4096, 256); /* record insn offsets + count blocks */
    if (!t)
        return 0;
    int ok = 0;
    switch (c->guest) {
    case G_X86: {
        emu_t *e = emu_open();
        emu_result_t r;
        if (e &&
            emu_call_traced(e, c->code, c->len, c->args, c->nargs, 0, &r, t))
            ok = 1;
        if (e)
            emu_close(e);
        break;
    }
    case G_WIN64: {
        emu_t *e = emu_open();
        emu_result_t r;
        if (e && emu_call_win64_traced(e, c->code, c->len, c->args, c->nargs, 0,
                                       &r, t))
            ok = 1;
        if (e)
            emu_close(e);
        break;
    }
    case G_ARM64: {
        emu_arm64_t *e = emu_arm64_open();
        emu_arm64_result_t r;
        if (e && emu_arm64_call_traced(e, c->code, c->len, c->args, c->nargs, 0,
                                       &r, t))
            ok = 1;
        if (e)
            emu_arm64_close(e);
        break;
    }
    case G_RISCV: {
        emu_riscv_t *e = emu_riscv_open();
        emu_riscv_result_t r;
        if (e && emu_riscv_call_traced(e, c->code, c->len, c->args, c->nargs, 0,
                                       &r, t))
            ok = 1;
        if (e)
            emu_riscv_close(e);
        break;
    }
    case G_ARM32: {
        emu_arm_t *e = emu_arm_open();
        emu_arm_result_t r;
        if (e && emu_arm_call_traced(e, c->code, c->len, c->args, c->nargs, 0,
                                     &r, t))
            ok = 1;
        if (e)
            emu_arm_close(e);
        break;
    }
    }
    *insns = asmtest_emu_trace_insns_total(t);
    *blocks = asmtest_emu_trace_blocks_len(t);
    *complete = !asmtest_emu_trace_truncated(t);
    *have_model = asmtest_disas_available() && ok;
    *mcost = *have_model ? model_cost(c, t) : 0.0;
    asmtest_trace_free(t);
    return ok;
}

int main(int argc, char **argv) {
    int json = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--format=json") == 0)
            json = 1;
        else if (strcmp(argv[i], "--format=text") == 0)
            json = 0;

    size_t n = sizeof CASES / sizeof CASES[0];
    if (json) {
        printf("{\"unit\": \"insn\", \"deterministic\": true, "
               "\"benchmarks\": [\n");
        for (size_t i = 0; i < n; i++) {
            unsigned long long insns = 0, blocks = 0;
            int complete = 1, have_model = 0;
            double mcost = 0.0;
            if (!run_case(&CASES[i], &insns, &blocks, &complete, &mcost,
                          &have_model)) {
                fprintf(stderr, "emu-bench: case %s (%s) failed to run\n",
                        CASES[i].name, CASES[i].arch);
                return 1;
            }
            if (i > 0)
                printf(",\n"); /* separator before each case's insns row */
            printf("    {\"name\": \"%s\", \"arch\": \"%s\", \"kind\": "
                   "\"insns\", \"value\": %llu, \"unit\": \"insn\", "
                   "\"deterministic\": true, \"complete\": %s, \"blocks\": "
                   "%llu}",
                   CASES[i].name, CASES[i].arch, insns,
                   complete ? "true" : "false", blocks);
            /* Sibling model-cost row, only where Capstone classified the stream. */
            if (have_model)
                printf(
                    ",\n    {\"name\": \"%s\", \"arch\": \"%s\", \"kind\": "
                    "\"model_cost\", \"value\": %g, \"unit\": \"model-cyc\", "
                    "\"deterministic\": true, \"complete\": %s}",
                    CASES[i].name, CASES[i].arch, mcost,
                    complete ? "true" : "false");
        }
        printf("\n  ]\n}\n");
    } else {
        printf("# emu-bench — deterministic instructions per call "
               "(host-independent); model-cyc is a MODEL, not silicon\n");
        printf("  %-18s %-8s %8s %8s %10s  %s\n", "suite.name", "arch", "insns",
               "blocks", "model-cyc", "complete");
        for (size_t i = 0; i < n; i++) {
            unsigned long long insns = 0, blocks = 0;
            int complete = 1, have_model = 0;
            double mcost = 0.0;
            if (!run_case(&CASES[i], &insns, &blocks, &complete, &mcost,
                          &have_model)) {
                fprintf(stderr, "emu-bench: case %s (%s) failed to run\n",
                        CASES[i].name, CASES[i].arch);
                return 1;
            }
            if (have_model)
                printf("  %-18s %-8s %8llu %8llu %10g  %s\n", CASES[i].name,
                       CASES[i].arch, insns, blocks, mcost,
                       complete ? "yes" : "no");
            else
                printf("  %-18s %-8s %8llu %8llu %10s  %s\n", CASES[i].name,
                       CASES[i].arch, insns, blocks, "-",
                       complete ? "yes" : "no");
        }
    }
    return 0;
}
