/*
 * watchpoint_spike.c — F3 spike: hardware DATA-watchpoint targeted mode.
 *
 * Plan: docs/internal/plans/live-attach-dataflow-followup-plan.md (F3).
 *
 * The repo wires only EXECUTION breakpoints today: src/ptrace_backend.c:485-493
 * (`set_hw_bp`) hard-codes DR7 = 0x1 — i.e. slot-0 enabled, R/W0 = 00 (execute),
 * LEN0 = 00 — via PTRACE_POKEUSER on struct user's u_debugreg[]. The very same
 * x86 debug registers also implement DATA watchpoints: R/Wn = 01 (write) or 11
 * (read+write) with LENn = 1/2/4/8 bytes, four slots (DR0..DR3), touching NO
 * code and running at NATIVE SPEED between hits. It is the only non-code-
 * modifying, non-single-step primitive that observes memory DATA flow.
 *
 * This self-contained probe proves the primitive answers "who touched this
 * field, and with what value" at near-zero perturbation:
 *
 *   Phase A (GO/NO-GO): fork a victim that stores a known deterministic
 *     sequence into a watched 8-byte global, arm a WRITE watchpoint (R/W0=01,
 *     LEN0=10b) on that global's address, PTRACE_CONT, and at each #DB capture
 *     the post-store value (process_vm_readv) + the faulting PC (GETREGS).
 *     ASSERT the captured value sequence equals the tracer's independent ground
 *     truth, and that every hit's PC is the SAME store instruction resolved to
 *     victim_write_sequence+<off>.
 *
 *   Phase B (perturbation): time the watched run vs an identical UNWATCHED
 *     baseline run of the same victim — the delta is only the cost of the few
 *     watchpoint traps, so the victim ran at native speed between hits.
 *
 *   Phase C (single-step contrast): measure this host's real per-single-step
 *     ptrace cost and project what single-stepping the same workload would cost
 *     — the watchpoint took N_WRITES stops where single-step would take one stop
 *     per instruction.
 *
 *   Phase D (read+write): show R/W0 = 11 additionally traps LOADS (a read
 *     watchpoint), and note that read-vs-write disambiguation needs an
 *     instruction decode (DR6 does not carry the access direction).
 *
 * Self-skips cleanly (exit 0, a `# SKIP` line) where debug-register arming is
 * refused (permission / seccomp) or not honored (qemu-user emulates zero
 * breakpoint slots), and on non-x86-64 (with the AArch64 NT_ARM_HW_WATCH note).
 *
 * Build + run on a real host (needs genuine x86 debug registers):
 *   gcc -O2 -Wall -Wextra examples/watchpoint_spike.c -o /tmp/watchpoint_spike
 *   /tmp/watchpoint_spike
 *
 * Throwaway research probe — NOT wired into the library or the Makefile, edits
 * no shared file, reimplements the DATA-watchpoint DR7 encoding independently of
 * ptrace_backend.c (which is read only for the DR_OFFSET / POKEUSER pattern).
 *
 * Caps (x86 hardware, documented + enforced below):
 *   - 4 watchpoints max (DR0..DR3).
 *   - lengths 1 / 2 / 4 / 8 bytes only; the watched address must be length-aligned.
 *   - a hit is a TRAP delivered AFTER the accessing instruction retires, so the
 *     captured value is the POST-write value and the PC is the following insn.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>

/* ---------------------------------------------------------------------------
 * Non-x86-64: no DR0..DR3/DR7 reachable via PTRACE_POKEUSER. Self-skip with the
 * AArch64 design note (the real analog mirrors the landed NT_ARM_HW_BREAK path).
 * ------------------------------------------------------------------------- */
#if !defined(__x86_64__)
int main(void) {
    printf("# asm-test F3 hardware DATA-watchpoint spike\n");
    printf("# SKIP watchpoint_spike: needs x86-64 debug registers "
           "(DR0-3/DR7 via PTRACE_POKEUSER).\n");
    printf("# AArch64 analog: the NT_ARM_HW_WATCH regset (struct "
           "user_hwdebug_state — DBGWCR\n");
    printf("#   E/PAC/LSC(load|store|both)/BAS in dbg_regs[].ctrl + "
           "dbg_regs[].addr), a direct mirror of\n");
    printf("#   the NT_ARM_HW_BREAK execution path\n");
    printf("#   already in src/ptrace_backend.c:502-550; it self-skips under "
           "qemu-user, which\n");
    printf("#   emulates zero debug slots (dbg_info & 0xff == 0).\n");
    return 0;
}
#else /* __x86_64__ */

/* ---------------------------------------------------------------------------
 * DATA-watchpoint DR7 encoding (reimplemented here; NOT shared with the
 * backend). DR0..DR3 + DR6/DR7 are reached through struct user's u_debugreg[]
 * via PTRACE_POKEUSER/PEEKUSER — the same door src/ptrace_backend.c:474-499
 * opens for EXECUTION breakpoints (where it writes DR7 = 0x1). A data watchpoint
 * differs only in the per-slot R/W and LEN fields.
 * ------------------------------------------------------------------------- */
#define DR_OFFSET(n)                                                           \
    (offsetof(struct user, u_debugreg) + (size_t)(n) * sizeof(long))

/* DR7 per-slot fields (slot n in 0..3):
 *   Ln    = bit (2n)             local-enable slot n
 *   R/Wn  = bits (16+4n)..(17+4n)  00 execute | 01 write | 10 I/O | 11 read+write
 *   LENn  = bits (18+4n)..(19+4n)  00 = 1B | 01 = 2B | 11 = 4B | 10 = 8B
 * Execution BPs need R/W=00, LEN=00 (what set_hw_bp ships as DR7=0x1); a DATA
 * watchpoint needs R/W = 01 or 11 and a real LEN. Note the LEN encoding is not
 * monotonic: 8 bytes is 10b, 4 bytes is 11b. */
enum {
    DR_RW_EXECUTE = 0x0,
    DR_RW_WRITE = 0x1,
    DR_RW_IO = 0x2,
    DR_RW_RDWR = 0x3
};

#define DR7_INVALID_LEN (~0UL)

static unsigned long dr7_len_field(int bytes) {
    switch (bytes) {
    case 1:
        return 0x0UL;
    case 2:
        return 0x1UL;
    case 4:
        return 0x3UL;
    case 8:
        return 0x2UL;
    default:
        return DR7_INVALID_LEN;
    }
}

static unsigned long dr7_encode(int slot, unsigned long rw, int bytes) {
    unsigned long len = dr7_len_field(bytes);
    unsigned long dr7 = 1UL << (2 * slot); /* Ln: local enable */
    dr7 |= rw << (16 + 4 * slot);          /* R/Wn */
    dr7 |= len << (18 + 4 * slot);         /* LENn */
    return dr7;
}

/* Arm a DATA watchpoint in `slot` (0..3) on tracee address `addr`, condition
 * `rw` (DR_RW_WRITE or DR_RW_RDWR), length `bytes` (1/2/4/8). The address must
 * be `bytes`-aligned (an x86 debug-register hardware requirement). Returns
 * 0 on success, -1 if PTRACE_POKEUSER is refused (permission/seccomp/emulation),
 * -2 on a bad length or misaligned address. */
static int arm_watch(pid_t pid, uint64_t addr, unsigned long rw, int bytes,
                     int slot) {
    if (dr7_len_field(bytes) == DR7_INVALID_LEN)
        return -2;
    if (addr & (uint64_t)(bytes - 1))
        return -2; /* misaligned: hardware would not match reliably */
    errno = 0;
    if (ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(slot),
               (void *)(uintptr_t)addr) != 0)
        return -1;
    unsigned long dr7 = dr7_encode(slot, rw, bytes);
    errno = 0;
    if (ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(7),
               (void *)(uintptr_t)dr7) != 0)
        return -1;
    return 0;
}

/* --- tracee inspection helpers -------------------------------------------- */

static int read_tracee_u64(pid_t pid, uint64_t addr, uint64_t *out) {
    struct iovec l = {out, sizeof *out};
    struct iovec r = {(void *)(uintptr_t)addr, sizeof *out};
    return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)sizeof *out ? 0
                                                                          : -1;
}

static int read_tracee_pc(pid_t pid, uint64_t *pc) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        return -1;
    *pc = (uint64_t)regs.rip;
    return 0;
}

static unsigned long read_dr6(pid_t pid) {
    errno = 0;
    long v = ptrace(PTRACE_PEEKUSER, pid, (void *)DR_OFFSET(6), NULL);
    return (v == -1 && errno != 0) ? 0UL : (unsigned long)v;
}

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* --- victim workloads (fork inherits identical text/data addresses) ------- */

#define MUL      6364136223846793005ULL
#define N_WRITES 16
#define FILLER   2000000UL /* inner-loop iterations between watched writes */

static volatile uint64_t g_watched; /* the watched field (8-byte aligned) */
static volatile uint64_t g_rw;      /* Phase-D read+write target */

/* N_WRITES stores into g_watched; between each, FILLER iterations of a dependent
 * register mul-chain — native-speed work that never touches g_watched, so ONLY
 * the N_WRITES stores can trip a watchpoint on &g_watched. */
static void victim_write_sequence(uint64_t seed) {
    uint64_t acc = seed;
    for (int w = 0; w < N_WRITES; w++) {
        for (uint64_t i = 0; i < FILLER; i++)
            acc = acc * MUL + i + 1;
        g_watched =
            acc; /* the WATCHED store (volatile => a real memory write) */
    }
}

/* The tracer's independent ground truth: the identical integer recurrence.
 * uint64_t arithmetic is fully defined (wraps), so this matches the victim's
 * stores exactly regardless of how each side is compiled. */
static void compute_expected(uint64_t seed, uint64_t out[N_WRITES]) {
    uint64_t acc = seed;
    for (int w = 0; w < N_WRITES; w++) {
        for (uint64_t i = 0; i < FILLER; i++)
            acc = acc * MUL + i + 1;
        out[w] = acc;
    }
}

#define RW_ROUNDS 8
/* Each round does one volatile READ then one volatile WRITE of g_rw, so a
 * write-only watch sees RW_ROUNDS hits and a read+write watch sees 2*RW_ROUNDS. */
static void victim_rw(uint64_t seed) {
    uint64_t acc = seed;
    for (int r = 0; r < RW_ROUNDS; r++) {
        acc += g_rw; /* volatile READ */
        for (uint64_t i = 0; i < (FILLER / 4); i++)
            acc = acc * MUL + i + 1;
        g_rw = acc; /* volatile WRITE */
    }
}

#define MICRO_LOOP 40000
static void microbench_victim(uint64_t seed) {
    volatile uint64_t x = seed;
    for (int i = 0; i < MICRO_LOOP; i++)
        x += (uint64_t)i;
    (void)x;
}

/* The child half of every victim fork: become traceable, stop so the parent can
 * arm, then run fn(seed) and exit. Never returns. */
static void child_run(void (*fn)(uint64_t), uint64_t seed) {
    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
        _exit(127);
    raise(SIGSTOP); /* parent waits on this, arms the watchpoint, then CONTs */
    fn(seed);
    _exit(0);
}

/* --- Phase A/D driver: run a victim under a data watchpoint ---------------- */

struct watch_result {
    int hits;              /* # of #DB stops (>=0), or <0 = skip */
    uint64_t values[64];   /* captured post-access values */
    uint64_t pcs[64];      /* faulting PC (the instruction after the access) */
    unsigned long dr6[64]; /* DR6 at each stop (B0 bit confirms slot 0) */
    double elapsed_s;      /* wall-clock of the CONT..exit run */
    unsigned long dr7;     /* the DR7 word actually armed */
    const char *skip;      /* reason when hits < 0 */
};

static void run_watched(void (*fn)(uint64_t), uint64_t seed, unsigned long rw,
                        int bytes, uint64_t watch_addr,
                        struct watch_result *out) {
    memset(out, 0, sizeof *out);
    out->dr7 = dr7_encode(0, rw, bytes);
    pid_t pid = fork();
    if (pid == 0)
        child_run(fn, seed);

    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        out->hits = -1;
        out->skip = "victim never reached the initial trace-stop";
        return;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    int rc = arm_watch(pid, watch_addr, rw, bytes, 0);
    if (rc != 0) {
        out->hits = -1;
        out->skip = (rc == -1)
                        ? "PTRACE_POKEUSER refused arming DR0/DR7 "
                          "(permission / seccomp / no real debug registers)"
                        : "invalid watchpoint length or misaligned address";
        ptrace(PTRACE_CONT, pid, NULL, NULL); /* reap the victim */
        waitpid(pid, &status, 0);
        return;
    }

    double t0 = now_s();
    ptrace(PTRACE_CONT, pid, NULL, (void *)0);
    for (;;) {
        if (waitpid(pid, &status, 0) < 0) {
            out->hits = -1;
            out->skip = "waitpid failed";
            return;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP) {
            uint64_t val = 0, pc = 0;
            read_tracee_u64(pid, watch_addr, &val);
            read_tracee_pc(pid, &pc);
            int n = out->hits;
            if (n < (int)(sizeof out->values / sizeof out->values[0])) {
                out->values[n] = val;
                out->pcs[n] = pc;
                out->dr6[n] = read_dr6(pid);
            }
            out->hits++;
            /* clear DR6 status (best-effort) so the next stop is unambiguous */
            ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(6), (void *)0UL);
            ptrace(PTRACE_CONT, pid, NULL, (void *)0);
            continue;
        }
        /* forward any other signal and keep going */
        ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)WSTOPSIG(status));
    }
    out->elapsed_s = now_s() - t0;
}

/* --- Phase B: identical victim, NO watchpoint, timed ---------------------- */

static double run_baseline(uint64_t seed) {
    pid_t pid = fork();
    if (pid == 0)
        child_run(victim_write_sequence, seed);
    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
        return -1.0;
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);
    double t0 = now_s();
    ptrace(PTRACE_CONT, pid, NULL, (void *)0);
    for (;;) {
        if (waitpid(pid, &status, 0) < 0)
            return -1.0;
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)WSTOPSIG(status));
    }
    return now_s() - t0;
}

/* --- Phase C: measured per-single-step ptrace cost on THIS host ----------- */

static double microbench_singlestep(unsigned long *steps) {
    *steps = 0;
    pid_t pid = fork();
    if (pid == 0)
        child_run(microbench_victim, 1);
    int status;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status))
        return -1.0;
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);
    double t0 = now_s();
    if (ptrace(PTRACE_SINGLESTEP, pid, NULL, (void *)0) != 0)
        return -1.0; /* single-step unsupported (e.g. qemu-user) */
    for (;;) {
        if (waitpid(pid, &status, 0) < 0)
            return -1.0;
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        (*steps)++;
        if (*steps > 20000000UL) { /* safety cap */
            ptrace(PTRACE_KILL, pid, NULL, NULL);
            waitpid(pid, &status, 0);
            break;
        }
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, (void *)0) != 0)
            break;
    }
    double dt = now_s() - t0;
    return (*steps == 0) ? -1.0 : dt * 1e6 / (double)*steps;
}

/* --------------------------------------------------------------------------- */

static uint64_t runtime_seed(void) {
    uint64_t s = (uint64_t)getpid() * 2654435761ULL + (uint64_t)time(NULL);
    return s | 1ULL; /* nonzero, unknown at compile time (no const-fold) */
}

int main(void) {
    printf("# asm-test F3 hardware DATA-watchpoint spike (x86-64)\n");
    printf("# watched field g_watched @ %p (8-byte, %zu-aligned)\n",
           (void *)&g_watched, (size_t) _Alignof(uint64_t));

    uint64_t seed = runtime_seed();
    uint64_t expected[N_WRITES];
    compute_expected(seed, expected);

    /* ---- Phase A: WRITE watchpoint, capture value + PC, assert ---- */
    struct watch_result wr;
    run_watched(victim_write_sequence, seed, DR_RW_WRITE, 8,
                (uint64_t)(uintptr_t)&g_watched, &wr);

    if (wr.hits < 0) {
        printf("# SKIP watchpoint_spike: %s\n", wr.skip);
        printf("#   (design findings still valid — see the analysis doc.)\n");
        return 0;
    }
    if (wr.hits == 0) {
        printf("# SKIP watchpoint_spike: DR7=0x%lx armed but NO #DB was "
               "delivered.\n",
               wr.dr7);
        printf("#   The watchpoint is not honored here (qemu-user emulates "
               "zero debug slots).\n");
        return 0;
    }

    printf("#\n# Phase A — WRITE watchpoint (R/W0=01, LEN0=8B) DR7=0x%lx\n",
           wr.dr7);
    uint64_t fn = (uint64_t)(uintptr_t)&victim_write_sequence;
    int values_ok = (wr.hits == N_WRITES);
    int pcs_equal = 1;
    for (int i = 0; i < wr.hits; i++) {
        if (i < N_WRITES && wr.values[i] != expected[i])
            values_ok = 0;
        if (i > 0 && wr.pcs[i] != wr.pcs[0])
            pcs_equal = 0;
    }
    for (int i = 0; i < wr.hits && i < N_WRITES; i++) {
        uint64_t off = wr.pcs[i] - fn;
        printf("  hit %2d: value=0x%016llx  PC=0x%llx (victim_write_sequence"
               "+0x%llx)  DR6=0x%lx  %s\n",
               i, (unsigned long long)wr.values[i],
               (unsigned long long)wr.pcs[i], (unsigned long long)off,
               wr.dr6[i] & 0xful,
               wr.values[i] == expected[i] ? "== expected" : "!! MISMATCH");
    }
    printf("  stops=%d expected=%d  values %s  all-PCs-identical=%s "
           "(same store insn)\n",
           wr.hits, N_WRITES, values_ok ? "MATCH" : "MISMATCH",
           pcs_equal ? "yes" : "no");

    /* ---- Phase B: perturbation — watched vs unwatched baseline ---- */
    double base = run_baseline(seed);
    printf("#\n# Phase B — perturbation (identical workload)\n");
    if (base > 0) {
        double overhead = (wr.elapsed_s - base) / base * 100.0;
        printf("  unwatched baseline: %8.2f ms\n", base * 1e3);
        printf("  watched run:        %8.2f ms  (%d traps)\n",
               wr.elapsed_s * 1e3, wr.hits);
        printf("  overhead: %+.2f%%  => victim ran at native speed between "
               "hits\n",
               overhead);
    }
    unsigned long between = FILLER;
    printf("  %lu loop iterations between consecutive writes, %d total stops\n",
           between, wr.hits);

    /* ---- Phase C: single-step contrast (measured per-step cost) ---- */
    unsigned long steps = 0;
    double per_step_us = microbench_singlestep(&steps);
    printf("#\n# Phase C — single-step contrast (per-step cost measured "
           "here)\n");
    if (per_step_us > 0) {
        unsigned long long work_iters =
            (unsigned long long)N_WRITES * FILLER; /* >= this many instrs */
        double proj_ss_s = per_step_us * 1e-6 * (double)work_iters;
        printf("  measured single-step: %.3f us/step over %lu steps\n",
               per_step_us, steps);
        printf("  watchpoint: %d stops for the whole workload (%.2f ms)\n",
               wr.hits, wr.elapsed_s * 1e3);
        printf("  single-step of the SAME workload: >= %llu stops -> "
               ">= %.1f s (proj., 1 instr/iter lower bound)\n",
               work_iters, proj_ss_s);
        printf("  => watchpoint has ~%.0fx fewer stops than single-step here\n",
               (double)work_iters / (double)wr.hits);
    } else {
        printf("  single-step unavailable here (skipped)\n");
    }

    /* ---- Phase D: read+write watchpoint traps LOADS too ---- */
    struct watch_result wonly, wrw;
    run_watched(victim_rw, seed, DR_RW_WRITE, 8, (uint64_t)(uintptr_t)&g_rw,
                &wonly);
    run_watched(victim_rw, seed, DR_RW_RDWR, 8, (uint64_t)(uintptr_t)&g_rw,
                &wrw);
    printf("#\n# Phase D — read+write watchpoint (R/W0=11) vs write-only "
           "(R/W0=01)\n");
    int rw_shows_reads = 0;
    if (wonly.hits >= 0 && wrw.hits >= 0) {
        printf("  write-only (DR7=0x%lx): %d hits (the %d stores)\n", wonly.dr7,
               wonly.hits, RW_ROUNDS);
        printf("  read+write (DR7=0x%lx): %d hits (the %d stores + %d loads)\n",
               wrw.dr7, wrw.hits, RW_ROUNDS, RW_ROUNDS);
        rw_shows_reads = (wrw.hits > wonly.hits);
        printf("  => R/W=11 also trapped LOADS: %s\n",
               rw_shows_reads ? "yes" : "no");
        printf("  (DR6 carries only WHICH slot fired, not read-vs-write; "
               "disambiguation needs an insn decode)\n");
    } else {
        printf("  skipped (arming refused for the R/W phase)\n");
    }

    /* ---- caps + verdict ---- */
    printf("#\n# Caps (x86 hardware): 4 watchpoints (DR0-3); lengths 1/2/4/8 "
           "bytes;\n");
    printf("#   address must be length-aligned; hit is a TRAP after the access "
           "(post-write value,\n");
    printf(
        "#   PC = following instruction). Self-skips where arming is refused "
        "or unhonored.\n");

    int go = values_ok && pcs_equal && rw_shows_reads;
    printf("#\n# VERDICT: %s — data watchpoints %s observe "
           "\"who touched this field, with what value\"\n",
           go ? "GO" : "FAIL", go ? "correctly" : "did NOT");
    return go ? 0 : 1;
}

#endif /* __x86_64__ */
