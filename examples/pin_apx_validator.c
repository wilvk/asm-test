/*
 * pin_apx_validator.c — the APX-silicon execution halves (T8): the POSITIVE half
 * (Pin traced the APX routine completely) and the NEGATIVE control (the pinned
 * DynamoRIO decoder does NOT cleanly trace the same bytes — the gap that justifies
 * the tier). Runs ONLY on APX silicon: pintool-apx-test gates it behind a CPUID
 * APX_F probe. On non-APX hosts the whole APX execution stage self-skips (the
 * ungated pin_apx_decode assertion still proves the bytes are APX everywhere).
 *
 * The negative control replays APX_ROUTINE under DR in a FORKED CHILD: the pinned
 * DR (11.91.20630) predates APX ([DR #6226] open), so its decoder rejects REX2/EGPR
 * — which can surface as an error return OR a hard #UD/abort while DR translates the
 * block. Forking isolates a crash so the parent turns it into a clean verdict:
 * DR-failed (control passes) vs DR-produced-a-complete-trace (control fails). When
 * DR_VERSION is ever bumped past an APX-capable release, revisit this control.
 */
#include "asmtest_drtrace.h"
#include "asmtest_trace.h"
#include "pin_apx_fixture.h"
#include "pintool_shm.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef long (*add2_fn)(long, long);

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

/* The DR-replay outcome the forked child reports back over a pipe. */
typedef struct {
    int dr_rc;           /* first non-OK DR status, or 0 if all calls OK  */
    unsigned long insns; /* insns_total DR recorded (meaningful if dr_rc==0) */
    int reached_end;     /* 1 if the child completed the replay without dying */
} apx_dr_outcome_t;

/* Child: replay APX_ROUTINE through DR, write the outcome, _exit. Any decoder
 * abort / #UD kills the child before it writes — the parent reads short and
 * treats that as DR-failed. */
static void dr_replay_child(int wfd, const char *client) {
    apx_dr_outcome_t o;
    memset(&o, 0, sizeof o);
    asmtest_drtrace_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.client_path = client;
    opts.mode = ASMTEST_DRTRACE_INSNS;
    int rc = asmtest_dr_init(&opts);
    if (rc != ASMTEST_DR_OK) {
        o.dr_rc = rc;
        goto emit;
    }
    rc = asmtest_dr_start();
    if (rc != ASMTEST_DR_OK) {
        o.dr_rc = rc;
        goto emit;
    }
    asmtest_exec_code_t code;
    rc = asmtest_exec_alloc(APX_ROUTINE, sizeof APX_ROUTINE, &code);
    if (rc != ASMTEST_DR_OK) {
        o.dr_rc = rc;
        goto emit;
    }
    asmtest_trace_t *tr =
        asmtest_trace_new(PIN_SHM_INSNS_CAP, PIN_SHM_BLOCKS_CAP);
    asmtest_dr_register_region(APX_REGION_NAME, code.base, code.len, tr);
    add2_fn fn = (add2_fn)code.base;
    asmtest_trace_begin(APX_REGION_NAME);
    (void)fn(3, 4); /* DR translates the APX block here — decoder may reject */
    asmtest_trace_end(APX_REGION_NAME);
    o.insns = (unsigned long)tr->insns_total;
    o.reached_end = 1;
    asmtest_dr_unregister_region(APX_REGION_NAME);
    asmtest_exec_free(&code);
    asmtest_dr_shutdown();
emit:
    (void)!write(wfd, &o, sizeof o);
    _exit(0);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *name = (argc > 1) ? argv[1] : PIN_SHM_NAME;

    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        printf("# SKIP pin-apx-validator: shm %s absent\n1..0 # skipped\n",
               name);
        return 0;
    }
    asmtest_pin_channel_t *shm = (asmtest_pin_channel_t *)mmap(
        NULL, sizeof(asmtest_pin_channel_t), PROT_READ | PROT_WRITE, MAP_SHARED,
        fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        printf("# SKIP pin-apx-validator: mmap failed\n1..0 # skipped\n");
        return 0;
    }

    for (int i = 0;
         i < 1000 && __atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 0; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    /* --- POSITIVE half: Pin traced the APX routine completely --- */
    CHECK(__atomic_load_n(&shm->done, __ATOMIC_ACQUIRE) == 1,
          "apx workload finished under pin");
    CHECK(shm->result == 7, "apx fixture returned 7 (r16 = 3 + 4)");
    CHECK(shm->truncated == 0, "apx trace not truncated");
    CHECK(shm->insns_total == APX_INSN_COUNT,
          "pin recorded a COMPLETE apx trace (insns_total == 4)");

    /* --- NEGATIVE control: DR does NOT cleanly trace the same APX bytes --- */
    const char *drclient = getenv("ASMTEST_DRCLIENT");
    if (drclient == NULL) {
        printf("# SKIP apx dr-negative-control: DynamoRIO not configured\n");
    } else {
        int pfd[2];
        if (pipe(pfd) != 0) {
            printf("# SKIP apx dr-negative-control: pipe failed\n");
        } else {
            pid_t pid = fork();
            if (pid == 0) {
                close(pfd[0]);
                dr_replay_child(pfd[1], drclient);
                _exit(0); /* not reached */
            }
            close(pfd[1]);
            apx_dr_outcome_t o;
            memset(&o, 0, sizeof o);
            ssize_t got = read(pfd[0], &o, sizeof o);
            close(pfd[0]);
            int status = 0;
            waitpid(pid, &status, 0);

            int dr_crashed = (got != (ssize_t)sizeof o) || !WIFEXITED(status) ||
                             !o.reached_end;
            int dr_errored = (got == (ssize_t)sizeof o) && o.dr_rc != 0;
            int dr_incomplete = (got == (ssize_t)sizeof o) && o.reached_end &&
                                o.dr_rc == 0 && o.insns < APX_INSN_COUNT;
            int dr_complete = (got == (ssize_t)sizeof o) && o.reached_end &&
                              o.dr_rc == 0 && o.insns >= APX_INSN_COUNT;

            if (dr_crashed)
                printf(
                    "# apx dr-negative: DR aborted translating the APX block "
                    "(child died / short read)\n");
            else if (dr_errored)
                printf("# apx dr-negative: DR returned error %d on the APX "
                       "block\n",
                       o.dr_rc);
            else if (dr_incomplete)
                printf("# apx dr-negative: DR trace incomplete (%lu of %d "
                       "insns)\n",
                       o.insns, APX_INSN_COUNT);
            else
                printf("# apx dr-negative: DR produced a COMPLETE trace (%lu "
                       "insns) — DR now decodes APX; revisit DR #6226\n",
                       o.insns);

            CHECK(!dr_complete,
                  "DynamoRIO does NOT cleanly trace the APX routine (negative "
                  "control)");
        }
    }

    munmap(shm, sizeof(asmtest_pin_channel_t));
    shm_unlink(name);

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
