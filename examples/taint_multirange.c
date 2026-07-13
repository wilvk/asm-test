/*
 * taint_multirange.c — the LAUNCHED native workload for the DynamoRIO taint tier's
 * multi-range / method-range-scoping slice (Increment 6). Runs as
 *   drrun -c libasmtest_drtaint_client.so [scope=whole] -- ./taint_multirange [/shm-name]
 * so DR owns the process from a clean start and the taint client instruments it.
 *
 * Unlike the single-region taint_workload.c, this registers TWO disjoint instrumented
 * ranges (range A + range B, see taint_multirange_fixture.h) around an un-instrumented GAP,
 * then seeds a buffer and runs the fixture. The taint flows: seed load (range A) -> spill
 * to [rsp-8] (range A) -> [un-instrumented gap] -> reload from [rsp-8] (range B) -> derive
 * -> branch sink (range B). Because the client's tag shadow is process-global, the stack
 * tag written in range A survives the gap and is read back in range B, so the sink fires
 * even though the code between the ranges was never instrumented — the plan's boundary
 * policy. A separate validator drains the shm channel and oracle-diffs the taint set.
 *
 * The marker symbols are defined HERE (as in taint_workload.c); -rdynamic exports them for
 * the client's dr_get_proc_address. The client reports its instrumented-instruction count
 * and registered range count to stderr (ASMTEST_TAINT_INSCOUNT ...), which the make lane
 * greps for the range-count-> 1 and scope=whole-vs-ranges inscount-delta assertions.
 */
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"
#include "taint_multirange_fixture.h"

#include "dataflow_dr.h" /* at_drval_t (the region marker's 3rd arg) */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* App-side markers the client resolves by PC (same symbols/ABI as dataflow_dr.c /
 * taint_workload.c). The volatile sinks keep the noinline bodies from folding away. */
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

typedef long (*fn2_t)(long, long);

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : AT_SHM_NAME;

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("taint_multirange: shm_open");
        return 2;
    }
    if (ftruncate(fd, (off_t)sizeof(at_shm_channel_t)) != 0) {
        perror("taint_multirange: ftruncate");
        return 2;
    }
    at_shm_channel_t *shm =
        (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) {
        perror("taint_multirange: mmap");
        return 2;
    }
    memset(shm, 0, sizeof *shm);
    shm->report.hits =
        shm->hits; /* producer-space ptr; consumer reads by offset */
    shm->report.hits_cap = AT_SHM_HITS_CAP;
    shm->drval.steps = shm->steps;
    shm->drval.steps_cap = AT_SHM_STEPS_CAP;
    shm->drval.step_taint = shm->step_taint;
    shm->drval.step_taint_cap = AT_SHM_STEPS_CAP;

    /* Seeded buffer the fixture loads from (a plain data page; the client paints its
     * shadow via the seed marker). */
    uint64_t seedbuf = 7;

    /* Materialize the fixture into an RWX page (DR instruments it like any app code). */
    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("taint_multirange: mmap code");
        return 2;
    }
    memcpy(code, taint_multirange_code, sizeof taint_multirange_code);

    /* Register the TWO instrumented ranges (both feeding the one shared capture buffer),
     * the sink report, and the seed — each a rare PC-resolved clean call in the client.
     * The gap between the ranges is deliberately NOT registered. */
    asmtest_dr_valcapture_marker((uint8_t *)code + TMR_RANGE_A_OFF,
                                 TMR_RANGE_A_LEN, &shm->drval);
    asmtest_dr_valcapture_marker((uint8_t *)code + TMR_RANGE_B_OFF,
                                 TMR_RANGE_B_LEN, &shm->drval);
    asmtest_dr_taint_sink_marker(&shm->report);
    asmtest_dr_taint_seed_marker(&seedbuf, sizeof seedbuf, AT_TAG_TAINTED);

    long r = ((fn2_t)code)((long)(uintptr_t)&seedbuf, 5);

    shm->result = r;
    __atomic_store_n(&shm->done, 1u, __ATOMIC_RELEASE);

    fprintf(stderr, "taint_multirange: done (result=%ld, hits=%u)\n", r,
            (unsigned)shm->report.hits_len);
    return 0;
}
