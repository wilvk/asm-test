/*
 * pin_probe_validator.c — the OUT-OF-PROCESS consumer + cross-check for the Intel Pin
 * PROBE-MODE argument/return capture lane (PIN-3, pin-probe-mode-capture.md).
 *
 * A SEPARATE process from the launched `pin -probe -- pin_probe_victim` (run after
 * `pin` exits): it maps the same POSIX shm channel, rebuilds the capture by OFFSET,
 * and — in `capture` mode — proves the Pin capture agrees with an INDEPENDENT
 * out-of-process ptrace single-step observation of the same routine (two unrelated
 * producers must match). Usage:
 *
 *   pin_probe_validator <shm> capture <victim-binary>
 *       Assert the Pin capture (RDI=a, RSI=b, XMM0=d, RAX=return, the buffer bytes),
 *       then spawn `<victim-binary> attach-loop` NATIVELY, PTRACE_ATTACH it,
 *       run_to(capref), read the SAME registers, and assert Pin == ptrace on the
 *       process-independent set (RDI, RSI, RAX). The pointer arg (RDX) and the unused
 *       arg regs are process-specific and are NOT cross-asserted for equality.
 *   pin_probe_validator <shm> badptr
 *       Assert the invalid-pointer buffer was REFUSED (zero-length MEM record) — not
 *       faulted, not silently captured.
 *   pin_probe_validator <shm> skip
 *       Assert the un-probeable target produced an explicit AV_SKIP_* reason.
 *
 * Self-skips (exit 0) if the shm segment is absent (the Pin run self-skipped).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "asmtest_ptrace.h"
#include "asmtest_valtrace_shm.h"

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

static const char *skip_reason(uint32_t s) {
    switch (s) {
    case AV_SKIP_NONE:
        return "captured";
    case AV_SKIP_TOO_SHORT:
        return "routine too short for a probe";
    case AV_SKIP_NOT_RELOCATABLE:
        return "routine not relocatable";
    case AV_SKIP_NOT_FOUND:
        return "routine not found";
    default:
        return "unknown";
    }
}

/* Find the first record with kind==AT_LOC_REG and reg==id (or NULL). */
static const at_val_rec_t *find_reg(const av_shm_channel_t *c, uint32_t id,
                                    int is_write) {
    for (uint32_t i = 0; i < c->recs_len; i++) {
        const at_val_rec_t *r = &c->recs[i];
        if (r->kind == AT_LOC_REG && r->reg == id &&
            (int)r->is_write == is_write)
            return r;
    }
    return NULL;
}
static const at_val_rec_t *find_mem(const av_shm_channel_t *c) {
    for (uint32_t i = 0; i < c->recs_len; i++)
        if (c->recs[i].kind == AT_LOC_MEM_ABS)
            return &c->recs[i];
    return NULL;
}

/* Capstone reg id literals (must match the tool / the ptrace id space). */
enum { CS_RAX = 35, CS_RDI = 39, CS_RSI = 43, CS_XMM0 = 122 };

/* ------------------------------------------------------------------------ */
/* Independent ptrace reference producer (capture mode only)                 */
/* ------------------------------------------------------------------------ */

/* Spawn `<victim> attach-loop` as a child, PTRACE_ATTACH it, run_to(capref), read the
 * SysV entry registers, then run_to the return address and read RAX. Fills the
 * rdi/rsi/rax out-params on success. Returns 0 on success, -1 on a hard failure, 1 to
 * self-skip (ptrace unavailable on this host). */
static int ptrace_reference(const char *victim, uint64_t *rdi, uint64_t *rsi,
                            uint64_t *rax) {
    if (!asmtest_ptrace_available()) {
        char why[256];
        asmtest_ptrace_skip_reason(why, sizeof why);
        printf("# SKIP ptrace cross-check: %s\n", why);
        return 1;
    }

    int pfd[2];
    if (pipe(pfd) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* child: victim's stderr -> pipe, then exec the attach-loop role */
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        execl(victim, victim, "attach-loop", (char *)NULL);
        _exit(127);
    }
    close(pfd[1]);

    /* Read the victim's banner: "victim pid=<pid> capref=<addr> ...". */
    char line[256];
    size_t n = 0;
    while (n + 1 < sizeof line) {
        char ch;
        ssize_t r = read(pfd[0], &ch, 1);
        if (r <= 0)
            break;
        line[n++] = ch;
        if (ch == '\n')
            break;
    }
    line[n] = 0;
    close(pfd[0]);
    const char *p = strstr(line, "capref=");
    uint64_t capref = p ? strtoull(p + 7, NULL, 0) : 0;
    if (capref == 0) {
        fprintf(stderr, "# ptrace ref: could not parse capref addr from '%s'\n",
                line);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }

    int rc = -1;
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        perror("# ptrace ref: PTRACE_ATTACH");
        goto done;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        goto detach;

    /* run_to the capref entry: plant a breakpoint at capref and let the loop call
     * in. PC lands exactly at capref. */
    if (asmtest_ptrace_run_to(pid, (const void *)(uintptr_t)capref) !=
        ASMTEST_PTRACE_OK) {
        fprintf(stderr, "# ptrace ref: run_to(capref) failed\n");
        goto detach;
    }
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        goto detach;
    *rdi = (uint64_t)regs.rdi;
    *rsi = (uint64_t)regs.rsi;

    /* The return address sits at [rsp] on entry (the call pushed it). run_to it and
     * read RAX = the return value. */
    errno = 0;
    long retaddr = ptrace(PTRACE_PEEKDATA, pid, (void *)(uintptr_t)regs.rsp, 0);
    if (retaddr == -1 && errno != 0)
        goto detach;
    if (asmtest_ptrace_run_to(pid, (const void *)(uintptr_t)retaddr) !=
        ASMTEST_PTRACE_OK) {
        fprintf(stderr, "# ptrace ref: run_to(return) failed\n");
        goto detach;
    }
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        goto detach;
    *rax = (uint64_t)regs.rax;
    rc = 0;

detach:
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
done:
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return rc;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *shm = (argc > 1) ? argv[1] : AV_SHM_NAME;
    const char *mode = (argc > 2) ? argv[2] : "capture";
    const char *victim = (argc > 3) ? argv[3] : NULL;

    int fd = shm_open(shm, O_RDONLY, 0600);
    if (fd < 0) {
        printf("# SKIP pin-probe: shm %s absent (Pin run self-skipped)\n"
               "1..0 # skipped\n",
               shm);
        return 0;
    }
    av_shm_channel_t *c =
        (av_shm_channel_t *)mmap(NULL, sizeof *c, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (c == MAP_FAILED) {
        printf("# SKIP pin-probe: mmap failed\n1..0 # skipped\n");
        return 0;
    }

    for (int i = 0;
         i < 2000 && __atomic_load_n(&c->done, __ATOMIC_ACQUIRE) == 0; i++) {
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }

    printf("== pin-probe-test [%s] (func=%s) ==\n", mode, c->func);
    CHECK(c->magic == AV_SHM_MAGIC, "shm channel magic valid");
    CHECK(__atomic_load_n(&c->done, __ATOMIC_ACQUIRE) == 1,
          "Pin run finished (done flag set)");

    if (strcmp(mode, "skip") == 0) {
        CHECK(c->skip != AV_SKIP_NONE, "un-probeable target reported a skip");
        CHECK(c->skip == AV_SKIP_TOO_SHORT,
              "skip reason is TOO_SHORT (sub-14-byte routine)");
        printf("# SKIP: %s: %s\n", c->func, skip_reason(c->skip));
    } else if (strcmp(mode, "badptr") == 0) {
        CHECK(c->skip == AV_SKIP_NONE, "capref captured (not skipped)");
        const at_val_rec_t *mem = find_mem(c);
        CHECK(mem != NULL, "a pointed-to-buffer MEM record is present");
        CHECK(mem && mem->size == 0 && !mem->value_valid,
              "invalid pointer REFUSED: zero-length record, not faulted");
    } else { /* capture */
        CHECK(c->skip == AV_SKIP_NONE, "capref captured (not skipped)");
        CHECK(c->truncated == 0, "capture not truncated");

        const at_val_rec_t *rdi = find_reg(c, CS_RDI, 0);
        const at_val_rec_t *rsi = find_reg(c, CS_RSI, 0);
        const at_val_rec_t *xmm0 = find_reg(c, CS_XMM0, 0);
        const at_val_rec_t *rax = find_reg(c, CS_RAX, 1);
        const at_val_rec_t *mem = find_mem(c);

        CHECK(rdi && rdi->value == (uint64_t)AV_CAPREF_A,
              "Pin entry RDI == a (11)");
        CHECK(rsi && rsi->value == (uint64_t)AV_CAPREF_B,
              "Pin entry RSI == b (22)");
        union {
            double d;
            uint64_t u;
        } fp;
        fp.d = AV_CAPREF_D;
        CHECK(xmm0 && xmm0->value == fp.u, "Pin entry XMM0 == d (3.5)");
        CHECK(rax && rax->value == (uint64_t)AV_CAPREF_RET,
              "Pin exit RAX == a+b+(long)d+buf[0] (133)");
        CHECK(mem && mem->size > 0 &&
                  memcmp(&c->wide[mem->wide_off], AV_CAPREF_BUF,
                         strlen(AV_CAPREF_BUF)) == 0,
              "Pin captured buffer bytes match the fixture string");

        /* Independent ptrace cross-check: two unrelated producers must agree. */
        if (victim == NULL) {
            printf("# SKIP ptrace cross-check: no victim binary passed\n");
        } else {
            uint64_t p_rdi = 0, p_rsi = 0, p_rax = 0;
            int rr = ptrace_reference(victim, &p_rdi, &p_rsi, &p_rax);
            if (rr == 1) {
                /* self-skip: ptrace unavailable (recorded above) */
            } else if (rr != 0) {
                CHECK(0, "ptrace reference capture succeeded");
            } else {
                CHECK(rdi && p_rdi == rdi->value,
                      "ptrace RDI == Pin RDI (two producers agree on a)");
                CHECK(rsi && p_rsi == rsi->value,
                      "ptrace RSI == Pin RSI (two producers agree on b)");
                CHECK(rax && p_rax == rax->value,
                      "ptrace RAX == Pin RAX (two producers agree on return)");
                printf("# ptrace: rdi=%llu rsi=%llu rax=%llu  pin: rdi=%llu "
                       "rsi=%llu rax=%llu\n",
                       (unsigned long long)p_rdi, (unsigned long long)p_rsi,
                       (unsigned long long)p_rax,
                       (unsigned long long)(rdi ? rdi->value : 0),
                       (unsigned long long)(rsi ? rsi->value : 0),
                       (unsigned long long)(rax ? rax->value : 0));
            }
        }
    }

    munmap(c, sizeof *c);
    shm_unlink(shm);
    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
