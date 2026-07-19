/*
 * pin_trace_workload.c — the LAUNCHED native workload for the XED-decoded Pin
 * trace tier (PIN-2). Runs as
 *   pin -t asmtest_pintool.so -shm /name -- ./pin_trace_workload /name
 * so Pin owns the process and the tool instruments it. It exports the
 * asmtest_trace_begin / _end marker symbols the tool resolves with RTN_FindByName
 * (the Pin analog of the DynamoRIO client's dr_get_proc_address), maps the POSIX
 * shared-memory results channel, materializes the shared parity ROUTINE into a W^X
 * page, and runs it twice between the markers.
 *
 * The markers are defined HERE (not linked from drtrace_app.c — its markers would
 * collide, and this workload runs under `pin`, not the DR lifecycle) so the program
 * is self-contained; -rdynamic puts them in the dynamic symbol table for
 * RTN_FindByName. The tool records nothing when run WITHOUT `-t` — the marker
 * bodies are ordinary no-ops, so a bare `pin --` (or native run) still exits 0 with
 * zero counters, and the tool is purely additive.
 */
#include "pintool_shm.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* App-side markers the tool resolves by symbol (same body shape as
 * src/drtrace_app.c:462). The volatile sinks keep the noinline bodies from folding
 * away so each has a stable entry point. */
static volatile unsigned long g_begin_sink, g_end_sink;

__attribute__((noinline, visibility("default"))) void
asmtest_trace_begin(const char *name) {
    g_begin_sink += 0x33 + (unsigned long)(uintptr_t)name;
}
__attribute__((noinline, visibility("default"))) void
asmtest_trace_end(const char *name) {
    g_end_sink += 0x44 + (unsigned long)(uintptr_t)name;
}

/* KEEP IN SYNC with examples/test_drtrace.c ROUTINE (the shared parity fixture):
 *   0:  48 89 f8             mov    rax, rdi          (off 0)
 *   3:  48 01 f0             add    rax, rsi
 *   6:  48 3d 64 00 00 00    cmp    rax, 100
 *   c:  7e 03                jle    .skip  ; +3 skips the dec (off 0xc)
 *   e:  48 ff c8             dec    rax               (off 0xe)
 *  11:  c3                   ret                      (off 0x11)  */
static const uint8_t ROUTINE[] = {
    0x48, 0x89, 0xf8,                   /* mov rax, rdi          (off 0)  */
    0x48, 0x01, 0xf0,                   /* add rax, rsi                   */
    0x48, 0x3d, 0x64, 0x00, 0x00, 0x00, /* cmp rax, 100                   */
    0x7e, 0x03,                         /* jle +3 -> ret (off 0xc)        */
    0x48, 0xff, 0xc8,                   /* dec rax               (off 0xe)*/
    0xc3                                /* ret                   (off 0x11)*/
};

typedef long (*add2_fn)(long, long);

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : PIN_SHM_NAME;

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("pin_trace_workload: shm_open");
        return 2;
    }
    if (ftruncate(fd, (off_t)sizeof(asmtest_pin_channel_t)) != 0) {
        perror("pin_trace_workload: ftruncate");
        return 2;
    }
    asmtest_pin_channel_t *shm = (asmtest_pin_channel_t *)mmap(
        NULL, sizeof(asmtest_pin_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        perror("pin_trace_workload: mmap");
        return 2;
    }
    memset(shm, 0, sizeof *shm);
    shm->magic = PIN_SHM_MAGIC;

    /* Materialize the fixture into a W^X page (Pin JITs it like any app code). */
    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("pin_trace_workload: mmap code");
        return 2;
    }
    memcpy(code, ROUTINE, sizeof ROUTINE);
    if (mprotect(code, 4096, PROT_READ | PROT_EXEC) != 0) {
        perror("pin_trace_workload: mprotect");
        return 2;
    }

    /* ORDERING (the tool relies on this — see asmtest_pintool.cpp on_begin): the
     * region's base/len/name are published to the channel BEFORE the first marker
     * call, and the region is first EXECUTED only after asmtest_trace_begin
     * returns. So by the time Pin JITs region code, the tool has already read
     * region_base/region_len from the channel. */
    shm->region_base = (uint64_t)(uintptr_t)code;
    shm->region_len = (uint64_t)sizeof ROUTINE;
    strncpy(shm->region_name, "add2", sizeof shm->region_name - 1);

    add2_fn fn = (add2_fn)code;

    asmtest_trace_begin("add2");
    long r1 = fn(3, 4); /* 7 <= 100 -> jle taken, dec skipped -> 7   */
    asmtest_trace_end("add2");

    asmtest_trace_begin("add2");
    long r2 = fn(60, 50); /* 110 > 100 -> fall through -> dec -> 109 */
    asmtest_trace_end("add2");

    shm->result = (int64_t)(r1 * 1000 + r2); /* 7*1000 + 109 = 7109 */
    __atomic_store_n(&shm->done, 1u, __ATOMIC_RELEASE);

    fprintf(stderr,
            "pin_trace_workload: done (result=%lld, insns_total=%llu)\n",
            (long long)shm->result, (unsigned long long)shm->insns_total);
    munmap(code, 4096);
    munmap(shm, sizeof *shm);
    return 0;
}
