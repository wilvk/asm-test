/*
 * taint_workload.c — the LAUNCHED native workload for the DynamoRIO taint tier's
 * launch-under-DR path (Increment 5, first slice). Runs as
 *   drrun -c libasmtest_drtaint_client.so -- ./taint_workload [/shm-name]
 * so DR owns the process from a clean start and the taint client instruments it. This
 * program does NOT drive the in-process DR lifecycle (no dr_init/dr_start — DR is already
 * active via drrun); it just exports the marker symbols the client resolves by PC, maps
 * the POSIX shared-memory results channel, materializes a fixture into an RWX page, and
 * runs it. The client seeds the buffer, propagates taint inline, and writes the sink hit
 * SYNCHRONOUSLY into the shared report; a separate validator process drains + oracle-diffs.
 *
 * The marker symbols are defined HERE (not linked from dataflow_dr.c) so the workload is
 * self-contained; -rdynamic puts them in the dynamic symbol table for dr_get_proc_address.
 *
 * Fixture = taint_sink_chain (KEEP IN SYNC with examples/dr_taint.c): a seeded load →
 * derive into a flag → conditional branch (the sink). Seeding [buf,buf+8) makes the jz at
 * 0x10 read a tainted ZF → one hit.
 */
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"

#include "dataflow_dr.h" /* at_drval_t (the region marker's 3rd arg) */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* App-side markers the client resolves by PC (same symbols/ABI as dataflow_dr.c). The
 * volatile sinks keep the noinline bodies from folding away so each has a stable entry. */
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

/* A region marker needs a non-NULL at_drval_t so the client activates taint for the
 * range; the value arrays stay NULL (this slice ships only the synchronous sink report,
 * not the drx_buf-buffered value trace). Static so it outlives main for the exit flush. */
static at_drval_t g_dv;

typedef long (*fn2_t)(long, long);

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : AT_SHM_NAME;

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("taint_workload: shm_open");
        return 2;
    }
    if (ftruncate(fd, (off_t)sizeof(at_shm_channel_t)) != 0) {
        perror("taint_workload: ftruncate");
        return 2;
    }
    at_shm_channel_t *shm =
        (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("taint_workload: mmap");
        return 2;
    }
    memset(shm, 0, sizeof *shm);
    shm->report.hits =
        shm->hits; /* producer-space ptr; consumer reads hits[] by offset */
    shm->report.hits_cap = AT_SHM_HITS_CAP;

    /* Seeded buffer the fixture loads from (a plain data page; the client paints its
     * shadow via the seed marker). */
    uint64_t seedbuf = 7;

    /* Materialize the fixture into an RWX page (DR instruments it like any app code). */
    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("taint_workload: mmap code");
        return 2;
    }
    memcpy(code, taint_sink_chain, sizeof taint_sink_chain);

    /* Register the region + the sink report, paint the seed, then run (each a rare PC-
     * resolved clean call in the client). The sink hit is written synchronously when the
     * branch executes, so it is in the shared report by the time the fixture returns. */
    asmtest_dr_valcapture_marker(code, sizeof taint_sink_chain, &g_dv);
    asmtest_dr_taint_sink_marker(&shm->report);
    asmtest_dr_taint_seed_marker(&seedbuf, sizeof seedbuf, AT_TAG_TAINTED);
    long r = ((fn2_t)code)((long)(uintptr_t)&seedbuf, 5);

    shm->result = r;
    __atomic_store_n(&shm->done, 1u, __ATOMIC_RELEASE);

    fprintf(stderr, "taint_workload: done (result=%ld, hits=%u)\n", r,
            (unsigned)shm->report.hits_len);
    return 0;
}
