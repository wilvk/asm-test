/*
 * taint_multirange_validator.c — the OUT-OF-PROCESS consumer for the DynamoRIO taint
 * tier's multi-range / method-range-scoping slice (Increment 6). A SEPARATE process from
 * the launched `drrun -- taint_multirange`: it opens the same POSIX shared-memory channel,
 * drains the synchronous sink report AND the process-exit-flushed value/taint trace, and
 * oracle-diffs them against the emulator L0 forward slice — the sink hit AND the full taint
 * SET, out of process.
 *
 * The point of THIS validator (vs taint_validator.c) is the multi-range boundary policy:
 * the client instrumented only two disjoint ranges with an un-instrumented gap between
 * them, so it captured only the range A + range B steps (the gap is absent from the trace).
 * The emulator, which replays the WHOLE blob, produces the full forward slice; the diff is
 * therefore over the CAPTURED offsets only, and it must still match — proving the tag
 * carried across the un-instrumented gap through the process-global shadow.
 *
 * Self-skips (exit 0) if the segment is absent. The emulator oracle is compiled in only
 * where libunicorn is present (-DDF_HAVE_EMU).
 */
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"
#include "asmtest_valtrace.h"
#include "taint_multirange_fixture.h"

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

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* Run the emulator on the whole blob and collect the blob-relative OFFSETS of the steps in
 * the forward slice from the seed (step 0). Returns 1 if the oracle ran, 0 if unavailable. */
static int emu_forward_offsets(uint64_t *off_out, int cap, int *n) {
    *n = 0;
#ifdef DF_HAVE_EMU
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    if (v == NULL)
        return 0;
    long args[2] = {EMU_MAPPED_PTR, 5};
    int ok = 0;
    if (asmtest_dataflow_emu_run(taint_multirange_code,
                                 sizeof taint_multirange_code, args, 2, 0,
                                 v) == 0) {
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

static int off_in(const uint64_t *offs, int n, uint64_t off) {
    for (int i = 0; i < n; i++)
        if (offs[i] == off)
            return 1;
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *name = (argc > 1) ? argv[1] : AT_SHM_NAME;

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        printf("# SKIP taint-multirange-validator: shm %s absent (workload "
               "self-skipped)\n1..0\n",
               name);
        return 0;
    }
    at_shm_channel_t *shm =
        (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        printf("# SKIP taint-multirange-validator: mmap failed\n1..0\n");
        return 0;
    }

    for (int i = 0;
         i < 1000 && __atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 0; i++) {
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    CHECK(__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 1,
          "multirange: workload finished under drrun (shm done flag set)");
    CHECK(shm->result == TMR_EXPECT_RESULT,
          "multirange: fixture returned 12 through the launched client");
    CHECK(shm->report.hits_len == 1 && shm->report.hits_total == 1 &&
              !shm->report.truncated,
          "multirange: exactly one sink hit crossed the shm channel");
    CHECK(shm->drval.steps_len == TMR_EXPECT_STEPS,
          "multirange: captured only the two ranges' steps (gap excluded, 7)");

    uint64_t fwd[AT_SHM_STEPS_CAP];
    int nfwd = 0;
    int have_oracle = emu_forward_offsets(fwd, AT_SHM_STEPS_CAP, &nfwd);

    if (shm->report.hits_len >= 1) {
        at_taint_hit_t *h = &shm->hits[0];
        CHECK(h->off == TMR_SINK_OFF,
              "multirange: sink hit offset is the range-B jz branch (0x20)");
        CHECK(h->tag != AT_TAG_CLEAN, "multirange: sink hit tag is tainted");
        CHECK(h->kind == 1, "multirange: sink hit kind = 1 (branch condition)");
        if (!have_oracle)
            printf("# SKIP out-of-process sink oracle: built without "
                   "libunicorn\n");
        else
            CHECK(off_in(fwd, nfwd, h->off),
                  "multirange: sink off in emulator forward slice "
                  "[OUT-OF-PROCESS ORACLE]");
    }

    /* THE OUT-OF-PROCESS TAINT-SET ORACLE ACROSS THE GAP: the launched client's tainted
     * step offsets (captured only inside the two ranges, drained over shm) must equal the
     * emulator forward slice from the seed RESTRICTED to those offsets — the gap steps are
     * simply absent from the client trace, and the tag still reaches range B. */
    if (!have_oracle) {
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
        CHECK(mism == 0,
              "multirange: taint set == emulator forward slice across the "
              "un-instrumented gap [OUT-OF-PROCESS TAINT-SET ORACLE]");
    }

    munmap(shm, sizeof(at_shm_channel_t));
    shm_unlink(name);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
