/*
 * taint_oracle_diff.c — the DIFFERENTIAL DIFF (pin-libdft-taint-oracle.md, T5). For ONE
 * fixture mode (argv[1]) it obtains BOTH sink reports — the DR one from
 * asmtest_dataflow_dr_taint_run() (in-process, one DynamoRIO lifecycle) and the libdft64
 * one drained from the at_oracle_shm_t channel after running `pin -t oracle.so -- pin_taint
 * <mode>` — and asserts the two sink sets AGREE byte-for-byte on the covered GP/integer
 * subset, classifying every divergence.
 *
 * ONE MODE PER PROCESS: DynamoRIO permits a single in-process lifecycle, so — exactly like
 * dr_taint.c — each invocation drives one scenario; the dr-taint-oracle-test make lane loops
 * over the modes. The libdft side (Pin) is forked FIRST, before the in-process DR lifecycle,
 * so Pin runs in a clean process.
 *
 * Divergence classes (the point of the oracle):
 *   - DR-client bug     — the two engines disagree on an operand class BOTH cover (GP reg,
 *                         integer memory). Fails the lane RED.
 *   - libdft coverage gap — the divergence lands in a class libdft documents as unsupported
 *                         (eflags, SIMD, ...). A named skip (T6), never a blanket pass.
 *   - modelling difference — a legitimate calling-convention/attribution boundary; recorded.
 * For the GP/integer subset exercised here the two engines are EXPECTED to agree, so any
 * divergence in a covered mode is reported RED with both hit lists printed.
 */
#include "asmtest_valtrace.h"

#include "asmtest_taint.h"

#include "taint_fixtures.h"
#include "taint_oracle_modes.h"

#include "asmtest_taint_oracle_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define DF_DR_OK    0
#define DF_DR_ENODR (-4)

int asmtest_dataflow_dr_available(void);
int asmtest_dataflow_dr_taint_run(const uint8_t *code, size_t code_len,
                                  const long *args, int nargs,
                                  uint64_t max_insns, uint64_t seed_base,
                                  uint64_t seed_len, at_tag_t seed_color,
                                  long *result, asmtest_valtrace_t *vt,
                                  at_tag_t *step_taint, size_t step_taint_cap,
                                  at_taint_report_t *report);

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

static long resolve_arg1(const struct at_fixmode *m, uint64_t buf2) {
    if (m->arg1_kind == ARG1_BUF2)
        return (long)(uintptr_t)buf2;
    if (m->arg1_kind == ARG1_ZERO)
        return 0;
    return 5;
}

/* Run the DR side: one DynamoRIO lifecycle, filling `report` (sink-family modes) or leaving
 * it empty (propagation-only modes register no report, exactly as dr_taint.c does). */
static int run_dr(const struct at_fixmode *m, at_taint_report_t *report,
                  at_taint_hit_t *hits, size_t cap, long *result) {
    asmtest_valtrace_t *v = asmtest_valtrace_new(64, 512, 512);
    at_tag_t step_taint[64];
    uint64_t *buf = (uint64_t *)malloc(sizeof(uint64_t));
    uint64_t *buf2 = (uint64_t *)malloc(sizeof(uint64_t));
    if (v == NULL || buf == NULL || buf2 == NULL) {
        free(buf);
        free(buf2);
        asmtest_valtrace_free(v);
        return DF_DR_ENODR;
    }
    *buf = (uint64_t)m->buf_val;
    *buf2 = 0;
    long args[2] = {(long)(uintptr_t)buf,
                    resolve_arg1(m, (uint64_t)(uintptr_t)buf2)};
    memset(report, 0, sizeof *report);
    report->hits = hits;
    report->hits_cap = cap;
    uint64_t seed_base =
        m->do_seed ? ((uint64_t)(uintptr_t)buf + (uint64_t)m->seed_off) : 0;
    uint64_t seed_len = m->do_seed ? (uint64_t)m->seed_len : 0;
    at_taint_report_t *rp = m->sink_family ? report : NULL;
    int rc = asmtest_dataflow_dr_taint_run(m->code, m->code_len, args, 2, 0,
                                           seed_base, seed_len, AT_TAG_TAINTED,
                                           result, v, step_taint, 64, rp);
    asmtest_valtrace_free(v);
    free(buf);
    free(buf2);
    return rc;
}

/* Fork `pin -t oracle.so -shm <name> -- pin_taint <mode>`; pin_taint attaches the
 * already-created channel and fills hits[]. Returns 0 on a clean child exit. */
static int run_libdft(const char *mode, const char *shm_name) {
    const char *pin = getenv("ASMTEST_PIN");
    const char *oracle_so = getenv("ASMTEST_ORACLE_SO");
    const char *pin_taint = getenv("ASMTEST_PIN_TAINT");
    if (pin == NULL || oracle_so == NULL || pin_taint == NULL)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        setenv("ASMTEST_ORACLE_SHM", shm_name, 1);
        execl(pin, pin, "-t", oracle_so, "-shm", shm_name, "--", pin_taint,
              mode, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Multiset equality on the CLIENT-filled fields {off, ea, kind} + tainted-bit compat. */
static int hits_multiset_equal(const at_taint_hit_t *a, size_t na,
                               const at_taint_hit_t *b, size_t nb) {
    if (na != nb)
        return 0;
    int used[AT_ORACLE_HITS_CAP];
    for (size_t j = 0; j < nb && j < AT_ORACLE_HITS_CAP; j++)
        used[j] = 0;
    for (size_t i = 0; i < na; i++) {
        int matched = 0;
        for (size_t j = 0; j < nb; j++) {
            if (used[j])
                continue;
            int tainted_a = (a[i].tag != AT_TAG_CLEAN);
            int tainted_b = (b[j].tag != AT_TAG_CLEAN);
            if (a[i].off == b[j].off && a[i].ea == b[j].ea &&
                a[i].kind == b[j].kind && tainted_a == tainted_b) {
                used[j] = 1;
                matched = 1;
                break;
            }
        }
        if (!matched)
            return 0;
    }
    return 1;
}

static void dump_hits(const char *tag, const at_taint_hit_t *h, size_t n) {
    printf("#   %s: %zu hit(s)\n", tag, n);
    for (size_t i = 0; i < n; i++)
        printf("#     off=0x%llx ea=0x%llx kind=%u tag=0x%x\n",
               (unsigned long long)h[i].off, (unsigned long long)h[i].ea,
               (unsigned)h[i].kind, (unsigned)h[i].tag);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (!asmtest_dataflow_dr_available()) {
        printf("# SKIP dr-taint-oracle: DynamoRIO / taint client unavailable\n"
               "1..0\n");
        return 0;
    }
    const char *mode = (argc > 1) ? argv[1] : "seeded";
    const struct at_fixmode *m = at_find_mode(mode);
    if (m == NULL) {
        fprintf(stderr, "taint_oracle_diff: unknown mode '%s'\n", mode);
        return 2;
    }
    printf("== dr-taint-oracle-test (%s: DR vs libdft64 sink agreement) ==\n",
           m->name);

    /* --- libdft side (Pin), forked before the in-process DR lifecycle --- */
    char shm_name[96];
    snprintf(shm_name, sizeof shm_name, "%s_%d", AT_ORACLE_SHM_NAME,
             (int)getpid());
    shm_unlink(shm_name); /* clear any stale segment */
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        perror("taint_oracle_diff: shm_open");
        return 1;
    }
    if (ftruncate(fd, (off_t)sizeof(at_oracle_shm_t)) != 0) {
        perror("taint_oracle_diff: ftruncate");
        close(fd);
        shm_unlink(shm_name);
        return 1;
    }
    at_oracle_shm_t *shm =
        (at_oracle_shm_t *)mmap(NULL, sizeof(at_oracle_shm_t),
                                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) {
        perror("taint_oracle_diff: mmap");
        shm_unlink(shm_name);
        return 1;
    }
    memset(shm, 0, sizeof(*shm));

    int libdft_rc = run_libdft(m->name, shm_name);

    /* --- DR side (in-process, one lifecycle) --- */
    at_taint_hit_t dr_hits[AT_ORACLE_HITS_CAP];
    at_taint_report_t dr_report;
    long dr_result = 0;
    int dr_rc = run_dr(m, &dr_report, dr_hits, AT_ORACLE_HITS_CAP, &dr_result);

    /* --- assertions --- */
    CHECK(dr_rc == DF_DR_OK, "DR taint run captured in-band");
    CHECK(libdft_rc == 0, "libdft run under pin exited cleanly");
    CHECK(shm->done == 1u, "libdft channel completed (done=1)");

    size_t dn = dr_report.hits_len;
    size_t ln = (size_t)shm->report.hits_len;

    if (m->expect_hits) {
        int dr_ok = (dn == 1 && dr_report.hits[0].off == m->hit_off &&
                     dr_report.hits[0].kind == m->hit_kind &&
                     dr_report.hits[0].tag != AT_TAG_CLEAN);
        CHECK(dr_ok, "DR reports the expected sink hit (off/kind/tainted)");
        int lb_ok = (ln == 1 && shm->hits[0].off == m->hit_off &&
                     shm->hits[0].kind == m->hit_kind &&
                     shm->hits[0].tag != AT_TAG_CLEAN);
        CHECK(lb_ok, "libdft reports the expected sink hit (off/kind/tainted)");
    } else {
        CHECK(dn == 0, "DR reports no sink hit (propagation-only / negative)");
        CHECK(ln == 0,
              "libdft reports no sink hit (propagation-only / negative)");
    }

    /* THE ORACLE DIFF: the two independent engines' sink sets must be equal. */
    int agree = hits_multiset_equal(dr_report.hits, dn, shm->hits, ln);
    if (!agree) {
        printf("# DIVERGENCE in a COVERED GP/integer mode -> DR-client bug "
               "(RED):\n");
        dump_hits("DR    ", dr_report.hits, dn);
        dump_hits("libdft", shm->hits, ln);
    }
    CHECK(agree, "DR sink set == libdft64 sink set [ORACLE DIFF]");

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);

    munmap(shm, sizeof(*shm));
    shm_unlink(shm_name);
    return failures ? 1 : 0;
}
