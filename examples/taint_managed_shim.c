/*
 * taint_managed_shim.c — native P/Invoke shim for the taint tier's MANAGED seed->sink lane
 * (dynamorio-taint-tier-plan.md, Increment 5 exit criterion 3). Built as
 * libtaint_managed_shim.so and loaded by the launched .NET workload
 * (examples/taint_managed/Program.cs) via [DllImport].
 *
 * A launched `dotnet app.dll` calls no C markers, and its managed heap moves under GC — so
 * this native shim owns the plumbing that the managed side cannot express portably:
 *  - it EXPORTS the seed/sink marker symbols the DR taint client resolves by PC across all
 *    modules (event_module_load fires when this .so loads), same ABI as taint_workload.c;
 *  - it maps the POSIX shared-memory results channel and registers &shm->report at the sink
 *    marker, so the branch-condition sink's hit crosses to the out-of-process validator;
 *  - it holds a NATIVE seed buffer (g_seedbuf) — a stable, never-GC-moved address — that the
 *    managed HotSeedSink() reads through a raw pointer (the Increment-5 seed->sink lane); and it
 *    exposes shim_seed_at() to paint an ARBITRARY address, which the Increment-7 GC-move
 *    survival lane uses to seed a GC-movable managed object at its current (briefly-pinned)
 *    address so the seed can survive a compacting GC move (taint_gcmove_managed).
 *
 * Flow (driven from managed Main): shim_init(name) maps shm + registers the sink report;
 * shim_seed() paints g_seedbuf's shadow via the seed marker; managed code then loops calling
 * HotSeedSink(shim_seedbuf()) — the perfmap poller (methodscan=Hot) registers that JIT'd
 * method, its seeded load->cmp->branch trips the sink, and the hit is appended to shm;
 * shim_finish(result) publishes done. The markers are compiled here (not linked from
 * dataflow_dr.c) so the shim is self-contained; -Wl,-E / default visibility puts them in the
 * dynamic symbol table for dr_get_proc_address.
 */
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"

#include "dataflow_dr.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Marker symbols the DR taint client resolves by PC (same ABI as taint_workload.c). The
 * volatile sinks keep the noinline bodies from folding so each has a stable call target. */
static volatile unsigned long g_seed_sink, g_sink_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_seed_marker(void *base, size_t len, unsigned long color) {
    g_seed_sink += 0x91 + (unsigned long)(uintptr_t)base + len + color;
}
__attribute__((noinline, visibility("default"))) void
asmtest_dr_taint_sink_marker(void *report) {
    g_sink_sink += 0xA3 + (unsigned long)(uintptr_t)report;
}

/* The native seed buffer the managed HotSeedSink reads through a raw pointer. A stable
 * address (no GC), so the seed marker's shadow paint stays valid for every instrumented
 * call. */
static uint64_t g_seedbuf;
static at_shm_channel_t *g_shm;

/* Map the shm channel and register &g_shm->report at the sink marker so the client appends
 * hits there. Returns 0 on success, non-zero on failure. */
__attribute__((visibility("default"))) int shim_init(const char *name) {
    if (name == NULL || name[0] == '\0')
        name = AT_SHM_NAME;
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("taint_managed_shim: shm_open");
        return 2;
    }
    if (ftruncate(fd, (off_t)sizeof(at_shm_channel_t)) != 0) {
        perror("taint_managed_shim: ftruncate");
        return 2;
    }
    g_shm = (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                     PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_shm == MAP_FAILED) {
        perror("taint_managed_shim: mmap");
        g_shm = NULL;
        return 2;
    }
    memset(g_shm, 0, sizeof *g_shm);
    g_shm->report.hits =
        g_shm->hits; /* producer-space; consumer reads by offset */
    g_shm->report.hits_cap = AT_SHM_HITS_CAP;
    /* Register the shm report at the sink marker (a PC-resolved clean call in the client). */
    asmtest_dr_taint_sink_marker(&g_shm->report);
    return 0;
}

/* Return the stable address of the native seed buffer (managed passes this to HotSeedSink). */
__attribute__((visibility("default"))) void *shim_seedbuf(void) {
    return &g_seedbuf;
}

/* Set the seed buffer's value and paint its 8-byte shadow tainted via the seed marker. Call
 * BEFORE the instrumented HotSeedSink runs; the shadow is process-global + monotone, so the
 * paint persists for every later instrumented call. */
__attribute__((visibility("default"))) void shim_seed(uint64_t val) {
    g_seedbuf = val;
    asmtest_dr_taint_seed_marker(&g_seedbuf, sizeof g_seedbuf, AT_TAG_TAINTED);
}

/* Seed an ARBITRARY address's shadow tainted via the seed marker (Increment 7 GC-move
 * survival). Unlike shim_seed (which paints the stable native g_seedbuf), this paints a
 * caller-supplied address — the managed workload passes a GC-movable object's CURRENT data
 * address (obtained by a brief pin) so the seed can then survive a compacting GC move. The
 * shadow is created-on-touch by the seed marker's clean call, so any canonical address works. */
__attribute__((visibility("default"))) void shim_seed_at(void *addr,
                                                         uint64_t len) {
    asmtest_dr_taint_seed_marker(addr, (size_t)len, AT_TAG_TAINTED);
}

/* Publish the fixture result + done so the out-of-process validator can drain. */
__attribute__((visibility("default"))) void shim_finish(long result) {
    if (g_shm == NULL)
        return;
    g_shm->result = result;
    __atomic_store_n(&g_shm->done, 1u, __ATOMIC_RELEASE);
    fprintf(stderr,
            "taint_managed_shim: done (result=%ld, hits=%u total=%llu)\n",
            result, (unsigned)g_shm->report.hits_len,
            (unsigned long long)g_shm->report.hits_total);
}
