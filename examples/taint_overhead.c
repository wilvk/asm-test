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
 * A 4th arg `simd` (Increment 8) times an AVX2 (YMM) loop instead of the scalar one, so the SIMD
 * taint overhead delta is reported SEPARATELY from the scalar band (the per-YMM-op 32-lane traffic
 * the granularity note flags). Prints one grep-able line:
 * `OVH mode=<m> kind=<scalar|simd> hotns=<ns> iters=<N> r=<checksum>`.
 *
 * The lane (dr-taint-overhead-test) asserts the monotonic STRUCTURAL facts (T_taint > T_dr >=
 * T_bare) as noise-tolerant checks AND — the Increment-9 exit criterion — HARD-GATES the band:
 * the build FAILS if the record-free PRODUCTION overhead regresses past the ~10-50x budget
 * (prod-taint > BAND_MAX x bare). The gate sits well above the measured ~11x (headroom for
 * wall-clock noise) so it catches a real regression, not jitter.
 *
 * Self-contained: defines the region + seed marker symbols the client resolves by PC
 * (-rdynamic), same ABI as taint_workload.c.
 */
#include "asmtest_taint.h"

#include "dataflow_dr.h"

#include <immintrin.h> /* AVX2 intrinsics for the SIMD overhead variant (Increment 8) */
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

/* Increment 8 SIMD overhead variant: the same shape as hot_loop but with a 32-byte AVX (YMM)
 * load + 256-bit vector arithmetic, so the timed cost is the SIMD taint path — per-YMM-op the
 * client emits 32 per-byte lane ops (union/broadcast) + a 32-byte memory shadow access, the
 * traffic the granularity note flags. target("avx2") compiles just this function with AVX2, no
 * global -mavx2. noinline so the region marker can scope it. */
__attribute__((noinline, target("avx2"))) static uint64_t
simd_hot_loop(uint64_t n) {
    __m256i acc = _mm256_set1_epi64x(1);
    for (uint64_t i = 0; i < n; i++) {
        __m256i v =
            _mm256_loadu_si256((const __m256i *)&g_buf[(i * 4) & (BUFN - 4)]);
        acc = _mm256_add_epi64(_mm256_xor_si256(acc, v),
                               _mm256_set1_epi64x((long long)(i << 3)));
    }
    uint64_t t[4];
    _mm256_storeu_si256((__m256i *)t, acc);
    return t[0] + t[1] + t[2] + t[3];
}

/* Throwaway capture buffer for the value/taint records under `mark` (we time the loop, not
 * the trace — the records overflow honestly and are never read). */
static at_vstep_t g_steps[64];
static at_tag_t g_step_taint[64];
static at_drval_t g_drval;

int main(int argc, char **argv) {
    int mark = (argc > 1 && strcmp(argv[1], "mark") == 0);
    uint64_t n = (argc > 2) ? strtoull(argv[2], NULL, 0) : 20000000ULL;
    /* argv[3] == "simd" times the AVX2 (YMM) loop instead of the scalar one — the SIMD taint
     * overhead delta (Increment 8), reported separately from the scalar band. */
    int simd = (argc > 3 && strcmp(argv[3], "simd") == 0);
    if (simd && !__builtin_cpu_supports("avx2")) {
        /* No AVX2: emit a skip line (hotns=0) so the driver reports it rather than SIGILL. */
        printf("OVH mode=%s kind=simd-skip hotns=0 iters=0 r=0\n",
               mark ? "mark" : "nomark");
        return 0;
    }

    for (int i = 0; i < BUFN; i++)
        g_buf[i] = (uint64_t)i * 2654435761u + 1;

    if (mark) {
        g_drval.steps = g_steps;
        g_drval.steps_cap = sizeof g_steps / sizeof g_steps[0];
        g_drval.step_taint = g_step_taint;
        g_drval.step_taint_cap = sizeof g_step_taint / sizeof g_step_taint[0];
        /* Scope a generous range over the timed loop (nothing else executes there during the
         * timed window) and seed the whole buffer so every load is tainted. */
        asmtest_dr_valcapture_marker(
            simd ? (void *)simd_hot_loop : (void *)hot_loop, 2048, &g_drval);
        asmtest_dr_taint_seed_marker(g_buf, sizeof g_buf, AT_TAG_TAINTED);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t r = simd ? simd_hot_loop(n) : hot_loop(n);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    uint64_t ns = (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                  (uint64_t)(t1.tv_nsec - t0.tv_nsec);

    printf("OVH mode=%s kind=%s hotns=%llu iters=%llu r=%llu\n",
           mark ? "mark" : "nomark", simd ? "simd" : "scalar",
           (unsigned long long)ns, (unsigned long long)n,
           (unsigned long long)r);
    return 0;
}
