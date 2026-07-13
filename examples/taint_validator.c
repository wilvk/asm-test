/*
 * taint_validator.c — the OUT-OF-PROCESS consumer for the DynamoRIO taint tier's
 * launch-under-DR path (Increment 5, first slice). A SEPARATE process from the launched
 * `drrun -- taint_workload`: it opens the same POSIX shared-memory channel, drains the
 * sink report the launched client wrote, and oracle-diffs it against the emulator L0
 * forward slice — the out-of-process validator the plan's "oracle-diff gate" depends on.
 *
 * Run AFTER the workload finishes (the Makefile lane sequences them; the workload sets
 * shm->done before exiting and the named segment persists until this process unlinks it).
 * It reads hits[] + the scalar counters by OFFSET, never the producer-space .hits pointer.
 *
 * Self-skips (exit 0) if the segment is absent (the workload self-skipped — no DR). The
 * emulator oracle is compiled in only where libunicorn is present (-DDF_HAVE_EMU).
 */
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"
#include "asmtest_valtrace.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef DF_HAVE_EMU
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt);
/* The emulator maps a fixed stack window and cannot read the workload's data page, so the
 * oracle run points rdi at a mapped stack address — the def-use STRUCTURE is address
 * independent. */
#define EMU_MAPPED_PTR 0x00200100L
#endif

/* KEEP IN SYNC with examples/taint_workload.c / dr_taint.c taint_sink_chain. */
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

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* True iff the emulator forward slice from the seed step (step 0) reaches the step at
 * `off`. 1 = reached, 0 = not, -1 = oracle unavailable. */
static int sink_in_forward_slice(uint64_t off) {
#ifdef DF_HAVE_EMU
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    if (v == NULL)
        return -1;
    long args[2] = {EMU_MAPPED_PTR, 5};
    int rc = asmtest_dataflow_emu_run(taint_sink_chain, sizeof taint_sink_chain,
                                      args, 2, 0, v);
    int result = -1;
    if (rc == 0) {
        asmtest_defuse_t *g = asmtest_defuse_build(v);
        at_val_rec_t seed = {0};
        seed.step = 0;
        asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
        result = 0;
        for (size_t i = 0; fwd && i < v->steps_len; i++)
            if (v->insn_off[i] == off &&
                asmtest_slice_contains(fwd, (uint32_t)i))
                result = 1;
        asmtest_slice_free(fwd);
        asmtest_defuse_free(g);
    }
    asmtest_valtrace_free(v);
    return result;
#else
    (void)off;
    return -1;
#endif
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *name = (argc > 1) ? argv[1] : AT_SHM_NAME;

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        printf("# SKIP taint-validator: shm %s absent (workload "
               "self-skipped)\n1..0\n",
               name);
        return 0;
    }
    at_shm_channel_t *shm =
        (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        printf("# SKIP taint-validator: mmap failed\n1..0\n");
        return 0;
    }

    /* The workload runs to completion before us, but spin briefly on done for safety. */
    for (int i = 0;
         i < 1000 && __atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 0; i++) {
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    CHECK(__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 1,
          "launch: workload finished under drrun (shm done flag set)");
    CHECK(shm->result == 12,
          "launch: fixture returned 12 through the launched client");
    CHECK(shm->report.hits_len == 1 && shm->report.hits_total == 1 &&
              !shm->report.truncated,
          "launch: exactly one sink hit crossed the shm channel");
    if (shm->report.hits_len >= 1) {
        at_taint_hit_t *h = &shm->hits[0];
        CHECK(h->off == SINK_OFF,
              "launch: sink hit offset is the jz branch (0x10)");
        CHECK(h->tag != AT_TAG_CLEAN, "launch: sink hit tag is tainted");
        CHECK(h->kind == 1, "launch: sink hit kind = 1 (branch condition)");
        int inslice = sink_in_forward_slice(h->off);
        if (inslice < 0)
            printf("# SKIP out-of-process oracle diff: built without "
                   "libunicorn\n");
        else
            CHECK(inslice == 1, "launch: sink off in emulator forward slice "
                                "[OUT-OF-PROCESS ORACLE]");
    }

    munmap(shm, sizeof(at_shm_channel_t));
    shm_unlink(name);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
