/*
 * taint_attach_coop.c — the COOPERATIVE-attach native workload for the DynamoRIO ATTACH
 * tier (dynamorio-attach-tier-plan.md, Increment 1, first slice). Unlike the launch-under-DR
 * workload (examples/taint_workload.c, run *under* `drrun`), this program is started as a
 * PLAIN native process — DR is ABSENT — and brings DR + the taint client up ON ITSELF
 * mid-run via the proven `dr_app_*` Application Interface, arms a scoped taint window, then
 * tears DR back down and continues NATIVELY to exit. It de-risks the takeover-of-a-running-
 * process + clean-detach lifecycle on the NON-experimental API before the external
 * (foreign-PID) injector (Increment 2).
 *
 * The lifecycle demonstrated (the headline capability):
 *   native  -> the fixture runs with DR absent (asmtest_dr_under_dynamorio() == 0); NOTHING
 *              is captured (no client is loaded);
 *   attach  -> asmtest_dr_init + asmtest_dr_start (dr_app_setup + dr_app_start) bring DR +
 *              libasmtest_drtaint_client.so up on this live process; under_dynamorio() == 1;
 *   armed   -> the SAME fixture runs, now instrumented in band: the client seeds the buffer,
 *              propagates taint inline, and writes the branch-condition sink hit SYNCHRONOUSLY
 *              into the POSIX-shm report; the value/taint trace is drx_buf-buffered;
 *   detach  -> asmtest_dr_shutdown (dr_app_stop_and_cleanup) removes DR: the client's exit
 *              event flushes the value/taint trace into the shm segment and frees the shadow;
 *              under_dynamorio() == 0 again;
 *   native  -> the fixture runs a THIRD time with DR gone; again NOTHING new is captured
 *              (the report hit-count is unchanged), proving detach left a clean native process.
 *
 * The results channel (asmtest_taint_shm.h) and the out-of-process oracle (taint_validator.c)
 * are reused VERBATIM from the launch tier — attach is a new *producer entry*, not new
 * capture. This program emits its own TAP for the attach-LIFECYCLE assertions (the
 * under_dynamorio() false->true->false transitions + capture-only-while-armed + exit 0); the
 * separate validator then oracle-diffs the captured window out of process.
 *
 * The marker symbols are defined HERE (same symbols/ABI as taint_workload.c / dataflow_dr.c)
 * so the client resolves their PCs via dr_get_proc_address; -rdynamic exports them.
 *
 * Fixture = taint_sink_chain (KEEP IN SYNC with examples/taint_workload.c / dr_taint.c /
 * taint_validator.c): a seeded load -> derive into a flag -> conditional branch (the sink).
 */
#include "asmtest_drtrace.h"
#include "asmtest_taint.h"
#include "asmtest_taint_shm.h"

#include "dataflow_dr.h" /* at_drval_t (the region marker's 3rd arg) */

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* App-side markers the client resolves by PC (same symbols/ABI as taint_workload.c). The
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

/* KEEP IN SYNC with examples/taint_workload.c taint_sink_chain. */
static const uint8_t taint_sink_chain[] = {
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]   (SEED origin)    */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax (spill)          */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8] (reload)         */
    0x48, 0x01, 0xf1,             /* 0x0d add rcx, rsi     (rcx+flags taint)*/
    0x74, 0x03,                   /* 0x10 jz 0x15          (SINK: taint ZF) */
    0x48, 0x89, 0xc8,             /* 0x12 mov rax, rcx                      */
    0xc3,                         /* 0x15 ret                               */
};

typedef long (*fn2_t)(long, long);

/* TAP for the attach-lifecycle assertions (the out-of-process oracle diff is the validator's
 * job). Results are collected and printed AFTER detach, in native mode — never under DR. */
static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : AT_SHM_NAME;

    /* The taint client to bring up on ourselves (env or option); self-skip if absent so
     * the lane degrades cleanly exactly like the launch tier does without DynamoRIO. */
    const char *client = getenv("ASMTEST_DRCLIENT");
    if (client == NULL || client[0] == '\0' || !asmtest_dr_available()) {
        printf("# SKIP taint_attach_coop: no taint client / libdynamorio "
               "(set ASMTEST_DRCLIENT + ASMTEST_DR_LIB)\n1..0\n");
        return 0;
    }

    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        perror("taint_attach_coop: shm_open");
        return 2;
    }
    if (ftruncate(fd, (off_t)sizeof(at_shm_channel_t)) != 0) {
        perror("taint_attach_coop: ftruncate");
        return 2;
    }
    at_shm_channel_t *shm =
        (at_shm_channel_t *)mmap(NULL, sizeof(at_shm_channel_t),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        perror("taint_attach_coop: mmap");
        return 2;
    }
    memset(shm, 0, sizeof *shm);
    shm->report.hits =
        shm->hits; /* producer-space ptr; consumer reads hits[] by offset */
    shm->report.hits_cap = AT_SHM_HITS_CAP;
    /* Point the value-capture buffer's step + taint arrays into the segment so the client's
     * drx_buf flush (fired by dr_app_stop_and_cleanup's exit event) lands the value trace +
     * taint set where the validator reads them. mem stays NULL — the taint SET oracle needs
     * step offsets + step_taint, not captured memory values. */
    shm->drval.steps = shm->steps;
    shm->drval.steps_cap = AT_SHM_STEPS_CAP;
    shm->drval.step_taint = shm->step_taint;
    shm->drval.step_taint_cap = AT_SHM_STEPS_CAP;

    /* Seeded buffer the fixture loads from (a plain data page; the client paints its shadow
     * via the seed marker, only while armed). */
    uint64_t seedbuf = 7;

    /* Materialize the fixture into real executable host memory (W^X). It runs natively AND
     * under DR from the same address, so DR instruments it like any app code. */
    asmtest_exec_code_t exec;
    if (asmtest_exec_alloc(taint_sink_chain, sizeof taint_sink_chain, &exec) !=
        ASMTEST_DR_OK) {
        fprintf(stderr, "taint_attach_coop: exec_alloc failed\n");
        return 2;
    }

    /* ---- NATIVE phase 1: DR absent, nothing captured ------------------------- */
    int native1_under = asmtest_dr_under_dynamorio();
    long r_native1 = ((fn2_t)exec.base)((long)(uintptr_t)&seedbuf, 5);
    unsigned hits_after_native1 = shm->report.hits_len;

    /* ---- ATTACH: bring DR + the taint client up on this running process ------ */
    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    int init_rc = asmtest_dr_init(&opts);
    int start_rc =
        (init_rc == ASMTEST_DR_OK) ? asmtest_dr_start() : ASMTEST_DR_ESTATE;
    int armed_under = asmtest_dr_under_dynamorio();

    /* ---- ARMED window: the SAME fixture, now instrumented in band ------------ */
    long r_armed = r_native1; /* default if attach failed; keeps result sane */
    if (start_rc == ASMTEST_DR_OK) {
        /* Register the region + sink report, paint the seed, then run. The sink hit is
         * written SYNCHRONOUSLY when the branch executes — present the instant the fixture
         * returns. (Same rare PC-resolved clean calls as the launch workload.) */
        asmtest_dr_valcapture_marker(exec.base, sizeof taint_sink_chain,
                                     &shm->drval);
        asmtest_dr_taint_sink_marker(&shm->report);
        asmtest_dr_taint_seed_marker(&seedbuf, sizeof seedbuf, AT_TAG_TAINTED);
        r_armed = ((fn2_t)exec.base)((long)(uintptr_t)&seedbuf, 5);
    }
    unsigned hits_after_armed = shm->report.hits_len;

    /* ---- DETACH: remove DR; the exit event flushes the value trace + frees shadow -- */
    asmtest_dr_shutdown();
    int detached_under = asmtest_dr_under_dynamorio();

    /* ---- NATIVE phase 2: DR gone; the fixture runs again, nothing new captured -- */
    long r_native2 = ((fn2_t)exec.base)((long)(uintptr_t)&seedbuf, 5);
    unsigned hits_after_native2 = shm->report.hits_len;

    asmtest_exec_free(&exec);

    /* Publish the armed result + done for the out-of-process validator. */
    shm->result = r_armed;
    __atomic_store_n(&shm->done, 1u, __ATOMIC_RELEASE);

    /* ---- Attach-lifecycle TAP (printed native, post-detach) ------------------ */
    CHECK(native1_under == 0, "native before attach: not under DynamoRIO");
    CHECK(hits_after_native1 == 0,
          "native before attach: no capture (zero sink hits)");
    CHECK(init_rc == ASMTEST_DR_OK && start_rc == ASMTEST_DR_OK,
          "attach: dr_app_setup + dr_app_start took DR over cleanly");
    CHECK(armed_under == 1, "attach: under DynamoRIO during the armed window");
    CHECK(hits_after_armed == 1, "armed window: exactly one sink hit captured");
    CHECK(detached_under == 0,
          "detach: not under DynamoRIO after dr_app_stop_and_cleanup");
    CHECK(hits_after_native2 == hits_after_armed,
          "native after detach: no NEW capture (hit-count unchanged)");
    CHECK(r_armed == 12 && r_native1 == 12 && r_native2 == 12,
          "fixture returned 12 natively AND while armed (state intact across "
          "attach/detach)");

    fprintf(stderr,
            "taint_attach_coop: done (armed=%ld native1=%ld native2=%ld "
            "hits=%u steps=%u)\n",
            r_armed, r_native1, r_native2, (unsigned)shm->report.hits_len,
            (unsigned)shm->drval.steps_len);

    munmap(shm, sizeof(at_shm_channel_t));
    /* Leave the segment for the validator (it drains + shm_unlink's). */

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d attach-lifecycle checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
