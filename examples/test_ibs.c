/*
 * test_ibs.c — AMD IBS-Op statistical tracing lane (asmtest_ibs.h), Phases 0-1.
 *
 * Two halves, matching the tier's design:
 *
 *   1. The PURE decoder (asmtest_ibs_decode_op) is validated with SYNTHETIC raw
 *      IBS-Op records — no hardware — so these checks run and pass on EVERY CI host
 *      (AMD or not, x86-64 or not), exactly like test_amd_reconstruction feeds the
 *      AMD decoder a synthetic branch stack.
 *
 *   2. The LIVE out-of-band capture (asmtest_ibs_survey_pid) self-profiles a known
 *      hot loop running on a SEPARATE worker thread and asserts its back-edge shows
 *      up in the survey. Guarded by asmtest_ibs_available(): on any host without
 *      IBS-Op (non-AMD, VM, CI, Zen without BrnTrgt) it prints `# SKIP` and exits 0.
 */
#include "asmtest_ibs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* IbsOpData (reg[2]) resolution bits, mirroring the backend's decoder. */
#define D_RETURN (1ull << 34)
#define D_TAKEN  (1ull << 35)
#define D_MISP   (1ull << 36)
#define D_RET    (1ull << 37)
#define D_RIPINV (1ull << 38)

/* Build a synthetic IBS-Op PERF_SAMPLE_RAW payload: [u32 caps][u64 reg0..reg7].
 * reg[1]=IbsOpRip (from), reg[2]=IbsOpData (bits), reg[7]=IbsBrTarget (to). */
#define RAW_LEN (4u + 8u * 8u) /* 68 */
static void build_raw(uint8_t *buf, uint64_t rip, uint64_t data2,
                      uint64_t tgt) {
    memset(buf, 0, RAW_LEN);
    uint32_t caps = 0x3ff;
    memcpy(buf + 0, &caps, 4);
    memcpy(buf + 4 + 8 * 1, &rip, 8);
    memcpy(buf + 4 + 8 * 2, &data2, 8);
    memcpy(buf + 4 + 8 * 7, &tgt, 8);
}

/* Host-independent: the pure decoder over synthetic records. */
static void test_decode(void) {
    uint8_t raw[RAW_LEN];
    asmtest_ibs_edge_t e;

    /* 1. A retired taken branch is a usable edge {from -> to}. */
    build_raw(raw, 0x401000, D_RET | D_TAKEN, 0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_OK,
          "decode: retired taken branch -> OK");
    CHECK(e.from == 0x401000 && e.to == 0x401040 && e.taken == 1 &&
              e.mispred == 0 && e.is_return == 0 && e.count == 1,
          "decode: taken branch yields the exact from->to edge");

    /* 2. A retired but NOT-taken conditional has no target: no edge. */
    build_raw(raw, 0x401000, D_RET, 0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_NOEDGE,
          "decode: not-taken branch -> NOEDGE");

    /* 3. A non-branch op (BrnRet clear) is no edge. */
    build_raw(raw, 0x401000, 0, 0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_NOEDGE,
          "decode: non-branch op -> NOEDGE");

    /* 4. A taken branch with an INVALID RIP is dropped (RipInvalidChk). */
    build_raw(raw, 0x401000, D_RET | D_TAKEN | D_RIPINV, 0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_NOEDGE,
          "decode: RIP-invalid taken branch -> NOEDGE");

    /* 5. A mispredicted taken RETURN carries both flags through. */
    build_raw(raw, 0x4010f0, D_RET | D_TAKEN | D_MISP | D_RETURN, 0x401200);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_OK &&
              e.mispred == 1 && e.is_return == 1,
          "decode: mispredicted return -> OK with mispred + is_return");

    /* 6. A record too short to hold the branch-target register is undecodable. */
    build_raw(raw, 0x401000, D_RET | D_TAKEN, 0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN - 8, &e) == ASMTEST_IBS_EDECODE,
          "decode: short record (no BrnTgt reg) -> EDECODE");

    /* 7. NULL arguments are rejected. */
    CHECK(asmtest_ibs_decode_op(NULL, RAW_LEN, &e) == ASMTEST_IBS_EINVAL,
          "decode: NULL raw -> EINVAL");
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, NULL) == ASMTEST_IBS_EINVAL,
          "decode: NULL out -> EINVAL");
}

/* The availability probe must be definite and stable (cached). */
static void test_available(void) {
    int a = asmtest_ibs_available();
    int b = asmtest_ibs_available();
    CHECK(a == 0 || a == 1, "available() returns a definite 0/1");
    CHECK(a == b, "available() is stable across calls");
    const char *why = asmtest_ibs_skip_reason();
    CHECK(why != NULL, "skip_reason() is never NULL");
    if (a)
        CHECK(why[0] == '\0', "skip_reason() is empty when available");
    else
        CHECK(why[0] != '\0', "skip_reason() names a reason when unavailable");
    printf("# IBS-Op on this host: %s%s%s\n", a ? "AVAILABLE" : "unavailable",
           a ? "" : " — ", a ? "" : why);
}

/* ---- live capture (Linux/x86-64/AMD with IBS-Op; self-skips otherwise) -------- */
#if defined(__linux__) && defined(__x86_64__)
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile int g_stop;
static volatile int g_tid_ready;
static pid_t g_worker_tid;
static long g_sink;

/* A distinct, small hot loop: its conditional back-edge is a taken branch IBS-Op
 * tags on nearly every iteration, so it dominates the survey. noinline keeps it a
 * real function whose address bounds the expected edge endpoints. */
static long __attribute__((noinline)) spin_loop(void) {
    long s = 0;
    for (long i = 0; !g_stop; i++) {
        if (i & 1)
            s += i;
        else
            s -= i;
    }
    return s;
}
static void *worker(void *arg) {
    (void)arg;
    g_worker_tid = (pid_t)syscall(SYS_gettid);
    __sync_synchronize();
    g_tid_ready = 1;
    g_sink = spin_loop();
    return NULL;
}

static void test_live(void) {
    if (!asmtest_ibs_available()) {
        printf("# SKIP IBS live capture: %s\n", asmtest_ibs_skip_reason());
        return;
    }
    g_stop = 0;
    g_tid_ready = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) != 0) {
        printf("# SKIP IBS live capture: pthread_create failed\n");
        return;
    }
    /* Wait for the worker to publish its tid and start spinning. */
    while (!g_tid_ready) {
        struct timespec s = {0, 1000 * 1000};
        nanosleep(&s, NULL);
    }

    /* Survey the WORKER's thread out of band from THIS thread — proving the
     * capture observes another running thread without single-stepping it. */
    asmtest_ibs_survey_t survey;
    int rc = asmtest_ibs_survey_pid(g_worker_tid, 300, NULL, &survey);

    g_stop = 1;
    pthread_join(th, NULL);

    /* available() is a SUBSTRATE probe (AMD + caps + PMU + swfilt); it does not
     * guarantee perf_event_open succeeds. On an AMD IBS host that still blocks perf
     * (paranoid=3, a restrictive seccomp), the substrate is present but the open
     * returns EUNAVAIL — a skip, not a failure. */
    if (rc == ASMTEST_IBS_EUNAVAIL) {
        printf("# SKIP IBS live capture: perf_event_open blocked "
               "(paranoid/seccomp), substrate present\n");
        asmtest_ibs_survey_free(&survey);
        return;
    }

    CHECK(rc == ASMTEST_IBS_OK, "survey_pid: out-of-band capture succeeds");
    CHECK(survey.samples > 0, "survey_pid: sampled the running worker thread");
    CHECK(survey.branch_samples > 0,
          "survey_pid: recorded retired taken-branch edges");
    CHECK(survey.n > 0, "survey_pid: produced at least one aggregated edge");

    /* The spin loop's edges must fall within spin_loop()'s own code window; its
     * hottest edge should be the loop back-edge (target before source). */
    uintptr_t fn = (uintptr_t)(void *)&spin_loop;
    const uintptr_t WIN = 0x2000; /* generous for an -O0 function */
    int in_range = 0, back_edge = 0;
    for (size_t i = 0; i < survey.n; i++) {
        uintptr_t from = (uintptr_t)survey.edges[i].from;
        uintptr_t to = (uintptr_t)survey.edges[i].to;
        if (from >= fn && from < fn + WIN && to >= fn && to < fn + WIN) {
            in_range = 1;
            if (to <= from)
                back_edge = 1;
        }
    }
    CHECK(in_range, "survey_pid: an edge lies within the profiled spin_loop()");
    CHECK(back_edge,
          "survey_pid: the spin loop's taken back-edge was captured");

    if (survey.n > 0) {
        printf("# top edge: %#lx -> %#lx  count=%llu  (%zu edges, %llu/%llu "
               "branch/total samples%s)\n",
               (unsigned long)survey.edges[0].from,
               (unsigned long)survey.edges[0].to,
               (unsigned long long)survey.edges[0].count, survey.n,
               (unsigned long long)survey.branch_samples,
               (unsigned long long)survey.samples,
               survey.throttled ? ", throttled" : "");
    }
    asmtest_ibs_survey_free(&survey);
    CHECK(survey.edges == NULL && survey.n == 0,
          "survey_free: releases and zeroes the survey");
}
#else
static void test_live(void) {
    printf("# SKIP IBS live capture: not Linux x86-64\n");
}
#endif

int main(void) {
    test_decode();
    test_available();
    test_live();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
