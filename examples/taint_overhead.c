/*
 * taint_overhead.c — the DR taint tier's first in-repo OVERHEAD number
 * (dynamorio-taint-tier-plan.md, Increment 9). Every cited slowdown figure in the plan is
 * external literature; this bench measures the tier's own cost on this repo.
 *
 * It times a hot loop of exactly the operations taint tracks — a memory load from a seeded
 * buffer, integer arithmetic (union propagation), and a data-dependent branch (a sink site) —
 * with an INTERNAL CLOCK_MONOTONIC timer around only the loop (so process/DR startup is
 * excluded). The same binary is run three ways so a driver can DECOMPOSE the cost:
 *   ./taint_overhead nomark N            -> bare native, no DR              (T_bare)
 *   drrun -c <client> -- ./taint_overhead nomark N  -> DR code-cache baseline, regions=0  (T_dr)
 *   drrun -c <client> -- ./taint_overhead mark  N   -> + inline taint instrumentation      (T_taint)
 * so DR-baseline = T_dr/T_bare, taint-instrumentation = T_taint/T_dr, whole-tier = T_taint/T_bare.
 * Prints one grep-able line: `OVH mode=<m> hotns=<ns> iters=<N> r=<checksum>`.
 *
 * Deliberately NOT a hard CI threshold: wall-clock ratios are noise-prone (the Increment-3
 * microbench makes the same call). The lane asserts only the monotonic STRUCTURAL fact
 * (T_taint > T_dr >= T_bare) and REPORTS the ratios against the plan's ~10-50x band.
 *
 * Self-contained: defines the region + seed marker symbols the client resolves by PC
 * (-rdynamic), same ABI as taint_workload.c.
 */
#include "asmtest_taint.h"

#include "dataflow_dr.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile unsigned long g_v_sink, g_seed_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_dr_valcapture_marker(void *base, size_t len, void *drval) {
    g_v_sink += 0x77 + (unsigned long)(uintptr_t)base + len +
                (unsigned long)(uintptr_t)drval;
}
__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_seed_marker(void *base, size_t len, unsigned long color) {
    g_seed_sink += 0x91 + (unsigned long)(uintptr_t)base + len + color;
}

#define BUFN 256 /* power of two */
static uint64_t g_buf[BUFN];

/* The hot loop: a seeded memory load + integer arithmetic (the tag-union edges the taint
 * client propagates inline). The BODY is branchless so the only conditional branch is the
 * loop back-edge — which the client currently instruments with an UNCONDITIONAL sink clean
 * call (one per iteration), the cost this bench exposes. noinline so it is a distinct, stable
 * code range the region marker can scope. */
__attribute__((noinline)) static uint64_t hot_loop(uint64_t n) {
    uint64_t acc = 1;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t v = g_buf[i & (BUFN - 1)];
        acc = acc * 6364136223846793005ULL + v + (i << 3);
    }
    return acc;
}

/* Throwaway capture buffer for the value/taint records under `mark` (we time the loop, not
 * the trace — the records overflow honestly and are never read). */
static at_vstep_t g_steps[64];
static at_tag_t g_step_taint[64];
static at_drval_t g_drval;

int main(int argc, char **argv) {
    int mark = (argc > 1 && strcmp(argv[1], "mark") == 0);
    uint64_t n = (argc > 2) ? strtoull(argv[2], NULL, 0) : 20000000ULL;

    for (int i = 0; i < BUFN; i++)
        g_buf[i] = (uint64_t)i * 2654435761u + 1;

    if (mark) {
        g_drval.steps = g_steps;
        g_drval.steps_cap = sizeof g_steps / sizeof g_steps[0];
        g_drval.step_taint = g_step_taint;
        g_drval.step_taint_cap = sizeof g_step_taint / sizeof g_step_taint[0];
        /* Scope a generous range over hot_loop (nothing else executes there during the
         * timed loop) and seed the whole buffer so every load is tainted. */
        asmtest_dr_valcapture_marker((void *)hot_loop, 2048, &g_drval);
        asmtest_dr_taint_seed_marker(g_buf, sizeof g_buf, AT_TAG_TAINTED);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t r = hot_loop(n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                  (uint64_t)(t1.tv_nsec - t0.tv_nsec);

    printf("OVH mode=%s hotns=%llu iters=%llu r=%llu\n",
           mark ? "mark" : "nomark", (unsigned long long)ns,
           (unsigned long long)n, (unsigned long long)r);
    return 0;
}
