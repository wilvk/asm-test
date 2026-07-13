/*
 * dr_valtrace_bench.c — direction-of-travel microbenchmark for the data-flow DR
 * value producers (taint-tier Increment 3): times one whole
 * asmtest_dataflow_dr_run over a LOOPING fixture under whichever value client
 * ASMTEST_DRVAL_CLIENT names, and prints "<ns> <steps>" on stdout. The
 * mk/native-trace.mk `dr-valtrace-bench` lane runs this per client (clean-call
 * vs inlined drmgr/drreg/drx_buf) across fresh processes and reports the
 * per-instruction capture-cost delta — the "measurable per-instruction cost
 * drop" Increment 3's exit criterion asks for (not the 10-50x taint claim; that
 * is Increment 9).
 *
 * DR init + the app-side replay (build_valtrace) are identical for both clients,
 * so the DELTA between the two whole-run timings is the per-instruction capture
 * (+ inlined-flush) cost — the only asymmetric term.
 *
 * The loop fixture doubles as a CORRECTNESS stress the 6-instruction oracle
 * fixture (df_chain) does not exercise: a back-edge / conditional branch, flag
 * dependence (dec -> jnz), and — for the inlined client — many drx_buf flushes
 * (the tiny fixture never fills the buffer). It asserts the routine's return
 * value so a miscapture that corrupts app state (e.g. clobbered flags) fails
 * loudly rather than just skewing the timing.
 *
 * Self-skips (exit 0) when DynamoRIO / the value client is unavailable.
 */
#include "asmtest_valtrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* The DR value producer ships no header; re-declare its entry points (as dr_valtrace.c does). */
#define DF_DR_OK 0
int asmtest_dataflow_dr_available(void);
int asmtest_dataflow_dr_run(const uint8_t *code, size_t code_len, const long *args,
                            int nargs, uint64_t max_insns, long *result,
                            asmtest_valtrace_t *vt);

/* Leaf loop (x86-64 SysV, rdi = iteration count):
 *   xor eax,eax / L: add rax,rdi / dec rdi / jnz L / ret
 * Returns rdi + (rdi-1) + ... + 1 = rdi*(rdi+1)/2. Executes 3*iters+2 in-region
 * dynamic instructions (a back-edge, a flag-dependent branch). */
static const uint8_t loop_fixture[] = {
    0x31, 0xC0,             /* 0x00 xor eax, eax       */
    0x48, 0x01, 0xF8,       /* 0x02 add rax, rdi       */
    0x48, 0xFF, 0xCF,       /* 0x05 dec rdi            */
    0x75, 0xF8,             /* 0x08 jnz 0x02           */
    0xC3,                   /* 0x0a ret                */
};

int main(int argc, char **argv) {
    if (!asmtest_dataflow_dr_available()) {
        printf("0 0\n"); /* stdout contract kept; the lane treats 0 steps as SKIP */
        fprintf(stderr, "# SKIP dr-valtrace-bench: DynamoRIO / value client unavailable\n");
        return 0;
    }
    long iters = argc > 1 ? atol(argv[1]) : 30000;
    if (iters < 1)
        iters = 1;
    long expected_steps = 3 * iters + 2;
    long expected_result = iters * (iters + 1) / 2;
    uint64_t cap = (uint64_t)expected_steps + 16; /* size at_drval_t so capture never truncates (fair) */

    /* A small replay sink: build_valtrace still iterates every captured step
     * (symmetric cost) but drops most appends — we only need the timing + the
     * routine's return value here, not the reconstructed trace. */
    asmtest_valtrace_t *v = asmtest_valtrace_new(1024, 1024, 0);
    if (v == NULL) {
        fprintf(stderr, "# dr-valtrace-bench: valtrace_new failed\n");
        return 1;
    }

    long args[1] = { iters };
    long result = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = asmtest_dataflow_dr_run(loop_fixture, sizeof loop_fixture, args, 1, cap,
                                     &result, v);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
                  (uint64_t)(t1.tv_nsec - t0.tv_nsec);

    if (rc != DF_DR_OK) {
        fprintf(stderr, "# dr-valtrace-bench: run failed rc=%d\n", rc);
        asmtest_valtrace_free(v);
        return 1;
    }
    if (result != expected_result) {
        fprintf(stderr,
                "# dr-valtrace-bench: WRONG RESULT %ld (expected %ld) — capture "
                "corrupted app state (flags/registers)\n",
                result, expected_result);
        asmtest_valtrace_free(v);
        return 1;
    }

    fprintf(stderr, "# bench: iters=%ld steps=%ld result=%ld ns=%llu\n", iters,
            expected_steps, result, (unsigned long long)ns);
    printf("%llu %ld\n", (unsigned long long)ns, expected_steps);
    asmtest_valtrace_free(v);
    return 0;
}
