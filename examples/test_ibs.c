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

#include "ibs_backend.h" /* internal IBS-Fetch front-end lane (Phase 7) */

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
/* Generalized builder: the record's own `caps` word plus reg[1]/reg[2]/reg[7].
 * `len` allows the 76-byte (9-register) append-order fixture. */
static void build_raw_caps(uint8_t *buf, size_t len, uint32_t caps,
                           uint64_t rip, uint64_t data2, uint64_t tgt) {
    memset(buf, 0, len);
    memcpy(buf + 0, &caps, 4);
    memcpy(buf + 4 + 8 * 1, &rip, 8);
    memcpy(buf + 4 + 8 * 2, &data2, 8);
    memcpy(buf + 4 + 8 * 7, &tgt, 8);
}
/* The self-consistent caps (0x3ff: BrnTrgt + RipInvalidChk + OpData4 all set)
 * the existing eight checks carry — they must pass byte-for-byte unchanged. */
static void build_raw(uint8_t *buf, uint64_t rip, uint64_t data2,
                      uint64_t tgt) {
    build_raw_caps(buf, RAW_LEN, 0x3ff, rip, data2, tgt);
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

    /* 8. 68-byte COLLISION, wrong side: caps 0x7df (bit 5 CLEAR, bit 10 set) is the
     * BRNTRGT=0/OPDATA4=1 shape — byte-identical in length to the branch-target
     * shape, but reg[7] is IbsOpData4, not a target. Length alone would misread
     * 0xdeadbeef as a destination; only the caps word disambiguates. A pre-fix
     * decoder returns OK with e.to == 0xdeadbeef; this must be EDECODE. */
    build_raw_caps(raw, RAW_LEN, 0x7df, 0x401000, D_RET | D_TAKEN, 0xdeadbeef);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_EDECODE,
          "decode: caps BrnTrgt clear (68-byte OpData4 collision) -> EDECODE");
    CHECK(e.to != 0xdeadbeef,
          "decode: the OpData4 value was NOT misread as a branch target");

    /* 9. Append order, RIGHT side: a 76-byte record with caps 0x7ff (bits 5 AND 10).
     * BrTarget is appended FIRST, so reg[7] is the target and reg[8] the OpData4. */
    uint8_t raw76[RAW_LEN + 8];
    build_raw_caps(raw76, sizeof raw76, 0x7ff, 0x401000, D_RET | D_TAKEN,
                   0x401040);
    uint64_t opdata4 = 0xcafef00d;
    memcpy(raw76 + 4 + 8 * 8, &opdata4, 8); /* reg[8] = IbsOpData4 (dummy) */
    CHECK(asmtest_ibs_decode_op(raw76, sizeof raw76, &e) == ASMTEST_IBS_OK &&
              e.to == 0x401040,
          "decode: caps BrnTrgt set (76-byte) -> OK, reg[7] is the target");

    /* 10. RipInvalidChk (cap bit 7) gate: with the cap CLEAR the reserved bit 38
     * must be IGNORED (no spurious drop); with it SET the RIP-invalid drop stands. */
    build_raw_caps(raw, RAW_LEN, 0x37f, 0x401000, D_RET | D_TAKEN | D_RIPINV,
                   0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_OK,
          "decode: RipInvalidChk cap clear -> bit 38 ignored, edge stands");
    build_raw_caps(raw, RAW_LEN, 0x3ff, 0x401000, D_RET | D_TAKEN | D_RIPINV,
                   0x401040);
    CHECK(asmtest_ibs_decode_op(raw, RAW_LEN, &e) == ASMTEST_IBS_NOEDGE,
          "decode: RipInvalidChk cap set -> RIP-invalid drop preserved");
}

/* Host-independent: Phase-6 edge -> basic-block normalization. Every branch TARGET
 * is a block leader (the exact AMD-LBR / native tiers' trace_append_block(to -
 * base_ip) rule); a region lifts the in-region targets to region-relative offsets
 * that line up with the exact blocks[], and duplicate targets merge (entry counts
 * summed). No hardware — runs on every CI host, like test_decode. */
static void test_normalize(void) {
    /* Four sampled edges: two land at the same in-region target (0x1000), one at a
     * second in-region target (0x1080), one at a target OUTSIDE the region (0x3000). */
    asmtest_ibs_edge_t edges[4] = {
        {0x1050, 0x1000, 100, 1, 0, 0}, /* back-edge into the routine entry  */
        {0x1040, 0x1080, 30, 1, 0, 0},  /* forward edge to a second block    */
        {0x1090, 0x1000, 20, 1, 0, 0},  /* another edge into the same entry  */
        {0x1200, 0x3000, 5, 1, 0, 0},   /* edge leaving the routine's region */
    };
    asmtest_ibs_survey_t survey;
    memset(&survey, 0, sizeof survey);
    survey.edges = edges;
    survey.n = 4;

    asmtest_ibs_blocks_t b;

    /* Region [0x1000, 0x1200): in-region targets normalize to offsets 0 and 0x80
     * (the out-of-region 0x3000 target is dropped), and the two edges into the entry
     * merge into one block whose entry count is their sum. */
    CHECK(asmtest_ibs_normalize_blocks(&survey, 0x1000, 0x200, &b) ==
              ASMTEST_IBS_OK,
          "normalize: region survey -> OK");
    CHECK(b.n == 2, "normalize: two distinct in-region block leaders");
    CHECK(b.n == 2 && b.blocks[0].start == 0 && b.blocks[0].entries == 120,
          "normalize: entry-offset block merges both edges (offset 0, 120)");
    CHECK(b.n == 2 && b.blocks[1].start == 0x80 && b.blocks[1].entries == 30,
          "normalize: second block lifts to region-relative offset 0x80");
    asmtest_ibs_blocks_free(&b);
    CHECK(b.blocks == NULL && b.n == 0,
          "normalize: blocks_free zeroes the set");

    /* No region (len 0): every target is kept as an ABSOLUTE address, sorted. */
    CHECK(asmtest_ibs_normalize_blocks(&survey, 0, 0, &b) == ASMTEST_IBS_OK,
          "normalize: no-region survey -> OK");
    CHECK(b.n == 3, "normalize: all three distinct targets kept absolute");
    CHECK(b.n == 3 && b.blocks[0].start == 0x1000 &&
              b.blocks[0].entries == 120 && b.blocks[1].start == 0x1080 &&
              b.blocks[2].start == 0x3000,
          "normalize: absolute block starts sorted ascending");
    asmtest_ibs_blocks_free(&b);

    /* An empty survey normalizes to a valid, empty block set (not an error). */
    asmtest_ibs_survey_t empty;
    memset(&empty, 0, sizeof empty);
    CHECK(asmtest_ibs_normalize_blocks(&empty, 0, 0, &b) == ASMTEST_IBS_OK &&
              b.n == 0 && b.blocks == NULL,
          "normalize: empty survey -> OK with no blocks");

    /* NULL arguments are rejected. */
    CHECK(asmtest_ibs_normalize_blocks(NULL, 0, 0, &b) == ASMTEST_IBS_EINVAL,
          "normalize: NULL survey -> EINVAL");
    CHECK(asmtest_ibs_normalize_blocks(&survey, 0, 0, NULL) ==
              ASMTEST_IBS_EINVAL,
          "normalize: NULL out -> EINVAL");
}

/* ---- IBS-Fetch front-end lane (Phase 7): pure synthetic-record checks --------- */
/* IbsFetchCtl (reg[0]) fields, mirroring the backend's fetch decoder. */
#define F_VAL      (1ull << 49) /* IbsFetchVal: sample valid   */
#define F_COMP     (1ull << 50) /* IbsFetchComp: fetch complete */
#define F_ICMISS   (1ull << 51) /* IbsIcMiss: i-cache miss     */
#define F_ITLBMISS (1ull << 55) /* IbsL1TlbMiss: L1 ITLB miss  */
#define F_LAT(x)   (((uint64_t)(x) & 0xFFFFull) << 32) /* IbsFetchLat [47:32] */

/* Build a synthetic ibs_fetch PERF_SAMPLE_RAW payload: [u32 caps][u64 reg0..reg2].
 * reg[0]=IbsFetchCtl (status), reg[1]=IbsFetchLinAd (addr), reg[2]=IbsFetchPhysAd. */
#define FRAW_LEN (4u + 8u * 3u) /* 28 */
static void build_fetch_raw(uint8_t *buf, uint64_t ctl, uint64_t linad) {
    memset(buf, 0, FRAW_LEN);
    uint32_t caps = 0x81bff;
    memcpy(buf + 0, &caps, 4);
    memcpy(buf + 4 + 8 * 0, &ctl, 8);
    memcpy(buf + 4 + 8 * 1, &linad, 8);
    /* reg[2] (physical address) left zero — the decoder does not read it. */
}

/* Host-independent: the pure fetch decoder over synthetic records. */
static void test_decode_fetch(void) {
    uint8_t raw[FRAW_LEN];
    asmtest_ibs_fetch_sample_t f;

    /* 1. A valid, complete fetch is a usable coverage sample at its linear addr. */
    build_fetch_raw(raw, F_VAL | F_COMP, 0x401080);
    CHECK(asmtest_ibs_decode_fetch(raw, FRAW_LEN, &f) == ASMTEST_IBS_OK,
          "decode_fetch: valid complete fetch -> OK");
    CHECK(f.fetch_addr == 0x401080 && f.valid == 1 && f.complete == 1 &&
              f.icache_miss == 0 && f.itlb_miss == 0,
          "decode_fetch: yields the exact fetched address + clean status");

    /* 2. A valid fetch that missed the i-cache carries the miss + its latency. */
    build_fetch_raw(raw, F_VAL | F_COMP | F_ICMISS | F_LAT(200), 0x401100);
    CHECK(asmtest_ibs_decode_fetch(raw, FRAW_LEN, &f) == ASMTEST_IBS_OK &&
              f.icache_miss == 1 && f.latency == 200,
          "decode_fetch: i-cache miss -> OK with icache_miss + latency");

    /* 3. A valid fetch that missed the L1 ITLB carries the itlb_miss bit. */
    build_fetch_raw(raw, F_VAL | F_ITLBMISS, 0x401140);
    CHECK(asmtest_ibs_decode_fetch(raw, FRAW_LEN, &f) == ASMTEST_IBS_OK &&
              f.itlb_miss == 1 && f.icache_miss == 0,
          "decode_fetch: ITLB miss -> OK with itlb_miss");

    /* 4. A record with IbsFetchVal clear decoded fine but has nothing to record. */
    build_fetch_raw(raw, F_COMP, 0x401180);
    CHECK(asmtest_ibs_decode_fetch(raw, FRAW_LEN, &f) == ASMTEST_IBS_NOEDGE &&
              f.valid == 0,
          "decode_fetch: not-valid fetch -> NOEDGE (valid=0)");

    /* 5. A record too short to hold the fetch linear-addr register is undecodable. */
    build_fetch_raw(raw, F_VAL, 0x401080);
    CHECK(asmtest_ibs_decode_fetch(raw, 4u + 8u, &f) == ASMTEST_IBS_EDECODE,
          "decode_fetch: short record (no LinAd reg) -> EDECODE");

    /* 6. NULL arguments are rejected. */
    CHECK(asmtest_ibs_decode_fetch(NULL, FRAW_LEN, &f) == ASMTEST_IBS_EINVAL,
          "decode_fetch: NULL raw -> EINVAL");
    CHECK(asmtest_ibs_decode_fetch(raw, FRAW_LEN, NULL) == ASMTEST_IBS_EINVAL,
          "decode_fetch: NULL out -> EINVAL");
}

/* Host-independent: the Phase-5 additive opts ABI. ASMTEST_IBS_OPTS_INIT must
 * self-describe struct_size and zero every knob, and the additive tail must have
 * landed inside the old reserved words (the struct did not grow past Phase 0). */
static void test_opts_abi(void) {
    asmtest_ibs_opts_t o = ASMTEST_IBS_OPTS_INIT;
    CHECK(o.struct_size == sizeof(asmtest_ibs_opts_t),
          "opts: INIT self-describes struct_size");
    CHECK(o.sample_period == 0 && o.flags == 0 && o.period_jitter == 0,
          "opts: INIT zero-fills every knob");
    CHECK(sizeof(asmtest_ibs_opts_t) == 32,
          "opts: additive tail kept the struct at 32 bytes");
}

/* The fetch availability probe must be definite and stable (cached), independent of
 * the Op probe. */
static void test_fetch_available(void) {
    int a = asmtest_ibs_fetch_available();
    int b = asmtest_ibs_fetch_available();
    CHECK(a == 0 || a == 1, "fetch_available() returns a definite 0/1");
    CHECK(a == b, "fetch_available() is stable across calls");
    const char *why = asmtest_ibs_fetch_skip_reason();
    CHECK(why != NULL, "fetch_skip_reason() is never NULL");
    if (a)
        CHECK(why[0] == '\0', "fetch_skip_reason() is empty when available");
    else
        CHECK(why[0] != '\0',
              "fetch_skip_reason() names a reason when unavailable");
    printf("# IBS-Fetch on this host: %s%s%s\n",
           a ? "AVAILABLE" : "unavailable", a ? "" : " — ", a ? "" : why);
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

/* Two more distinct hot loops for the whole-process test: each is a separate
 * noinline function, so its back-edge lands in its OWN address window and we can
 * tell which worker a sampled edge came from. Different bodies keep them apart. */
static long __attribute__((noinline)) spin_loop2(void) {
    long s = 1;
    for (long i = 0; !g_stop; i++) {
        s ^= (i << 3);
        s += i * 7;
    }
    return s;
}
static long __attribute__((noinline)) spin_loop3(void) {
    long s = 3;
    for (long i = 0; !g_stop; i++) {
        if (i % 3 == 0)
            s *= 2;
        else
            s -= i;
    }
    return s;
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
        printf("# SKIP IBS live capture: %s\n", asmtest_ibs_unavail_reason());
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

    /* Phase 6: normalize the real sampled edge stream into basic-block leaders over
     * spin_loop()'s region — the same branch-target convention the exact tiers use.
     * With an in-region edge present the survey must yield >=1 leader, and every
     * recovered leader must be a REGION-RELATIVE offset (< WIN), so IBS block offsets
     * line up with how the exact blocks[] index this routine. */
    asmtest_ibs_blocks_t blks;
    int nrc = asmtest_ibs_normalize_blocks(&survey, (uint64_t)fn, WIN, &blks);
    CHECK(nrc == ASMTEST_IBS_OK && (!in_range || blks.n > 0),
          "normalize: lifted the spin_loop edges to basic-block leaders");
    int leaders_in_region = 1;
    for (size_t i = 0; i < blks.n; i++)
        if (blks.blocks[i].start >= WIN)
            leaders_in_region = 0;
    CHECK(leaders_in_region,
          "normalize: every block leader is a region-relative offset (< WIN)");
    asmtest_ibs_blocks_free(&blks);

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

/* Whole-process (all-threads) capture: run three distinct hot loops on three worker
 * threads, survey the WHOLE process out of band, and assert the merged histogram
 * carries edges from more than one worker's code window — something no single-tid
 * survey_pid could ever produce, proving the per-thread fan-out merges correctly. */
#include <stdint.h>
static long (*const g_fns[3])(void) = {spin_loop, spin_loop2, spin_loop3};
static volatile int g_ready;

static void *worker_n(void *arg) {
    long (*fn)(void) = g_fns[(int)(intptr_t)arg];
    __sync_fetch_and_add(&g_ready, 1);
    g_sink = fn();
    return NULL;
}

/* Is there a survey edge whose endpoints both fall inside [fn, fn+WIN)? */
static int edge_in_fn(const asmtest_ibs_survey_t *s, long (*fn)(void)) {
    uintptr_t base = (uintptr_t)(void *)fn;
    const uintptr_t WIN = 0x2000;
    for (size_t i = 0; i < s->n; i++) {
        uintptr_t from = (uintptr_t)s->edges[i].from;
        uintptr_t to = (uintptr_t)s->edges[i].to;
        if (from >= base && from < base + WIN && to >= base && to < base + WIN)
            return 1;
    }
    return 0;
}

static void test_live_process(void) {
    if (!asmtest_ibs_available()) {
        printf("# SKIP IBS whole-process capture: %s\n",
               asmtest_ibs_skip_reason());
        return;
    }
    g_stop = 0;
    g_ready = 0;
    pthread_t th[3];
    int made = 0;
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&th[i], NULL, worker_n, (void *)(intptr_t)i) == 0)
            made++;
        else
            break;
    }
    if (made < 3) { /* partial spawn: stop what started, skip the check */
        g_stop = 1;
        for (int i = 0; i < made; i++)
            pthread_join(th[i], NULL);
        printf("# SKIP IBS whole-process capture: pthread_create failed\n");
        return;
    }
    while (g_ready < 3) {
        struct timespec s = {0, 1000 * 1000};
        nanosleep(&s, NULL);
    }

    /* Survey EVERY thread of this process (0 => self) out of band. */
    asmtest_ibs_survey_t s;
    int rc = asmtest_ibs_survey_process(0, 400, NULL, &s);

    g_stop = 1;
    for (int i = 0; i < 3; i++)
        pthread_join(th[i], NULL);

    if (rc == ASMTEST_IBS_EUNAVAIL) {
        printf("# SKIP IBS whole-process capture: %s\n",
               asmtest_ibs_unavail_reason());
        asmtest_ibs_survey_free(&s);
        return;
    }
    CHECK(rc == ASMTEST_IBS_OK,
          "survey_process: whole-process capture succeeds");
    CHECK(s.n > 0, "survey_process: produced aggregated edges");

    int covered = edge_in_fn(&s, spin_loop) + edge_in_fn(&s, spin_loop2) +
                  edge_in_fn(&s, spin_loop3);
    /* Two-plus distinct worker functions in one merged survey can only come from
     * sampling multiple threads together (each worker runs exactly one function).
     * A >=2 gate — not ==3 — stays robust under IBS's throttle governor sharing the
     * sample budget across the three concurrent events. */
    CHECK(covered >= 2,
          "survey_process: merged edges from multiple worker threads");
    printf(
        "# whole-process: %d/3 worker functions covered, %zu edges, %llu/%llu "
        "branch/total samples%s\n",
        covered, s.n, (unsigned long long)s.branch_samples,
        (unsigned long long)s.samples, s.throttled ? ", throttled" : "");
    asmtest_ibs_survey_free(&s);
}

/* Live IBS-FETCH front-end coverage: run the same hot loop on a worker thread, survey
 * its FETCH stream out of band, and assert a fetched address lands inside spin_loop()'s
 * own code window — proving the front-end sampler covers the code the loop runs. Same
 * self-skip discipline as the Op tests (off IBS-Fetch / perf-blocked). */
static void test_live_fetch(void) {
    if (!asmtest_ibs_fetch_available()) {
        printf("# SKIP IBS-Fetch survey: %s\n",
               asmtest_ibs_fetch_skip_reason());
        return;
    }
    g_stop = 0;
    g_tid_ready = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) != 0) {
        printf("# SKIP IBS-Fetch survey: pthread_create failed\n");
        return;
    }
    while (!g_tid_ready) {
        struct timespec s = {0, 1000 * 1000};
        nanosleep(&s, NULL);
    }

    /* Survey the WORKER's fetched code out of band from THIS thread. */
    asmtest_ibs_fetch_survey_t fs;
    int rc = asmtest_ibs_survey_fetch_pid(g_worker_tid, 300, NULL, &fs);

    g_stop = 1;
    pthread_join(th, NULL);

    /* fetch_available() is a SUBSTRATE probe; perf_event_open can still be blocked
     * (paranoid=4 without CAP_PERFMON, seccomp) — a skip, not a failure. */
    if (rc == ASMTEST_IBS_EUNAVAIL) {
        printf("# SKIP IBS-Fetch survey: %s\n", asmtest_ibs_unavail_reason());
        asmtest_ibs_fetch_survey_free(&fs);
        return;
    }

    CHECK(rc == ASMTEST_IBS_OK,
          "survey_fetch_pid: out-of-band fetch survey succeeds");
    CHECK(fs.samples > 0,
          "survey_fetch_pid: sampled the worker's fetch stream");
    CHECK(fs.valid_samples > 0,
          "survey_fetch_pid: recorded valid fetch samples");
    CHECK(fs.n > 0,
          "survey_fetch_pid: produced at least one fetch-address bucket");

    /* A fetched address must fall within spin_loop()'s own code window — the front-end
     * fetched the loop body it is running. */
    uintptr_t fn = (uintptr_t)(void *)&spin_loop;
    const uintptr_t WIN = 0x2000; /* generous for an -O0 function */
    int in_range = 0;
    for (size_t i = 0; i < fs.n; i++) {
        uintptr_t a = (uintptr_t)fs.hot[i].addr;
        if (a >= fn && a < fn + WIN)
            in_range = 1;
    }
    CHECK(in_range,
          "survey_fetch_pid: a fetched address lies within spin_loop()");

    if (fs.n > 0) {
        printf(
            "# top fetch: %#lx  count=%llu  (%zu addrs, %llu/%llu valid/total "
            "samples, %llu ic-miss, %llu itlb-miss%s)\n",
            (unsigned long)fs.hot[0].addr, (unsigned long long)fs.hot[0].count,
            fs.n, (unsigned long long)fs.valid_samples,
            (unsigned long long)fs.samples,
            (unsigned long long)fs.icache_misses,
            (unsigned long long)fs.itlb_misses,
            fs.throttled ? ", throttled" : "");
    }
    asmtest_ibs_fetch_survey_free(&fs);
    CHECK(fs.hot == NULL && fs.n == 0,
          "fetch_survey_free: releases and zeroes the survey");
}

/* Phase 5: drive the opt knobs on the live out-of-band survey. Callchain ON must
 * still recover the spin loop's back-edge — proving the drain steps over the
 * per-sample caller-stack block to reach the RAW edge record. A second window opts
 * OUT of the dispatched-op default and disables jitter, exercising those paths too. */
static void test_live_phase5(void) {
    if (!asmtest_ibs_available()) {
        printf("# SKIP IBS Phase-5 opts: %s\n", asmtest_ibs_skip_reason());
        return;
    }
    g_stop = 0;
    g_tid_ready = 0;
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL) != 0) {
        printf("# SKIP IBS Phase-5 opts: pthread_create failed\n");
        return;
    }
    while (!g_tid_ready) {
        struct timespec s = {0, 1000 * 1000};
        nanosleep(&s, NULL);
    }

    asmtest_ibs_opts_t o = ASMTEST_IBS_OPTS_INIT;
    o.flags = ASMTEST_IBS_OPT_CALLCHAIN; /* attach a caller stack per sample */
    asmtest_ibs_survey_t s1;
    int rc = asmtest_ibs_survey_pid(g_worker_tid, 300, &o, &s1);

    /* Opt out of BOTH Phase-5 defaults: legacy cycle counting + no period jitter. */
    asmtest_ibs_opts_t o2 = ASMTEST_IBS_OPTS_INIT;
    o2.flags = ASMTEST_IBS_OPT_COUNT_CYCLES | ASMTEST_IBS_OPT_NO_JITTER;
    asmtest_ibs_survey_t s2;
    int rc2 = asmtest_ibs_survey_pid(g_worker_tid, 200, &o2, &s2);

    g_stop = 1;
    pthread_join(th, NULL);

    if (rc == ASMTEST_IBS_EUNAVAIL) {
        printf("# SKIP IBS Phase-5 opts: %s\n", asmtest_ibs_unavail_reason());
        asmtest_ibs_survey_free(&s1);
        asmtest_ibs_survey_free(&s2);
        return;
    }

    CHECK(rc == ASMTEST_IBS_OK, "phase5: callchain survey succeeds");
    CHECK(edge_in_fn(&s1, spin_loop),
          "phase5: callchain-on survey still recovers the spin_loop edge");
    CHECK(rc2 == ASMTEST_IBS_OK && s2.n > 0,
          "phase5: cycle-count + no-jitter survey still produces edges");
    asmtest_ibs_survey_free(&s1);
    asmtest_ibs_survey_free(&s2);
}

/* Phase 5: system-wide (per-CPU) whole-process capture. Needs CAP_PERFMON /
 * perf_event_paranoid<=0 and self-skips (EUNAVAIL) otherwise. When it opens it must
 * still surface edges from the target's own worker code — proving the per-CPU open +
 * software pid-filter path delivers the target (not the whole system) unperturbed. */
static void test_live_system_wide(void) {
    if (!asmtest_ibs_available()) {
        printf("# SKIP IBS system-wide capture: %s\n",
               asmtest_ibs_skip_reason());
        return;
    }
    g_stop = 0;
    g_ready = 0;
    pthread_t th[3];
    int made = 0;
    for (int i = 0; i < 3; i++) {
        if (pthread_create(&th[i], NULL, worker_n, (void *)(intptr_t)i) == 0)
            made++;
        else
            break;
    }
    if (made < 3) {
        g_stop = 1;
        for (int i = 0; i < made; i++)
            pthread_join(th[i], NULL);
        printf("# SKIP IBS system-wide capture: pthread_create failed\n");
        return;
    }
    while (g_ready < 3) {
        struct timespec s = {0, 1000 * 1000};
        nanosleep(&s, NULL);
    }

    asmtest_ibs_opts_t o = ASMTEST_IBS_OPTS_INIT;
    o.flags = ASMTEST_IBS_OPT_SYSTEM_WIDE;
    asmtest_ibs_survey_t s;
    int rc = asmtest_ibs_survey_process(0, 400, &o, &s);

    g_stop = 1;
    for (int i = 0; i < 3; i++)
        pthread_join(th[i], NULL);

    if (rc == ASMTEST_IBS_EUNAVAIL) {
        printf("# SKIP IBS system-wide capture: needs CAP_PERFMON / "
               "paranoid<=0 (substrate present) — %s\n",
               asmtest_ibs_unavail_reason());
        asmtest_ibs_survey_free(&s);
        return;
    }
    CHECK(rc == ASMTEST_IBS_OK,
          "system_wide: per-CPU whole-process capture succeeds");
    CHECK(s.n > 0, "system_wide: produced aggregated edges");
    int covered = edge_in_fn(&s, spin_loop) + edge_in_fn(&s, spin_loop2) +
                  edge_in_fn(&s, spin_loop3);
    /* The address windows are in THIS binary, so any covered edge is the target's
     * own code: covered>=1 proves the pid-filter delivered us, not the whole system.
     * (A >=1 gate stays robust under the throttle governor sharing the whole-machine
     * sample budget across every process.) */
    CHECK(
        covered >= 1,
        "system_wide: recovered the target's own worker edges (pid-filtered)");
    printf("# system-wide: %d/3 worker functions covered, %zu edges, %llu/%llu "
           "branch/total samples%s\n",
           covered, s.n, (unsigned long long)s.branch_samples,
           (unsigned long long)s.samples, s.throttled ? ", throttled" : "");
    asmtest_ibs_survey_free(&s);
}

/* Pure (no perf): pin the callchain-aware max-record bound. Runs on every
 * Linux/x86-64 host, AMD or not — the ring-loss heuristic must reserve the
 * callchain worst case (~1.2 KB), not the 112-byte base, or callchain samples
 * vanish with lost==0 && throttled==0. A pre-fix build returns 112 for
 * has_callchain=1 and the second CHECK prints `not ok`. */
static void test_record_bound(void) {
    CHECK(asmtest_ibs_max_record(0) == 112,
          "record-bound: base (no-callchain) bound is 112 (fetch/sw lanes "
          "unchanged)");
    CHECK(asmtest_ibs_max_record(1) >= 24u + 8u * 136u + 80u,
          "record-bound: callchain bound covers the ABI worst case (>= 1192)");
    CHECK(asmtest_ibs_max_record(1) < 64u * 4096u / 4u,
          "record-bound: callchain bound still leaves the 256 KiB ring usable");
}
#else
static void test_live(void) {
    printf("# SKIP IBS live capture: not Linux x86-64\n");
}
static void test_live_process(void) {
    printf("# SKIP IBS whole-process capture: not Linux x86-64\n");
}
static void test_live_fetch(void) {
    printf("# SKIP IBS-Fetch survey: not Linux x86-64\n");
}
static void test_live_phase5(void) {
    printf("# SKIP IBS Phase-5 opts: not Linux x86-64\n");
}
static void test_live_system_wide(void) {
    printf("# SKIP IBS system-wide capture: not Linux x86-64\n");
}
static void test_record_bound(void) {
    printf("# SKIP IBS record-bound: not Linux x86-64\n");
}
#endif

int main(void) {
    test_decode();
    test_normalize();
    test_decode_fetch();
    test_opts_abi();
    test_available();
    test_fetch_available();
    test_record_bound();
    test_live();
    test_live_process();
    test_live_fetch();
    test_live_phase5();
    test_live_system_wide();
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
