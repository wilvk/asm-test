/*
 * taint_stress.c — concurrent-writer stress for the DynamoRIO taint tier's process-global
 * shadow (Increment 5; validates the Increment-4 race policy). Launched under
 *   drrun -c libasmtest_drtaint_client.so -- ./taint_stress
 * it spawns N threads that, released together by a barrier, ALL seed a disjoint buffer
 * slice and run the branch-sink fixture at once — so the client's process-global tag
 * shadow takes concurrent leaf-CAS installs (nearby seed buffers share a 1 MiB leaf; each
 * thread's stack spill first-touches its own leaf) and concurrent single-byte tag stores,
 * and the process-global sink report takes concurrent appends.
 *
 * The Increment-4 policy: aligned at_tag_t byte writes are atomic and a union tag is
 * monotone within a seed epoch, so a lost update is a conservative MISS, never a false
 * clean->tainted flip; the leaf CAS install is the one mandatory-atomic mutation; the sink
 * report append is atomic (a unique fetch-add slot). With DISJOINT per-thread buffers there
 * is no true data race on any byte, so the correct, checkable outcome is: EXACTLY N sink
 * hits, every one at the sink offset, tainted, kind 1 — no crash, no hang, no false flip,
 * no lost/corrupted hit. Checked in-process after join (the report is process-local; this
 * stress is about concurrency, not the cross-process channel).
 *
 * Self-contained (defines the marker symbols; -rdynamic exports them). No in-process
 * dr_init/dr_start — DR owns the process via drrun. KEEP the fixture IN SYNC with dr_taint.c.
 */
#include "asmtest_taint.h"

#include "dataflow_dr.h" /* at_drval_t (the region marker's 3rd arg) */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static volatile unsigned long g_v_sink, g_seed_sink, g_sink_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_dr_valcapture_marker(void *base, size_t len, void *drval) {
    g_v_sink += 0x77 + (unsigned long)(uintptr_t)base + len +
                (unsigned long)(uintptr_t)drval;
}
__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_seed_marker(void *base, size_t len, unsigned long color) {
    g_seed_sink += 0x91 + (unsigned long)(uintptr_t)base + len + color;
}
__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_sink_marker(void *report) {
    g_sink_sink += 0xA3 + (unsigned long)(uintptr_t)report;
}

/* KEEP IN SYNC with examples/dr_taint.c taint_sink_chain. */
static const uint8_t taint_sink_chain[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]   (SEED origin)    */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax (spill)          */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8] (reload)         */
    0x48, 0x01, 0xf1,             /* 0x0d add rcx, rsi     (rcx+flags taint)*/
    0x74, 0x03,                   /* 0x10 jz 0x15          (SINK: taint ZF) */
    0x48, 0x89, 0xc8,             /* 0x12 mov rax, rcx                      */
    0xc3,                         /* 0x15 ret                               */
};
#define SINK_OFF 0x10
#define NTHREADS 8
#define HITS_CAP 64

typedef long (*fn2_t)(long, long);

static uint8_t *g_code;
static uint64_t
    g_buf[NTHREADS]; /* contiguous -> concurrent same-leaf seed CAS */
static at_taint_report_t g_report;
static at_taint_hit_t g_hits[HITS_CAP];
static at_drval_t
    g_dv; /* region marker needs non-NULL; value arrays stay NULL */
static pthread_barrier_t g_barrier;

static void *worker(void *arg) {
    long i = (long)(intptr_t)arg;
    g_buf[i] = 7;
    pthread_barrier_wait(&g_barrier); /* all threads seed + run together */
    asmtest_dr_taint_seed_marker(&g_buf[i], sizeof(uint64_t), AT_TAG_TAINTED);
    ((fn2_t)g_code)((long)(uintptr_t)&g_buf[i], 5);
    return NULL;
}

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    g_code = (uint8_t *)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_code == MAP_FAILED) {
        perror("taint_stress: mmap");
        return 2;
    }
    memcpy(g_code, taint_sink_chain, sizeof taint_sink_chain);

    memset(&g_report, 0, sizeof g_report);
    g_report.hits = g_hits;
    g_report.hits_cap = HITS_CAP;

    /* Register the region + the report ONCE, before the threads run it concurrently. */
    asmtest_dr_valcapture_marker(g_code, sizeof taint_sink_chain, &g_dv);
    asmtest_dr_taint_sink_marker(&g_report);

    pthread_barrier_init(&g_barrier, NULL, NTHREADS);
    pthread_t th[NTHREADS];
    int spawned = 0;
    for (long i = 0; i < NTHREADS; i++)
        if (pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i) == 0)
            spawned++;
    for (int i = 0; i < spawned; i++)
        pthread_join(th[i], NULL);
    pthread_barrier_destroy(&g_barrier);

    CHECK(spawned == NTHREADS, "stress: all worker threads spawned");
    CHECK(
        g_report.hits_total == (uint64_t)NTHREADS,
        "stress: exactly N sink hits under concurrent writers (no lost/extra)");
    CHECK(!g_report.truncated, "stress: no report truncation (cap >= N)");

    int bad = 0;
    uint64_t n =
        g_report.hits_total < HITS_CAP ? g_report.hits_total : HITS_CAP;
    for (uint64_t j = 0; j < n; j++)
        if (g_report.hits[j].off != SINK_OFF ||
            g_report.hits[j].tag == AT_TAG_CLEAN || g_report.hits[j].kind != 1)
            bad++;
    CHECK(bad == 0, "stress: every hit correct (off/tag/kind) — no false flip, "
                    "no corruption");

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
