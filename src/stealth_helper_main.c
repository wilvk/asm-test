/*
 * stealth_helper_main.c — the standalone `asmtest-stealth-helper` binary. Spawned
 * (fork + execv) by asmtest_hwtrace_stealth_trace on a host with no hardware trace,
 * it reverse-attaches to its parent (the caller) and single-steps the scoped region
 * out of band, writing the reconstructed trace into a shared memfd the parent maps.
 * This is the "bundled" form of the §D3 stepper — a real separate process the
 * managed packages can ship — as opposed to the in-process forked-child fallback.
 *
 * Invocation (all argv, so nothing leaks through the environment):
 *   asmtest-stealth-helper <parent-pid> <memfd> <region-base> <region-len>
 * The memfd (inherited across execv, sized by the parent) carries the shared
 * asmtest_stealth_scratch_t; region-base is an address in the PARENT's space.
 * See src/stealth_helper.h and docs/plans/scoped-tracing-managed-plan.md §D3.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "stealth_helper.h"

int main(int argc, char **argv) {
    if (argc < 5)
        return 2;
    pid_t parent = (pid_t)strtol(argv[1], NULL, 10);
    int fd = (int)strtol(argv[2], NULL, 10);
    const void *base = (const void *)(uintptr_t)strtoull(argv[3], NULL, 0);
    size_t len = (size_t)strtoull(argv[4], NULL, 0);

    struct stat sb;
    if (fstat(fd, &sb) != 0 ||
        sb.st_size < (off_t)sizeof(asmtest_stealth_scratch_t))
        return 2;
    void *m = mmap(NULL, (size_t)sb.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
    if (m == MAP_FAILED)
        return 2;

    asmtest_stealth_scratch_t *sc = (asmtest_stealth_scratch_t *)m;
    asmtest_stealth_helper_run(sc, parent, base, len);
    munmap(m, (size_t)sb.st_size);
    return 0;
}
