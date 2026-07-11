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
 * `blocks` companion field; consumed by scripts/bench-report.sh into the report.
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

/* Run one case traced; fill insns/blocks/complete. Returns 1 on success. */
static int run_case(const emu_case_t *c, unsigned long long *insns,
                    unsigned long long *blocks, int *complete) {
    asmtest_trace_t *t =
        asmtest_trace_new(0, 256); /* count-only insns + blocks */
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
            int complete = 1;
            if (!run_case(&CASES[i], &insns, &blocks, &complete)) {
                fprintf(stderr, "emu-bench: case %s (%s) failed to run\n",
                        CASES[i].name, CASES[i].arch);
                return 1;
            }
            printf("    {\"name\": \"%s\", \"arch\": \"%s\", \"kind\": "
                   "\"insns\", \"value\": %llu, \"unit\": \"insn\", "
                   "\"deterministic\": true, \"complete\": %s, \"blocks\": "
                   "%llu}%s\n",
                   CASES[i].name, CASES[i].arch, insns,
                   complete ? "true" : "false", blocks, (i + 1 < n) ? "," : "");
        }
        printf("  ]\n}\n");
    } else {
        printf("# emu-bench — deterministic instructions per call "
               "(host-independent)\n");
        printf("  %-18s %-8s %8s %8s  %s\n", "suite.name", "arch", "insns",
               "blocks", "complete");
        for (size_t i = 0; i < n; i++) {
            unsigned long long insns = 0, blocks = 0;
            int complete = 1;
            if (!run_case(&CASES[i], &insns, &blocks, &complete)) {
                fprintf(stderr, "emu-bench: case %s (%s) failed to run\n",
                        CASES[i].name, CASES[i].arch);
                return 1;
            }
            printf("  %-18s %-8s %8llu %8llu  %s\n", CASES[i].name,
                   CASES[i].arch, insns, blocks, complete ? "yes" : "no");
        }
    }
    return 0;
}
