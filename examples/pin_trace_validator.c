/*
 * pin_trace_validator.c — the OUT-OF-PROCESS consumer for the XED-decoded Pin
 * trace tier (PIN-2). A SEPARATE process from the launched `pin -- pin_trace_workload`
 * (run after `pin` exits): it opens the same POSIX shm channel, asserts the exact
 * expected instruction/block offsets for the shared parity ROUTINE, and then proves
 * byte-for-byte offset parity against the in-process SINGLE-STEP backend (always
 * available on x86-64 Linux) and — when ASMTEST_DRCLIENT is set (T7) — the
 * DynamoRIO backend. The three-way byte-identity (Pin ≡ DR ≡ single-step) is
 * PIN-2's exit criterion.
 *
 * Self-skips (exit 0) if the segment is absent (the workload self-skipped).
 */
#include "asmtest_hwtrace.h"
#include "asmtest_trace.h"
#include "pintool_shm.h"

#ifdef PIN_VALIDATOR_DR
#include "asmtest_drtrace.h"
#endif

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* KEEP IN SYNC with examples/test_drtrace.c ROUTINE / examples/pin_trace_workload.c
 * (the shared parity fixture). */
static const unsigned char ROUTINE[] = {
    0x48, 0x89, 0xf8,                   /* mov rax, rdi          (off 0)  */
    0x48, 0x01, 0xf0,                   /* add rax, rsi                   */
    0x48, 0x3d, 0x64, 0x00, 0x00, 0x00, /* cmp rax, 100                   */
    0x7e, 0x03,                         /* jle +3 -> ret (off 0xc)        */
    0x48, 0xff, 0xc8,                   /* dec rax               (off 0xe)*/
    0xc3                                /* ret                   (off 0x11)*/
};
typedef long (*add2_fn)(long, long);

/* Expected offsets, derived from the ROUTINE disassembly for the two calls the
 * workload makes — fn(3,4) [7 <= 100, jle taken, dec skipped] then fn(60,50)
 * [110 > 100, jle not taken, dec runs]:
 *   call 1 insns: 0x0,0x3,0x6,0xc,0x11   (jle taken -> ret)
 *   call 2 insns: 0x0,0x3,0x6,0xc,0xe,0x11 (fall through -> dec -> ret)
 * blocks are the DISTINCT heads in first-entry order: 0x0 (entry), 0x11 (a head
 * only when entered as the jle TARGET in call 1), 0xe (the not-taken fall-through
 * head in call 2). In call 2, 0x11 continues the 0xe block (sequential after dec),
 * so it is NOT re-counted as a head. => blocks_len 3, blocks_total 4. */
static const uint64_t EXPECT_INSNS[] = {0x0, 0x3, 0x6, 0xc, 0x11, 0x0,
                                        0x3, 0x6, 0xc, 0xe, 0x11};
enum { EXPECT_INSNS_N = 11 };
static const uint64_t EXPECT_BLOCKS[] = {0x0, 0x11, 0xe};
enum { EXPECT_BLOCKS_N = 3 };

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* Element-for-element diff of two offset arrays; returns 1 on exact match. */
static int diff_offsets(const uint64_t *a, size_t na, const uint64_t *b,
                        size_t nb) {
    if (na != nb)
        return 0;
    for (size_t i = 0; i < na; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* Trace the two parity calls under the in-process single-step backend into a fresh
 * trace. Works on any x86-64 Linux (no PMU/perf/privilege), so it runs in-container
 * unconditionally. Returns the trace (caller frees) or NULL on init failure. */
static asmtest_trace_t *single_step_trace(void) {
    void *base = 0;
    size_t len = 0;
    if (asmtest_hwtrace_exec_alloc(ROUTINE, sizeof ROUTINE, &base, &len) !=
        ASMTEST_HW_OK)
        return 0;

    asmtest_hwtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.struct_size = sizeof opts;
    opts.backend = ASMTEST_HWTRACE_SINGLESTEP;
    if (asmtest_hwtrace_init(&opts) != ASMTEST_HW_OK) {
        asmtest_hwtrace_exec_free(base, len);
        return 0;
    }
    asmtest_trace_t *tr =
        asmtest_trace_new(PIN_SHM_INSNS_CAP, PIN_SHM_BLOCKS_CAP);
    asmtest_hwtrace_register_region("pinparity", base, len, tr);
    add2_fn fn = (add2_fn)base;

    asmtest_hwtrace_begin("pinparity");
    (void)fn(3, 4);
    asmtest_hwtrace_end("pinparity");
    asmtest_hwtrace_begin("pinparity");
    (void)fn(60, 50);
    asmtest_hwtrace_end("pinparity");

    asmtest_hwtrace_shutdown();
    asmtest_hwtrace_exec_free(base, len);
    return tr;
}

#ifdef PIN_VALIDATOR_DR
/* Trace the same two calls through the DynamoRIO backend (T7). Replays the
 * test_drtrace.c flow. Returns the trace (caller frees) or NULL if DR is not
 * configured / init fails. */
static asmtest_trace_t *dynamorio_trace(const char *client) {
    if (!asmtest_dr_available())
        return 0;
    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    opts.mode =
        ASMTEST_DRTRACE_INSNS; /* record the ordered instruction stream */
    if (asmtest_dr_init(&opts) != ASMTEST_DR_OK)
        return 0;
    if (asmtest_dr_start() != ASMTEST_DR_OK) {
        asmtest_dr_shutdown();
        return 0;
    }
    asmtest_exec_code_t code;
    if (asmtest_exec_alloc(ROUTINE, sizeof ROUTINE, &code) != ASMTEST_DR_OK) {
        asmtest_dr_shutdown();
        return 0;
    }
    asmtest_trace_t *tr =
        asmtest_trace_new(PIN_SHM_INSNS_CAP, PIN_SHM_BLOCKS_CAP);
    asmtest_dr_register_region("pindr", code.base, code.len, tr);
    add2_fn fn = (add2_fn)code.base;
    asmtest_trace_begin("pindr");
    (void)fn(3, 4);
    asmtest_trace_end("pindr");
    asmtest_trace_begin("pindr");
    (void)fn(60, 50);
    asmtest_trace_end("pindr");
    asmtest_dr_unregister_region("pindr");
    asmtest_exec_free(&code);
    asmtest_dr_shutdown();
    return tr;
}
#endif

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *name = (argc > 1) ? argv[1] : PIN_SHM_NAME;

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        printf("# SKIP pin-validator: shm %s absent (workload self-skipped)\n"
               "1..0 # skipped\n",
               name);
        return 0;
    }
    asmtest_pin_channel_t *shm = (asmtest_pin_channel_t *)mmap(
        NULL, sizeof(asmtest_pin_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        printf("# SKIP pin-validator: mmap failed\n1..0 # skipped\n");
        return 0;
    }

    /* The workload runs to completion before us, but spin briefly on done. */
    for (int i = 0;
         i < 1000 && __atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 0; i++) {
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }

    /* --- Expected-value assertions (the exact ROUTINE partition) --- */
    CHECK(__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 1,
          "workload finished under pin (shm done flag set)");
    CHECK(shm->result == 7109, "fixture returned 7109 (7*1000 + 109)");
    CHECK(shm->truncated == 0, "trace not truncated");
    CHECK(shm->insns_len == EXPECT_INSNS_N &&
              shm->insns_total == EXPECT_INSNS_N,
          "insns_len == insns_total == 11");
    CHECK(
        diff_offsets(shm->insns, shm->insns_len, EXPECT_INSNS, EXPECT_INSNS_N),
        "pin instruction offsets match the expected ordered stream");
    CHECK(shm->blocks_len == EXPECT_BLOCKS_N && shm->blocks_total == 4,
          "blocks_len == 3, blocks_total == 4");
    CHECK(diff_offsets(shm->blocks, shm->blocks_len, EXPECT_BLOCKS,
                       EXPECT_BLOCKS_N),
          "pin block offsets match the expected distinct-head set");

    /* --- Single-step offset parity (always on, in-container) --- */
    asmtest_trace_t *ss = single_step_trace();
    if (ss == NULL) {
        printf("# SKIP single-step parity: hwtrace single-step init failed\n");
    } else {
        CHECK(
            diff_offsets(shm->insns, shm->insns_len, ss->insns, ss->insns_len),
            "pin/single-step insn offsets byte-identical");
        CHECK(diff_offsets(shm->blocks, shm->blocks_len, ss->blocks,
                           ss->blocks_len),
              "pin/single-step block offsets byte-identical");
        asmtest_trace_free(ss);
    }

    /* --- DynamoRIO offset parity (T7; env-gated) --- */
#ifdef PIN_VALIDATOR_DR
    const char *drclient = getenv("ASMTEST_DRCLIENT");
    if (drclient == NULL) {
        printf("# SKIP dr-parity: DynamoRIO not configured (set "
               "ASMTEST_DRCLIENT)\n");
    } else {
        asmtest_trace_t *dr = dynamorio_trace(drclient);
        if (dr == NULL) {
            printf("# SKIP dr-parity: DynamoRIO init failed (client %s)\n",
                   drclient);
        } else {
            CHECK(diff_offsets(shm->insns, shm->insns_len, dr->insns,
                               dr->insns_len),
                  "pin/DynamoRIO insn offsets byte-identical");
            CHECK(diff_offsets(shm->blocks, shm->blocks_len, dr->blocks,
                               dr->blocks_len),
                  "pin/DynamoRIO block offsets byte-identical");
            asmtest_trace_free(dr);
        }
    }
#else
    printf("# SKIP dr-parity: validator built without the DynamoRIO arm\n");
#endif

    munmap(shm, sizeof(asmtest_pin_channel_t));
    shm_unlink(name);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
