/*
 * pin_taint.c — the fixture DRIVER for the libdft64 differential taint oracle
 * (pin-libdft-taint-oracle.md, T4). Runs the SAME GP/integer-memory fixtures as
 * examples/dr_taint.c (examples/taint_fixtures.h), NATIVELY, under `pin -t oracle.so`, and
 * leaves a completed at_oracle_shm_t channel for the diff (examples/taint_oracle_diff.c).
 *
 * Modes match dr_taint.c's (argv[1]): seeded / negative / sink / sink-negative /
 * heapstore / highbyte / callarg / callarg-negative / memlen / memlen-negative.
 *
 * For each mode it: maps the fixture bytes executable (asmtest_exec_alloc, W^X, as
 * dr_taint does), maps the POSIX shm channel, writes region_base/len, registers the sink
 * report (asmtest_dr_taint_sink_marker) for the sink-family modes, seeds [base,len) via
 * asmtest_dr_taint_seed_marker (unseeded negatives skip the seed), runs the fixture, and
 * sets shm->result + shm->done. Under `pin` the Pintool observes all of this and fills
 * hits[]; run natively (no pin), the channel still completes but hits[] stays empty.
 *
 * SHM OWNERSHIP: the process that CREATES the segment (O_CREAT|O_EXCL) owns it — zeroes it,
 * unlinks at exit, and prints a standalone TAP line. When the diff orchestrator has already
 * created it, this driver ATTACHES (no zero, no unlink, quiet) and just fills the channel.
 * The Pintool attaches too (never creates), so this is robust to the tool's main() running
 * first (it maps the channel lazily, once this driver has created it).
 *
 * Linked -rdynamic with the SAME marker TU the DR harness uses (src/dataflow_dr.c
 * -DASMTEST_TAINT), so the marker PCs the Pintool resolves by name are identical. It runs
 * the fixture natively and never calls asmtest_dr_init, so no DynamoRIO is loaded here.
 */
#include "asmtest_drtrace.h" /* asmtest_exec_alloc / _free, ASMTEST_DR_OK */
#include "asmtest_taint.h"   /* AT_TAG_TAINTED */

#include "asmtest_taint_oracle_shm.h" /* at_oracle_shm_t                    */
#include "taint_fixtures.h"           /* shared fixtures + region metadata */
#include "taint_oracle_modes.h"       /* shared per-mode driver config     */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The app-side seed/sink markers (src/dataflow_dr.c built -DASMTEST_TAINT, linked in). */
void asmtest_dr_taint_seed_marker(void *base, size_t len, unsigned long color);
void asmtest_dr_taint_sink_marker(void *report);

typedef long (*fn6_t)(long, long, long, long, long, long);

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = (argc > 1) ? argv[1] : "seeded";
    const struct at_fixmode *m = at_find_mode(mode);
    if (m == NULL) {
        fprintf(stderr, "pin_taint: unknown mode '%s'\n", mode);
        return 2;
    }

    const char *shm_name = getenv("ASMTEST_ORACLE_SHM");
    if (shm_name == NULL || shm_name[0] == '\0')
        shm_name = AT_ORACLE_SHM_NAME;

    /* Create (own) or attach the shm channel. */
    int owned = 1;
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0 && errno == EEXIST) {
        owned = 0;
        fd = shm_open(shm_name, O_RDWR, 0600);
    }
    if (fd < 0) {
        perror("pin_taint: shm_open");
        return 1;
    }
    if (ftruncate(fd, (off_t)sizeof(at_oracle_shm_t)) != 0) {
        perror("pin_taint: ftruncate");
        close(fd);
        return 1;
    }
    at_oracle_shm_t *shm =
        (at_oracle_shm_t *)mmap(NULL, sizeof(at_oracle_shm_t),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        perror("pin_taint: mmap");
        return 1;
    }
    if (owned)
        memset(shm, 0, sizeof(*shm));

    /* Real host buffers the fixture reads/writes (as dr_taint.c / dr_taint_simd.c
     * allocate): buf_size bytes (8 GP, 16 XMM), seeded from seed_bytes (SIMD pattern) or
     * buf_val (GP scalar). */
    size_t bsz = (m->buf_size > 0) ? (size_t)m->buf_size : sizeof(uint64_t);
    uint8_t *buf = (uint8_t *)malloc(bsz);
    uint8_t *buf2 = (uint8_t *)malloc(bsz);
    if (buf == NULL || buf2 == NULL) {
        fprintf(stderr, "pin_taint: buffer alloc failed\n");
        return 1;
    }
    if (m->seed_bytes != NULL) {
        memcpy(buf, m->seed_bytes, bsz);
    } else {
        uint64_t v = (uint64_t)m->buf_val;
        memcpy(buf, &v, (sizeof v < bsz) ? sizeof v : bsz);
    }
    memset(buf2, 0, bsz);

    long arg0 = (long)(uintptr_t)buf;
    long arg1 = 5;
    if (m->arg1_kind == ARG1_BUF2)
        arg1 = (long)(uintptr_t)buf2;
    else if (m->arg1_kind == ARG1_ZERO)
        arg1 = 0;

    asmtest_exec_code_t exec;
    if (asmtest_exec_alloc(m->code, m->code_len, &exec) != ASMTEST_DR_OK) {
        fprintf(stderr, "pin_taint: exec_alloc failed\n");
        return 1;
    }

    /* Publish the fixture region so the Pintool reports region offsets + bounds sinks. */
    shm->region_base = (uint64_t)(uintptr_t)exec.base;
    shm->region_len = (uint64_t)exec.len;

    /* Register the sink report (sink-family modes) then paint the seed — both are rare
     * marker calls the Pintool resolves by name, off the fixture's hot path. */
    if (m->sink_family)
        asmtest_dr_taint_sink_marker(&shm->report);
    if (m->do_seed)
        asmtest_dr_taint_seed_marker(
            (void *)(uintptr_t)((uint64_t)(uintptr_t)buf +
                                (uint64_t)m->seed_off),
            (size_t)m->seed_len, (unsigned long)AT_TAG_TAINTED);

    long r = ((fn6_t)exec.base)(arg0, arg1, 0, 0, 0, 0);

    shm->result = (int64_t)r;
    __atomic_store_n(&shm->done, 1u, __ATOMIC_RELEASE);

    asmtest_exec_free(&exec);

    int rc = 0;
    if (owned) {
        /* Standalone self-check: the fixture ran under Pin and the channel completed. */
        int checks = 0, failures = 0;
        printf("== pin-taint (%s) ==\n", m->name);
        checks++;
        int ok1 = (shm->done == 1u);
        printf("%s %d - %s: fixture ran under pin, done=1\n",
               ok1 ? "ok" : "not ok", checks, m->name);
        if (!ok1)
            failures++;
        if (m->check_result) {
            checks++;
            int ok2 = (shm->result == (int64_t)m->want_result);
            printf("%s %d - %s: result == %ld\n", ok2 ? "ok" : "not ok", checks,
                   m->name, m->want_result);
            if (!ok2)
                failures++;
        }
        printf("# libdft hits: total=%llu len=%zu truncated=%u\n",
               (unsigned long long)shm->report.hits_total,
               (size_t)shm->report.hits_len, (unsigned)shm->report.truncated);
        for (size_t i = 0; i < shm->report.hits_len && i < AT_ORACLE_HITS_CAP;
             i++)
            printf("#   hit[%zu]: off=0x%llx kind=%u ea=0x%llx tag=0x%x\n", i,
                   (unsigned long long)shm->hits[i].off,
                   (unsigned)shm->hits[i].kind,
                   (unsigned long long)shm->hits[i].ea,
                   (unsigned)shm->hits[i].tag);
        printf("1..%d\n", checks);
        if (failures)
            printf("# %d/%d checks FAILED\n", failures, checks);
        rc = failures ? 1 : 0;
    }

    munmap(shm, sizeof(*shm));
    if (owned)
        shm_unlink(shm_name);
    free(buf);
    free(buf2);
    return rc;
}
