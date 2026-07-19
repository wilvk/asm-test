/*
 * pin_apx_workload.c — the launched workload for the APX positive half (T8). Same
 * shm scaffold as examples/pin_trace_workload.c, but the region is the APX
 * (EGPR/REX2) fixture (pin_apx_fixture.h). Run as
 *   pin -t asmtest_pintool.so -shm /name -- ./pin_apx_workload /name
 * ONLY on APX silicon — it EXECUTES r16/r17 instructions, which #UD on a non-APX
 * CPU. The pintool-apx-test recipe gates this run behind a CPUID APX_F probe
 * (pin_apx_decode cpuid); the ungated decode assertion never runs these bytes.
 */
#include "pin_apx_fixture.h"
#include "pintool_shm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* App-side markers the tool resolves by symbol (see pin_trace_workload.c). */
static volatile unsigned long g_begin_sink, g_end_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_trace_begin(const char *name) {
    g_begin_sink += 0x33 + (unsigned long)(uintptr_t)name;
}
__attribute__((noinline, visibility("default"))) void
asmtest_trace_end(const char *name) {
    g_end_sink += 0x44 + (unsigned long)(uintptr_t)name;
}

typedef long (*add2_fn)(long, long);

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : PIN_SHM_NAME;

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("pin_apx_workload: shm_open");
        return 2;
    }
    if (ftruncate(fd, (off_t)sizeof(asmtest_pin_channel_t)) != 0) {
        perror("pin_apx_workload: ftruncate");
        return 2;
    }
    asmtest_pin_channel_t *shm = (asmtest_pin_channel_t *)mmap(
        NULL, sizeof(asmtest_pin_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        perror("pin_apx_workload: mmap");
        return 2;
    }
    memset(shm, 0, sizeof *shm);
    shm->magic = PIN_SHM_MAGIC;

    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("pin_apx_workload: mmap code");
        return 2;
    }
    memcpy(code, APX_ROUTINE, sizeof APX_ROUTINE);
    if (mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("pin_apx_workload: mprotect");
        return 2;
    }

    /* Publish the region BEFORE the first marker (see pin_trace_workload.c). */
    shm->region_base = (uint64_t)(uintptr_t)code;
    shm->region_len = (uint64_t)sizeof APX_ROUTINE;
    strncpy(shm->region_name, APX_REGION_NAME, sizeof shm->region_name - 1);

    add2_fn fn = (add2_fn)code;
    asmtest_trace_begin(APX_REGION_NAME);
    long r = fn(3, 4); /* r16 = 3 + 4 = 7 -> rax (executes APX; #UD off-APX) */
    asmtest_trace_end(APX_REGION_NAME);

    shm->result = (int64_t)r; /* 7 */
    __atomic_store_n(&shm->done, 1u, __ATOMIC_RELEASE);

    fprintf(stderr, "pin_apx_workload: done (result=%lld, insns_total=%llu)\n",
            (long long)shm->result, (unsigned long long)shm->insns_total);
    munmap(code, 4096);
    munmap(shm, sizeof *shm);
    return 0;
}
