/*
 * taint_managed_validator.c — OUT-OF-PROCESS consumer for the taint tier's MANAGED seed->sink
 * lane (dynamorio-taint-tier-plan.md, Increment 5 exit criterion 3). A SEPARATE process from
 * the launched `drrun -- dotnet taint_managed.dll`: it opens the same POSIX shm channel and
 * drains the synchronous sink report the DR taint client wrote while instrumenting the JIT'd
 * managed HotSeedSink.
 *
 * Unlike taint_validator.c there is NO emulator oracle here: the fixture is REAL JIT'd .NET
 * code, which the L0 emulator (a fixed native-blob replay) cannot reproduce. So the
 * out-of-process check is STRUCTURAL — a seeded run must report >=1 branch-condition hit that
 * is tainted (kind=1), and the negative control (unseeded) must report ZERO hits. The full
 * emulator cross-check on a shared fixture is Increment 9.
 *
 *   argv[1] = "seed" (expect a tainted branch hit) | "noseed" (expect none)
 *   argv[2] = shm name (optional; defaults to AT_SHM_NAME)
 *
 * Self-skips (exit 0) if the segment is absent (the workload self-skipped — no DR / no SDK).
 */
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int want_seed = (argc > 1) ? (strcmp(argv[1], "noseed") != 0) : 1;
    const char *name = (argc > 2) ? argv[2] : AT_SHM_NAME;

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        printf("# SKIP taint-managed-validator: shm %s absent (workload "
               "self-skipped)\n1..0\n",
               name);
        return 0;
    }
    at_shm_channel_t *shm =
        (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        printf("# SKIP taint-managed-validator: mmap failed\n1..0\n");
        return 0;
    }

    for (int i = 0;
         i < 5000 && __atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 0; i++) {
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    CHECK(__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 1,
          "managed: workload finished under drrun (shm done flag set)");

    uint64_t total = shm->report.hits_total;
    if (want_seed) {
        CHECK(total >= 1,
              "managed: seed->sink reported >=1 hit over JIT'd managed code");
        if (shm->report.hits_len >= 1) {
            at_taint_hit_t *h = &shm->hits[0];
            CHECK(h->tag != AT_TAG_CLEAN,
                  "managed: sink hit tag is tainted (seed reached the branch)");
            CHECK(h->kind == 1,
                  "managed: sink hit kind = 1 (branch condition)");
        } else {
            CHECK(0, "managed: at least one hit present in hits[]");
            CHECK(0, "managed: (kind check skipped — no hit)");
        }
    } else {
        CHECK(total == 0,
              "managed: negative control — unseeded run reported ZERO hits");
    }

    munmap(shm, sizeof(at_shm_channel_t));
    shm_unlink(name);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
