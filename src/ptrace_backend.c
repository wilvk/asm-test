/*
 * ptrace_backend.c — out-of-process single-step native-trace backend (W2).
 * See asmtest_ptrace.h and docs/plans/zen2-singlestep-trace-plan.md (Phase 5, W2).
 *
 * A tracer PARENT PTRACE_SINGLESTEPs a forked tracee that calls the registered code,
 * reading the program counter from the child's register file at each stop. It produces
 * the same exact/complete asmtest_trace_t offsets as the in-process EFLAGS.TF stepper
 * (src/ss_backend.c) — and reuses that backend's single-entry/ends-at-branch block
 * normalization — but collects them entirely out of band, so the tracee's signal
 * disposition and code cache are never touched (the property a JIT/GC managed
 * runtime needs, and — because the AArch64 single-step bit MDSCR_EL1.SS is kernel-only,
 * with no in-process form — the ONLY single-step form available on AArch64).
 *
 * The parent observes every step through ptrace, so no shared memory is needed: it
 * fills the caller-owned trace directly from the register reads.
 *
 * Two arch seams keep the stepper one body across x86-64 and AArch64:
 *   - read_pc_ret(): PC + integer return register. x86-64 has PTRACE_GETREGS; AArch64
 *     does not, so it reads the GP set via PTRACE_GETREGSET/NT_PRSTATUS.
 *   - PTRACE_TRACE_ARCH: the Capstone arch the block-normalizer decodes lengths with.
 * The fork/SIGSTOP/SINGLESTEP/wait control flow and the SysV/AAPCS64 register-arg call
 * are otherwise identical.
 *
 * The OS code-region readers below it (asmtest_proc_* and asmtest_jitdump_find) are
 * pure /proc + perf-file parsing — arch-INDEPENDENT — so they build and run on ANY
 * Linux (they are the resolve-a-foreign-JIT building blocks, useful on AArch64 hosts
 * whether or not the single-step stepper is exercisable there).
 */
#define _GNU_SOURCE

#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ================================================================= */
/* OS code-region readers — arch-independent; any Linux.             */
/* ================================================================= */
#if defined(__linux__)

#include <stdio.h>
#include <stdlib.h>

int asmtest_proc_region_by_addr(pid_t pid, const void *addr, void **base_out,
                                size_t *len_out) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return ASMTEST_PTRACE_ETRACE;

    uint64_t want = (uint64_t)(uintptr_t)addr;
    char *line = NULL;
    size_t cap = 0;
    int rc = ASMTEST_PTRACE_ENOENT;
    while (getline(&line, &cap, f) != -1) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3)
            continue;
        /* perms is "rwxp"; index 2 is the execute bit. */
        if (perms[2] == 'x' && want >= start && want < end) {
            if (base_out)
                *base_out = (void *)(uintptr_t)start;
            if (len_out)
                *len_out = (size_t)(end - start);
            rc = ASMTEST_PTRACE_OK;
            break;
        }
    }
    free(line);
    fclose(f);
    return rc;
}

int asmtest_proc_perfmap_symbol(pid_t pid, const char *name, void **base_out,
                                size_t *len_out) {
    if (name == NULL)
        return ASMTEST_PTRACE_EINVAL;
    char path[64];
    snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return ASMTEST_PTRACE_ENOENT;

    char *line = NULL;
    size_t cap = 0;
    int rc = ASMTEST_PTRACE_ENOENT;
    while (getline(&line, &cap, f) != -1) {
        unsigned long start, size;
        int soff = 0;
        /* "<hex start> <hex size> <symbol...>"; %n yields the symbol's offset. */
        if (sscanf(line, "%lx %lx %n", &start, &size, &soff) < 2 || soff == 0)
            continue;
        char *sym = line + soff;
        size_t sl = strlen(sym);
        while (sl > 0 && (sym[sl - 1] == '\n' || sym[sl - 1] == '\r' ||
                          sym[sl - 1] == ' ' || sym[sl - 1] == '\t'))
            sym[--sl] = '\0';
        if (strcmp(sym, name) == 0) {
            if (base_out)
                *base_out = (void *)(uintptr_t)start;
            if (len_out)
                *len_out = (size_t)size;
            rc = ASMTEST_PTRACE_OK;
            break;
        }
    }
    free(line);
    fclose(f);
    return rc;
}

/* jitdump readers: assemble little-endian, byte-swap when the file is big-endian. */
static uint32_t jd_rd32(const unsigned char *p, int swap) {
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                 (uint32_t)p[3] << 24;
    return swap ? __builtin_bswap32(v) : v;
}
static uint64_t jd_rd64(const unsigned char *p, int swap) {
    uint64_t v = (uint64_t)jd_rd32(p, 0) | (uint64_t)jd_rd32(p + 4, 0) << 32;
    return swap ? __builtin_bswap64(v) : v;
}

#define JITDUMP_MAGIC 0x4A695444u    /* 'JiTD'           */
#define JITDUMP_MAGIC_SW 0x4454694Au /* byte-swapped     */
#define JIT_CODE_LOAD 0

int asmtest_jitdump_find(const char *path, pid_t pid, const char *name,
                         asmtest_jitdump_entry_t *out, uint8_t *bytes_out,
                         size_t bytes_cap, size_t *bytes_len) {
    if (name == NULL)
        return ASMTEST_PTRACE_EINVAL;
    char buf[64];
    const char *p = path;
    if (p == NULL) {
        snprintf(buf, sizeof buf, "/tmp/jit-%d.dump", (int)pid);
        p = buf;
    }
    FILE *f = fopen(p, "rb");
    if (f == NULL)
        return ASMTEST_PTRACE_ENOENT;

    /* File header: magic, version, total_size, elf_mach, pad1, pid, timestamp,
     * flags (40 bytes for v1). */
    unsigned char hdr[40];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fclose(f);
        return ASMTEST_PTRACE_EINVAL;
    }
    uint32_t magic = jd_rd32(hdr, 0);
    int swap;
    if (magic == JITDUMP_MAGIC)
        swap = 0;
    else if (magic == JITDUMP_MAGIC_SW)
        swap = 1;
    else {
        fclose(f);
        return ASMTEST_PTRACE_EINVAL;
    }
    uint32_t header_size = jd_rd32(hdr + 8, swap);
    if (fseek(f, (long)header_size, SEEK_SET) != 0) {
        fclose(f);
        return ASMTEST_PTRACE_EINVAL;
    }

    int rc = ASMTEST_PTRACE_ENOENT;
    /* Records are written in timestamp order, so the LAST matching JIT_CODE_LOAD is
     * the most recent body — just overwrite on each match. */
    for (;;) {
        unsigned char pre[16]; /* id, total_size, timestamp */
        if (fread(pre, 1, sizeof pre, f) != sizeof pre)
            break; /* EOF */
        uint32_t id = jd_rd32(pre, swap);
        uint32_t total = jd_rd32(pre + 4, swap);
        uint64_t ts = jd_rd64(pre + 8, swap);
        if (total < sizeof pre)
            break; /* malformed */

        if (id != JIT_CODE_LOAD) {
            if (fseek(f, (long)(total - sizeof pre), SEEK_CUR) != 0)
                break;
            continue;
        }

        /* jr_code_load body: pid, tid, vma, code_addr, code_size, code_index. */
        unsigned char fx[40];
        if (fread(fx, 1, sizeof fx, f) != sizeof fx)
            break;
        uint64_t code_addr = jd_rd64(fx + 16, swap);
        uint64_t code_size = jd_rd64(fx + 24, swap);
        uint64_t code_index = jd_rd64(fx + 32, swap);
        long name_len = (long)total - 56 - (long)code_size;
        if (name_len <= 0) /* total = 16 prefix + 40 body + name + code */
            break;

        char *nm = (char *)malloc((size_t)name_len);
        if (nm == NULL) {
            fclose(f);
            return ASMTEST_PTRACE_ETRACE;
        }
        if (fread(nm, 1, (size_t)name_len, f) != (size_t)name_len) {
            free(nm);
            break;
        }
        nm[name_len - 1] = '\0';
        int match = (strcmp(nm, name) == 0);
        free(nm);

        if (match) {
            rc = ASMTEST_PTRACE_OK;
            if (out) {
                out->code_addr = code_addr;
                out->code_size = code_size;
                out->timestamp = ts;
                out->code_index = code_index;
            }
            if (bytes_out != NULL && bytes_cap > 0) {
                size_t cpy = code_size < bytes_cap ? (size_t)code_size : bytes_cap;
                if (fread(bytes_out, 1, cpy, f) != cpy)
                    break;
                if (bytes_len)
                    *bytes_len = cpy;
                if (code_size > cpy &&
                    fseek(f, (long)(code_size - cpy), SEEK_CUR) != 0)
                    break;
            } else if (fseek(f, (long)code_size, SEEK_CUR) != 0) {
                break;
            }
        } else if (fseek(f, (long)code_size, SEEK_CUR) != 0) {
            break;
        }
    }
    fclose(f);
    return rc;
}

#else /* readers need Linux /proc + perf files */

int asmtest_proc_region_by_addr(pid_t pid, const void *addr, void **base_out,
                                size_t *len_out) {
    (void)pid;
    (void)addr;
    (void)base_out;
    (void)len_out;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_proc_perfmap_symbol(pid_t pid, const char *name, void **base_out,
                                size_t *len_out) {
    (void)pid;
    (void)name;
    (void)base_out;
    (void)len_out;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_jitdump_find(const char *path, pid_t pid, const char *name,
                         asmtest_jitdump_entry_t *out, uint8_t *bytes_out,
                         size_t bytes_cap, size_t *bytes_len) {
    (void)path;
    (void)pid;
    (void)name;
    (void)out;
    (void)bytes_out;
    (void)bytes_cap;
    (void)bytes_len;
    return ASMTEST_PTRACE_ENOSYS;
}

#endif /* __linux__ readers */

/* ================================================================= */
/* Out-of-process single-step stepper — Linux x86-64 / AArch64.      */
/* ================================================================= */
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

#include <elf.h> /* NT_PRSTATUS */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Ordered in-region PC-offset capture buffer; overflow is flagged truncated, never
 * emitted as complete. Sized for the small-routine envelope, like ss_backend. */
#ifndef PTRACE_STREAM_CAP
#define PTRACE_STREAM_CAP (1u << 16) /* 65536 offsets */
#endif

/* Capstone arch for the in-region instruction-length decode used by block
 * normalization. (Instruction offsets are exact regardless; only block boundaries
 * need the length decode.) */
#if defined(__x86_64__)
#define PTRACE_TRACE_ARCH ASMTEST_ARCH_X86_64
#else
#define PTRACE_TRACE_ARCH ASMTEST_ARCH_ARM64
#endif

/* Software-breakpoint encoding for asmtest_ptrace_run_to. x86 int3 is a one-byte trap
 * whose stop-PC lands one past the byte; AArch64 brk #0 is a four-byte fault whose
 * stop-PC lands AT the instruction. Both are spliced into the low bytes of a peeked
 * word and removed before handing the stop to the stepper. */
#if defined(__x86_64__)
#define PTRACE_BP_INSN 0xccULL       /* int3                 */
#define PTRACE_BP_LEN 1
#else /* __aarch64__ */
#define PTRACE_BP_INSN 0xd4200000ULL /* brk #0 (little-endian) */
#define PTRACE_BP_LEN 4
#endif

/* Read the tracee's program counter (the about-to-execute instruction) and the integer
 * return register. x86-64 has PTRACE_GETREGS; AArch64 does not, so it reads the GP set
 * via PTRACE_GETREGSET/NT_PRSTATUS. Returns 0 on success, -1 on a ptrace failure. */
static int read_pc_ret(pid_t pid, uint64_t *pc, uint64_t *ret) {
    struct user_regs_struct regs;
#if defined(__x86_64__)
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        return -1;
    *pc = (uint64_t)regs.rip;
    if (ret != NULL)
        *ret = (uint64_t)regs.rax;
#else /* __aarch64__ */
    struct iovec iov = {&regs, sizeof regs};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0)
        return -1;
    *pc = (uint64_t)regs.pc;
    if (ret != NULL)
        *ret = (uint64_t)regs.regs[0];
#endif
    return 0;
}

#if defined(__x86_64__)
/* Rewind the tracee's program counter to `pc` (only x86 needs this: int3 is a trap, so
 * the stop-PC is one past the breakpoint byte and must be backed up before resuming).
 * Returns 0 on success, -1 on a ptrace failure. */
static int set_pc(pid_t pid, uint64_t pc) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        return -1;
    regs.rip = pc;
    return ptrace(PTRACE_SETREGS, pid, NULL, &regs);
}
#endif

#if defined(__aarch64__)
/* Hang-proof capability self-probe: the out-of-process stepper needs PTRACE_SINGLESTEP
 * to actually advance a traced child. It does on real AArch64 Linux, but NOT under
 * qemu-user emulation, which does not emulate the ptrace tracer/tracee relationship at
 * all (a blocking waitpid for a PTRACE_TRACEME child never returns there). Every wait
 * here is WNOHANG with a short deadline, so this returns 0 quickly where single-step is
 * unsupported instead of hanging. (x86-64 always has it, so this probe is AArch64-only.)
 */
static int probe_singlestep(void) {
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(0);
    }
    int st, got = 0, stepped = 0;
    for (int i = 0; i < 200; i++) { /* up to ~200 ms for the initial stop */
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid && WIFSTOPPED(st)) {
            got = 1;
            break;
        }
        if (w < 0)
            break;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    if (got && ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) == 0) {
        for (int i = 0; i < 200; i++) {
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) {
                stepped = WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP;
                break;
            }
            if (w < 0)
                break;
            struct timespec ts = {0, 1000000};
            nanosleep(&ts, NULL);
        }
    }
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    return stepped;
}
#endif /* __aarch64__ */

int asmtest_ptrace_available(void) {
#if defined(__x86_64__)
    return 1;
#else /* __aarch64__: require a functional PTRACE_SINGLESTEP (probed once, hang-proof) */
    static int cached = -1;
    if (cached < 0)
        cached = probe_singlestep();
    return cached;
#endif
}

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg =
        asmtest_ptrace_available()
            ? "available"
            : "AArch64 PTRACE_SINGLESTEP is non-functional here (e.g. qemu-user "
              "emulation); the out-of-process stepper needs a real AArch64 host";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

/* Replay the captured ordered offsets into the trace, deriving blocks from
 * fall-through discontinuities — byte-identical to ss_backend.c's ss_normalize. */
static void normalize(asmtest_trace_t *t, const uint8_t *base, uint64_t base_ip,
                      size_t len, const uint64_t *stream, uint32_t n,
                      int overflow) {
    if (t == NULL)
        return;
    int have_prev = 0;
    uint64_t expected_next = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = stream[i];
        if (!have_prev || off != expected_next)
            trace_append_block(t, off);
        trace_append_insn(t, off);
        size_t l = asmtest_disas(PTRACE_TRACE_ARCH, base, len, base_ip, off, NULL, 0);
        if (l == 0) {
            t->truncated = true;
            return;
        }
        expected_next = off + l;
        have_prev = 1;
    }
    if (overflow)
        t->truncated = true;
}

/* Plant a software breakpoint at `target`, PTRACE_CONT the tracee until it reaches it,
 * remove the breakpoint, and (x86) rewind the PC so the tracee is left stopped exactly
 * at `target`. PTRACE_POKETEXT patches even an r-x text page the way a debugger does
 * (process_vm_writev would be refused on it). Splices the breakpoint into the low
 * PTRACE_BP_LEN bytes of the peeked word, preserving the rest. Returns OK (stopped at
 * target), ENOENT (the tracee exited before reaching it), or ETRACE. Shared by
 * asmtest_ptrace_run_to (run an attached target to a resolved method) and the trace
 * loops (run native-speed OVER a call-out to its return address). Unrelated signals are
 * forwarded so the tracee's own signal flow is undisturbed. */
static int run_until(pid_t pid, uint64_t target) {
    errno = 0;
    long orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)(uintptr_t)target, NULL);
    if (orig == -1 && errno != 0)
        return ASMTEST_PTRACE_ETRACE;
    const unsigned long mask = PTRACE_BP_LEN >= (int)sizeof(long)
                                   ? ~0UL
                                   : ((1UL << (PTRACE_BP_LEN * 8)) - 1);
    long planted = (long)(((unsigned long)orig & ~mask) | (PTRACE_BP_INSN & mask));
    if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target, (void *)planted) != 0)
        return ASMTEST_PTRACE_ETRACE;

    int rc = ASMTEST_PTRACE_OK, status = 0, sig = 0;
    for (;;) {
        if (ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)sig) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        sig = 0;
        if (waitpid(pid, &status, 0) < 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            rc = ASMTEST_PTRACE_ENOENT; /* target ended before reaching `target` */
            break;
        }
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            sig = WSTOPSIG(status); /* forward an unrelated signal and keep running */
            continue;
        }
        uint64_t pc;
        if (read_pc_ret(pid, &pc, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
#if defined(__x86_64__)
        uint64_t hit = pc - PTRACE_BP_LEN; /* int3 trap: PC is one past the byte */
#else
        uint64_t hit = pc; /* brk fault: PC is AT the instruction */
#endif
        if (hit != target)
            continue; /* a SIGTRAP that is not our breakpoint; keep going */

        if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target, (void *)orig) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
#if defined(__x86_64__)
        if (set_pc(pid, target) != 0)
            rc = ASMTEST_PTRACE_ETRACE;
#endif
        break;
    }
    if (rc == ASMTEST_PTRACE_ETRACE)
        ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target, (void *)orig);
    return rc;
}

/* How a single-step that LEFT the registered region (after entering it) is classified. */
typedef enum {
    EXIT_RETURNED,         /* the routine's own return (or a tail-jump out) */
    EXIT_CALLOUT_RESUMED,  /* a call-out to a helper, stepped over; recording resumes */
    EXIT_CALLOUT_LOST      /* a call-out whose step-over failed / target exited */
} exit_kind_t;

/* Decide whether a region exit is the routine RETURNING or merely CALLING OUT to a
 * helper outside the region (a runtime helper / GC barrier / PLT stub — the common case
 * for real managed-runtime methods, which the old "first exit == return" model could not
 * trace). If the last in-region instruction (`last_off` in `code`/`len`) is a call and
 * its return lands back in the region, run `pid` at NATIVE SPEED to that return address
 * (no per-instruction step through the callee) and report the in-region resume offset.
 * Needs the Capstone is-call query; without it, every exit reads as a return (leaf-only,
 * the previous behaviour). */
static exit_kind_t classify_region_exit(pid_t pid, const uint8_t *code, size_t len,
                                        uint64_t base_ip, uint64_t last_off,
                                        uint64_t *resume_off) {
    if (!asmtest_disas_available() ||
        !asmtest_disas_is_call(PTRACE_TRACE_ARCH, code, len, last_off))
        return EXIT_RETURNED;
    size_t cl = asmtest_disas(PTRACE_TRACE_ARCH, code, len, base_ip, last_off, NULL, 0);
    uint64_t ret_off = last_off + cl;
    if (cl == 0 || ret_off >= len)
        return EXIT_RETURNED; /* the call's fall-through is outside the region */
    if (run_until(pid, base_ip + ret_off) != ASMTEST_PTRACE_OK)
        return EXIT_CALLOUT_LOST;
    *resume_off = ret_off;
    return EXIT_CALLOUT_RESUMED;
}

typedef long (*fn6_t)(long, long, long, long, long, long);

int asmtest_ptrace_trace_call(const void *code, size_t len, const long *args,
                              int nargs, long *result, asmtest_trace_t *trace) {
    if (code == NULL || len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return ASMTEST_PTRACE_EINVAL;

    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL)
        return ASMTEST_PTRACE_ETRACE;

    pid_t pid = fork();
    if (pid < 0) {
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }

    if (pid == 0) {
        /* Tracee: request tracing, stop so the parent can attach the stepper, then
         * call the registered code (inherited at the same address via fork) with up
         * to six integer args, passed in the SysV / AAPCS64 argument registers. Extra
         * register args are ignored by the callee. _exit avoids running atexit/stdio
         * in the stepped child. */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)code)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }

    /* Tracer parent. */
    const uint64_t base_ip = (uint64_t)(uintptr_t)code;
    uint32_t n = 0;
    int overflow = 0, entered = 0, returned = 0, rc = ASMTEST_PTRACE_OK;
    int status = 0;

    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        /* Could not reach the initial SIGSTOP. */
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (waitpid(pid, &status, 0) < 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break; /* tracee finished */
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            /* The tracee took a real signal (e.g. a faulting routine). Record what we
             * have as truncated and let it die. */
            if (entered)
                overflow = 1; /* incomplete in-region capture */
            break;
        }

        uint64_t pc, retval;
        if (read_pc_ret(pid, &pc, &retval) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }

        if (pc >= base_ip && pc < base_ip + len) {
            entered = 1;
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = pc - base_ip;
            else
                overflow = 1;
        } else if (entered && !returned) {
            /* The step left the region. Distinguish the routine RETURNING from it
             * CALLING OUT to a helper (which we step over at native speed and resume
             * after) — so a routine that calls runtime helpers traces correctly, not
             * just a pure-compute leaf. */
            uint64_t resume_off = 0;
            exit_kind_t k = classify_region_exit(
                pid, (const uint8_t *)code, len, base_ip, stream[n - 1], &resume_off);
            if (k == EXIT_CALLOUT_RESUMED) {
                if (n < PTRACE_STREAM_CAP)
                    stream[n++] = resume_off;
                else
                    overflow = 1;
                continue; /* resume single-stepping from the call's return */
            }
            if (k == EXIT_CALLOUT_LOST) {
                /* The callee never returned to the region (it exited, or a ptrace
                 * error). Keep the partial trace as truncated; we own this forked
                 * tracee, so make sure it is reaped (no-op if it already exited). */
                overflow = 1;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            /* EXIT_RETURNED: the integer return register holds the value. */
            if (result != NULL)
                *result = (long)retval;
            returned = 1;
            ptrace(PTRACE_CONT, pid, NULL, NULL);
            waitpid(pid, &status, 0);
            break;
        }
    }

    if (rc == ASMTEST_PTRACE_OK)
        normalize(trace, (const uint8_t *)code, base_ip, len, stream, n, overflow);
    else {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    free(stream);
    return rc;
}

int asmtest_ptrace_trace_attached(pid_t pid, const void *base, size_t len,
                                  long *result, asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || trace == NULL)
        return ASMTEST_PTRACE_EINVAL;

    /* Read the region bytes FROM THE TARGET (not a shared mapping) so this works on a
     * foreign process — the same way a debugger reads a tracee's text. */
    uint8_t *code = (uint8_t *)malloc(len);
    if (code == NULL)
        return ASMTEST_PTRACE_ETRACE;
    struct iovec liov = {code, len};
    struct iovec riov = {(void *)(uintptr_t)base, len};
    if (process_vm_readv(pid, &liov, 1, &riov, 1, 0) != (ssize_t)len) {
        free(code);
        return ASMTEST_PTRACE_ETRACE;
    }

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL) {
        free(code);
        return ASMTEST_PTRACE_ETRACE;
    }

    const uint64_t base_ip = (uint64_t)(uintptr_t)base;
    uint32_t n = 0;
    int overflow = 0, entered = 0, returned = 0, rc = ASMTEST_PTRACE_OK;
    int status = 0;

    /* The loop below records the PC AFTER each single-step (the next about-to-execute
     * instruction), which captures offset 0 only when the target was stopped BEFORE the
     * region so the entering step lands on it (the resolve+attach path stops in call glue
     * / a spin loop). When the caller instead stopped the target EXACTLY at the region
     * entry — what asmtest_ptrace_run_to leaves — offset 0 is the current PC and must be
     * recorded before the first step, or it is missed. Recording the initial in-region PC
     * here makes both entry conventions yield the identical stream; it is a no-op for a
     * before-the-region start (that initial PC is out of region). */
    {
        uint64_t pc0, ret0;
        if (read_pc_ret(pid, &pc0, &ret0) != 0) {
            free(code);
            free(stream);
            return ASMTEST_PTRACE_ETRACE;
        }
        if (pc0 >= base_ip && pc0 < base_ip + len) {
            entered = 1;
            stream[n++] = pc0 - base_ip;
        }
    }

    /* `pid` is already in a ptrace-stop (the caller attached). Single-step it from
     * here; record only in-region offsets. The target is foreign, so we neither
     * attach nor detach — that is the caller's policy. */
    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (waitpid(pid, &status, 0) < 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break; /* target ended before/while in the region */
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            if (entered)
                overflow = 1;
            break;
        }

        uint64_t pc, retval;
        if (read_pc_ret(pid, &pc, &retval) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }

        if (pc >= base_ip && pc < base_ip + len) {
            entered = 1;
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = pc - base_ip;
            else
                overflow = 1;
        } else if (entered && !returned) {
            /* The step left the region: routine return, or a call-out to a helper we
             * step over (native-speed) and resume after — so a real managed-runtime
             * method that calls runtime helpers traces correctly, not just a leaf. */
            uint64_t resume_off = 0;
            exit_kind_t k = classify_region_exit(pid, code, len, base_ip,
                                                 stream[n - 1], &resume_off);
            if (k == EXIT_CALLOUT_RESUMED) {
                if (n < PTRACE_STREAM_CAP)
                    stream[n++] = resume_off;
                else
                    overflow = 1;
                continue; /* resume single-stepping from the call's return */
            }
            if (k == EXIT_CALLOUT_LOST) {
                overflow = 1; /* the callee never returned to the region — truncate */
                break;
            }
            if (result != NULL)
                *result = (long)retval;
            returned = 1;
            break; /* leave the target stopped past the region for the caller */
        }
    }

    if (rc == ASMTEST_PTRACE_OK)
        normalize(trace, code, base_ip, len, stream, n, overflow);
    free(stream);
    free(code);
    return rc;
}

int asmtest_ptrace_run_to(pid_t pid, const void *addr) {
    if (addr == NULL)
        return ASMTEST_PTRACE_EINVAL;
    /* Run the already-attached, ptrace-stopped target to `addr` (until the program
     * itself next calls in, with timing we do not control), leaving it stopped exactly
     * there for asmtest_ptrace_trace_attached. The breakpoint-cont-rewind core is shared
     * with the call-out step-over in the trace loops. */
    return run_until(pid, (uint64_t)(uintptr_t)addr);
}

#else /* stepper unsupported: not Linux, or not x86-64 / AArch64 */

int asmtest_ptrace_available(void) { return 0; }

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "out-of-process ptrace stepper needs Linux x86-64 or AArch64";
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
}

int asmtest_ptrace_trace_call(const void *code, size_t len, const long *args,
                              int nargs, long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached(pid_t pid, const void *base, size_t len,
                                  long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_run_to(pid_t pid, const void *addr) {
    (void)pid;
    (void)addr;
    return ASMTEST_PTRACE_ENOSYS;
}

#endif /* stepper */
