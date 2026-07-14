/*
 * taint_validator.c — the OUT-OF-PROCESS consumer for the DynamoRIO taint tier's
 * launch-under-DR path (Increment 5, first slice). A SEPARATE process from the launched
 * `drrun -- taint_workload`: it opens the same POSIX shared-memory channel, drains BOTH
 * the synchronous sink report AND the process-exit-flushed value/taint trace the launched
 * client wrote, and oracle-diffs them against the emulator L0 forward slice — the sink hit
 * AND the full taint SET, both OUT OF PROCESS (the "oracle-diff gate" the plan depends on).
 *
 * Run AFTER the workload finishes AND after drrun returns (the Makefile lane sequences
 * them): the sink hits are synchronous, but the value/taint trace is drained by the
 * client's drx_buf flush at process exit, so it is complete only once the launched
 * process has fully exited. It reads hits[]/steps[]/step_taint[] + the scalar counters by
 * OFFSET, never the producer-space pointers.
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

/* Run the emulator on the fixture and collect the REGION OFFSETS of the steps in the
 * forward slice from the seed (step 0) into off_out[0..*n). Returns 1 if the oracle ran,
 * 0 if unavailable (no libunicorn). The fixture is straight-line, so offsets are unique
 * and an offset set faithfully represents the step set. */
static int emu_forward_offsets(uint64_t *off_out, int cap, int *n) {
    *n = 0;
#ifdef DF_HAVE_EMU
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    if (v == NULL)
        return 0;
    long args[2] = {EMU_MAPPED_PTR, 5};
    int ok = 0;
    if (asmtest_dataflow_emu_run(taint_sink_chain, sizeof taint_sink_chain,
                                 args, 2, 0, v) == 0) {
        asmtest_defuse_t *g = asmtest_defuse_build(v);
        at_val_rec_t seed = {0};
        seed.step = 0;
        asmtest_slice_t *fwd = g ? asmtest_slice_forward(g, seed) : NULL;
        if (fwd != NULL) {
            ok = 1;
            for (size_t i = 0; i < v->steps_len && *n < cap; i++)
                if (asmtest_slice_contains(fwd, (uint32_t)i))
                    off_out[(*n)++] = v->insn_off[i];
        }
        asmtest_slice_free(fwd);
        asmtest_defuse_free(g);
    }
    asmtest_valtrace_free(v);
    return ok;
#else
    (void)off_out;
    (void)cap;
    return 0;
#endif
}

/* Membership of `off` in an offset list. */
static int off_in(const uint64_t *offs, int n, uint64_t off) {
    for (int i = 0; i < n; i++)
        if (offs[i] == off)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Flags (any order, alongside the shm name): `prod` — the record-free production client wrote
     * NO value trace and NO step_taint witness, so skip the value-trace + emulator-oracle +
     * taint-SET checks and validate SINK-only (a seed reaching a sink is the end-to-end proof);
     * `noseed` — the negative control, expect ZERO sink hits. Neither flag → the full Increment-5
     * launch validation (backward-compatible). */
    const char *name = AT_SHM_NAME;
    int prod = 0, noseed = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "prod") == 0)
            prod = 1;
        else if (strcmp(argv[i], "noseed") == 0)
            noseed = 1;
        else
            name = argv[i];
    }

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
    if (noseed)
        CHECK(shm->report.hits_total == 0 && shm->report.hits_len == 0 &&
                  !shm->report.truncated,
              "prod-negative: unseeded => ZERO sink hits (no phantom taint)");
    else
        CHECK(shm->report.hits_len == 1 && shm->report.hits_total == 1 &&
                  !shm->report.truncated,
              "launch: exactly one sink hit crossed the shm channel");
    /* The value / taint trace is drained by the client's drx_buf flush at process exit; it is
     * complete now (we run AFTER drrun returned). PRODUCTION (`prod`) writes no value trace, so
     * skip this — the sink checks below are the record-free path's gate. */
    if (!prod)
        CHECK(shm->drval.steps_len == 7,
              "launch: value trace drained via drx_buf process-exit flush (7 "
              "steps)");

    uint64_t fwd[AT_SHM_STEPS_CAP];
    int nfwd = 0;
    int have_oracle = emu_forward_offsets(fwd, AT_SHM_STEPS_CAP, &nfwd);

    if (shm->report.hits_len >= 1) {
        at_taint_hit_t *h = &shm->hits[0];
        CHECK(h->off == SINK_OFF,
              "launch: sink hit offset is the jz branch (0x10)");
        CHECK(h->tag != AT_TAG_CLEAN, "launch: sink hit tag is tainted");
        CHECK(h->kind == 1, "launch: sink hit kind = 1 (branch condition)");
        /* prod: no value trace => no emu forward slice to place the sink off in — sink-only. */
        if (prod)
            printf("# prod: sink off/tag/kind are the record-free path's proof "
                   "(no value-trace oracle)\n");
        else if (!have_oracle)
            printf("# SKIP out-of-process sink oracle: built without "
                   "libunicorn\n");
        else
            CHECK(off_in(fwd, nfwd, h->off),
                  "launch: sink off in emulator forward slice [OUT-OF-PROCESS "
                  "ORACLE]");
    }

    /* THE OUT-OF-PROCESS TAINT-SET ORACLE: the launched client's tainted step offsets
     * (drained over shm) must equal the emulator forward slice from the seed. PRODUCTION
     * (`prod`) writes no step_taint witness, so there is no taint SET to diff — skip. */
    if (prod) {
        printf("# prod: no taint-SET oracle (record-free client writes no "
               "witness) — the sink is the gate\n");
    } else if (!have_oracle) {
        printf("# SKIP out-of-process taint-set oracle: built without "
               "libunicorn\n");
    } else {
        size_t n = shm->drval.steps_len;
        int mism = (n == 0 || n > AT_SHM_STEPS_CAP) ? 1 : 0;
        for (size_t i = 0; i < n && i < AT_SHM_STEPS_CAP; i++) {
            int tainted = shm->step_taint[i] != 0;
            int inslice = off_in(fwd, nfwd, shm->steps[i].off);
            if (tainted != inslice)
                mism++;
        }
        CHECK(mism == 0, "launch: taint set == emulator forward slice "
                         "[OUT-OF-PROCESS TAINT-SET ORACLE]");
    }

    munmap(shm, sizeof(at_shm_channel_t));
    shm_unlink(name);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
