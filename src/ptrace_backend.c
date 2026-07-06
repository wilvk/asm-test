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

#include "asmtest_addr_channel.h"
#include "asmtest_codeimage.h"
#include "asmtest_descent_internal.h"
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

#define JITDUMP_MAGIC    0x4A695444u /* 'JiTD'           */
#define JITDUMP_MAGIC_SW 0x4454694Au /* byte-swapped     */
#define JIT_CODE_LOAD    0

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

    /* Initialize the output length so a caller that ignores a non-OK return (or a
     * path that never assigns it) cannot index bytes_out with stack garbage. */
    if (bytes_len != NULL)
        *bytes_len = 0;

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
                size_t cpy =
                    code_size < bytes_cap ? (size_t)code_size : bytes_cap;
                if (fread(bytes_out, 1, cpy, f) != cpy) {
                    /* Record's code bytes are truncated (a live JIT still flushing
                     * the newest record) — do not return OK with an unset length
                     * and partial bytes. */
                    rc = ASMTEST_PTRACE_ETRACE;
                    break;
                }
                if (bytes_len)
                    *bytes_len = cpy;
                if (code_size > cpy &&
                    fseek(f, (long)(code_size - cpy), SEEK_CUR) != 0) {
                    rc = ASMTEST_PTRACE_ETRACE;
                    break;
                }
            } else if (fseek(f, (long)code_size, SEEK_CUR) != 0) {
                rc = ASMTEST_PTRACE_ETRACE;
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
#include <sys/time.h>
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
#define PTRACE_BP_INSN 0xccULL /* int3                 */
#define PTRACE_BP_LEN  1
#else                                /* __aarch64__ */
#define PTRACE_BP_INSN 0xd4200000ULL /* brk #0 (little-endian) */
#define PTRACE_BP_LEN  4
#endif

/* Read the tracee's program counter (the about-to-execute instruction), the integer
 * return register, the stack pointer, and — on AArch64 — the link register x30. x86-64
 * has PTRACE_GETREGS; AArch64 does not, so it reads the GP set via
 * PTRACE_GETREGSET/NT_PRSTATUS. Any out-pointer may be NULL to skip that field; `pc` is
 * always required. SP and LR feed the call-descent shadow stack (the pop predicate needs
 * SP; AArch64 frame identity is (entry_lr, sp) because `bl` writes x30, not the stack).
 * `lr` is always 0 on x86-64 (no link register). Returns 0 on success, -1 on failure. */
static int read_pc_ret(pid_t pid, uint64_t *pc, uint64_t *ret, uint64_t *sp,
                       uint64_t *lr) {
    struct user_regs_struct regs;
#if defined(__x86_64__)
    if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
        return -1;
    if (pc != NULL)
        *pc = (uint64_t)regs.rip;
    if (ret != NULL)
        *ret = (uint64_t)regs.rax;
    if (sp != NULL)
        *sp = (uint64_t)regs.rsp;
    if (lr != NULL)
        *lr = 0; /* x86-64 has no link register */
#else            /* __aarch64__ */
    struct iovec iov = {&regs, sizeof regs};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &iov) !=
        0)
        return -1;
    if (pc != NULL)
        *pc = (uint64_t)regs.pc;
    if (ret != NULL)
        *ret = (uint64_t)regs.regs[0];
    if (sp != NULL)
        *sp = (uint64_t)regs.sp;
    if (lr != NULL)
        *lr = (uint64_t)regs.regs[30];
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

/* The x86 debug registers are reached through `struct user`'s u_debugreg[] via
 * PTRACE_POKEUSER. DR0..3 hold breakpoint addresses; DR7 enables them and selects the
 * condition/length. */
#define DR_OFFSET(n)                                                           \
    (offsetof(struct user, u_debugreg) + (size_t)(n) * sizeof(long))

/* Arm a HARDWARE execution breakpoint at `addr` in DR0 (per-thread). Unlike a software
 * int3 it writes no code, so it works on a W^X JIT code heap whose executable page is not
 * writable — and being per-thread, it never traps a sibling thread the way a process-wide
 * int3 can. DR7: L0=1 (enable DR0), R/W0=00 (execute), LEN0=00 (required for execute).
 * Returns 0 on success, -1 on a ptrace failure. */
static int set_hw_bp(pid_t pid, uint64_t addr) {
    if (ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(0),
               (void *)(uintptr_t)addr) != 0)
        return -1;
    if (ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(7),
               (void *)(uintptr_t)0x1UL) != 0)
        return -1;
    return 0;
}

/* Disarm the DR0 hardware breakpoint (clear DR7 enable, then DR0). Best-effort. */
static void clear_hw_bp(pid_t pid) {
    ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(7), (void *)(uintptr_t)0UL);
    ptrace(PTRACE_POKEUSER, pid, (void *)DR_OFFSET(0), (void *)(uintptr_t)0UL);
}
#endif /* __x86_64__ hardware breakpoint */

#if defined(__aarch64__)
/* AArch64 hardware execution breakpoints are reached through the NT_ARM_HW_BREAK
 * regset (struct user_hwdebug_state: dbg_info + dbg_regs[]), not debug-register
 * POKEUSER like x86. This is the W^X fallback for run_until on AArch64 — the software
 * `brk` (PTRACE_POKETEXT) cannot patch a non-writable JIT text page, so without this a
 * W^X callee's return breakpoint could not be planted and L2/L3 descent degraded to
 * edges-only. Requires >=1 debug breakpoint slot (probed below); qemu-user emulates none. */
#include <asm/ptrace.h> /* struct user_hwdebug_state; NT_ARM_HW_BREAK via elf.h */

/* DBGBCR control word for a 4-byte-aligned A64 execution breakpoint:
 *   E (bit 0) = 1                 enable
 *   PMC (bits 2:1) = 0b10         match at EL0 (user)
 *   BAS (bits 8:5) = 0b1111       all four instruction bytes
 * = 0x1e5. BT (breakpoint type, bits 23:20) = 0 (unlinked address match). */
#define ARM64_HWBP_CTRL 0x1e5u

/* Arm a HARDWARE execution breakpoint at `addr` in slot 0 (per-thread). Writes no code,
 * so it traps W^X JIT text as-shipped and never touches a sibling thread. Returns 0 on
 * success, -1 on a ptrace failure or when the host has no breakpoint slot. */
static int set_hw_bp(pid_t pid, uint64_t addr) {
    struct user_hwdebug_state dbg;
    memset(&dbg, 0, sizeof dbg);
    struct iovec iov = {&dbg, sizeof dbg};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_ARM_HW_BREAK,
               &iov) != 0)
        return -1;
    if ((dbg.dbg_info & 0xffu) == 0)
        return -1; /* no breakpoint register slots on this host */
    dbg.dbg_regs[0].addr = addr;
    dbg.dbg_regs[0].ctrl = ARM64_HWBP_CTRL;
    iov.iov_len = sizeof dbg;
    return ptrace(PTRACE_SETREGSET, pid, (void *)(uintptr_t)NT_ARM_HW_BREAK,
                  &iov);
}

/* Disarm the slot-0 hardware breakpoint. Best-effort. */
static void clear_hw_bp(pid_t pid) {
    struct user_hwdebug_state dbg;
    memset(&dbg, 0, sizeof dbg);
    struct iovec iov = {&dbg, sizeof dbg};
    if (ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_ARM_HW_BREAK,
               &iov) != 0)
        return;
    dbg.dbg_regs[0].addr = 0;
    dbg.dbg_regs[0].ctrl = 0;
    iov.iov_len = sizeof dbg;
    ptrace(PTRACE_SETREGSET, pid, (void *)(uintptr_t)NT_ARM_HW_BREAK, &iov);
}
#endif /* __aarch64__ hardware breakpoint */

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
    const char *msg = asmtest_ptrace_available()
                          ? "available"
                          : "AArch64 PTRACE_SINGLESTEP is non-functional here "
                            "(e.g. qemu-user "
                            "emulation); the out-of-process stepper needs a "
                            "real AArch64 host";
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
        size_t l =
            asmtest_disas(PTRACE_TRACE_ARCH, base, len, base_ip, off, NULL, 0);
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
    /* Default to a software int3/brk, which works on ordinary executable memory and needs
     * no debug-register budget. Fall back to a HARDWARE execution breakpoint when the code
     * is W^X (the executable page is not writable, so PTRACE_POKETEXT is refused with
     * EIO) — the case for a hardened JIT code heap (e.g. .NET's default). A hardware
     * breakpoint writes no code and is per-thread, so it traces W^X code as-shipped and
     * never traps a sibling thread. ASMTEST_PTRACE_HW_BP forces the hardware path (used to
     * exercise it deterministically on ordinary memory). Both x86-64 (debug registers) and
     * AArch64 (NT_ARM_HW_BREAK) implement set_hw_bp/clear_hw_bp; on AArch64 set_hw_bp fails
     * when the host exposes no breakpoint slots (e.g. qemu-user), so run_until then returns
     * ETRACE and the descent caller self-skips to edges-only. */
    int hw = 0;
    long orig = 0;
    const int force_hw = getenv("ASMTEST_PTRACE_HW_BP") != NULL;

    if (!force_hw) {
        errno = 0;
        orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)(uintptr_t)target, NULL);
        if (orig == -1 && errno != 0)
            return ASMTEST_PTRACE_ETRACE;
        const unsigned long mask = PTRACE_BP_LEN >= (int)sizeof(long)
                                       ? ~0UL
                                       : ((1UL << (PTRACE_BP_LEN * 8)) - 1);
        long planted =
            (long)(((unsigned long)orig & ~mask) | (PTRACE_BP_INSN & mask));
        if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target,
                   (void *)planted) != 0) {
            if (set_hw_bp(pid, target) !=
                0) /* W^X / unwritable text: try hardware */
                return ASMTEST_PTRACE_ETRACE;
            hw = 1;
        }
    } else {
        if (set_hw_bp(pid, target) != 0)
            return ASMTEST_PTRACE_ETRACE;
        hw = 1;
    }

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
            rc =
                ASMTEST_PTRACE_ENOENT; /* target ended before reaching `target` */
            break;
        }
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            sig = WSTOPSIG(
                status); /* forward an unrelated signal and keep running */
            continue;
        }
        uint64_t pc;
        if (read_pc_ret(pid, &pc, NULL, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        /* A hardware execution breakpoint and an AArch64 brk are FAULTS: the stop-PC is
         * AT the instruction. A software int3 is a TRAP: the stop-PC is one past the
         * byte, so back it up to find the hit and rewind before resuming. */
        uint64_t hit;
        if (hw)
            hit = pc;
        else
#if defined(__x86_64__)
            hit = pc - PTRACE_BP_LEN;
#else
            hit = pc;
#endif
        if (hit != target)
            continue; /* a SIGTRAP that is not our breakpoint; keep going */

        if (hw) {
            clear_hw_bp(pid); /* PC is already at target; nothing to rewind */
        } else {
            if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target,
                       (void *)orig) != 0) {
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
#if defined(__x86_64__)
            if (set_pc(pid, target) != 0)
                rc = ASMTEST_PTRACE_ETRACE;
#endif
        }
        break;
    }
    if (rc == ASMTEST_PTRACE_ETRACE) {
        if (hw) {
            clear_hw_bp(pid);
        } else {
            ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target,
                   (void *)orig);
        }
    }
    return rc;
}

/* How a single-step that LEFT the registered region (after entering it) is classified. */
typedef enum {
    EXIT_RETURNED,        /* the routine's own return (or a tail-jump out) */
    EXIT_CALLOUT_RESUMED, /* a call-out to a helper, stepped over; recording resumes */
    EXIT_CALLOUT_LOST /* a call-out whose step-over failed / target exited */
} exit_kind_t;

/* Decide whether a region exit is the routine RETURNING or merely CALLING OUT to a
 * helper outside the region (a runtime helper / GC barrier / PLT stub — the common case
 * for real managed-runtime methods, which the old "first exit == return" model could not
 * trace). If the last in-region instruction (`last_off` in `code`/`len`) is a call and
 * its return lands back in the region, run `pid` at NATIVE SPEED to that return address
 * (no per-instruction step through the callee) and report the in-region resume offset.
 * Needs the Capstone is-call query; without it, every exit reads as a return (leaf-only,
 * the previous behaviour). */
static exit_kind_t classify_region_exit(pid_t pid, const uint8_t *code,
                                        size_t len, uint64_t base_ip,
                                        uint64_t last_off,
                                        uint64_t *resume_off) {
    if (!asmtest_disas_available() ||
        !asmtest_disas_is_call(PTRACE_TRACE_ARCH, code, len, last_off))
        return EXIT_RETURNED;
    size_t cl =
        asmtest_disas(PTRACE_TRACE_ARCH, code, len, base_ip, last_off, NULL, 0);
    uint64_t ret_off = last_off + cl;
    if (cl == 0 || ret_off >= len)
        return EXIT_RETURNED; /* the call's fall-through is outside the region */
    if (run_until(pid, base_ip + ret_off) != ASMTEST_PTRACE_OK)
        return EXIT_CALLOUT_LOST;
    *resume_off = ret_off;
    return EXIT_CALLOUT_RESUMED;
}

/* ================================================================= */
/* Call descent — the return-address shadow-stack step loop.         */
/*                                                                   */
/* Levels >= 1 replace classify_region_exit's single-region model    */
/* with a shadow stack: each call-out records an edge (L1) and,       */
/* for L2/L3, may be DESCENDED as a nested frame whose own            */
/* instruction/block stream is recorded relative to the callee base. */
/* Frame 0 is the root region and, when `flat` is non-NULL, is        */
/* mirrored into the flat asmtest_trace_t byte-for-byte as before.    */
/* See docs/plans/call-descent-plan.md (Correctness core).           */
/* ================================================================= */

/* One OPEN frame on the shadow stack (a slice of descent->frames plus the live pop
 * bookkeeping). sp_ret / ret_addr are captured from the CALL that created the frame:
 * `call_sp` is the caller's SP at the call instruction (its pre-call SP), which the
 * callee's return restores exactly — so the pop predicate is PC==ret_addr && SP==sp_ret
 * && the just-stepped insn was a return, and a non-local exit that raises SP above sp_ret
 * is swept even without a matching return. */
typedef struct {
    int32_t idx;        /* index into descent->frames                       */
    uint64_t base, len; /* frame region extent (absolute)                   */
    uint64_t ret_addr;  /* absolute return address to pop at (0 for root)   */
    uint64_t sp_ret;    /* SP the caller regains once this frame returns     */
    uint32_t depth;
    int last_was_call;  /* the last recorded insn in this frame was a call  */
    int last_was_ret;   /* ... or a return (pop-predicate confirmation)     */
    uint64_t call_site; /* that call's offset within this frame             */
    uint64_t call_len;  /* that call's byte length                          */
    uint64_t call_sp;   /* SP at that call (the caller pre-call SP)          */
} dframe_t;

typedef struct {
    pid_t pid;
    asmtest_descent_t *d;
    asmtest_trace_t *flat; /* frame 0 -> flat trace (may be NULL) */
    long *result;
    uint64_t root_base, root_len;
    const uint8_t *root_code; /* bytes for the root region (frame 0) */
    int forward_faults; /* live/attached: forward SIGSEGV etc.; fork: terminal */
    int have_deadline;
    struct timespec deadline;
    int reaped; /* set once descend_core's own waitpid reaped the tracee */
} dctx_t;

#define DESCEND_MAX_STACK     256u
#define DESCEND_RECURSION_CAP 64
#define DESCEND_HARD_STEP_CAP                                                  \
    (1u << 22) /* anti-infinite-loop backstop (~4M steps) */

/* Disassemble the instruction at absolute `at`: return its byte length (0 = undecodable /
 * unreadable) and, via the out-params, whether it is a call / a return. Bytes come from
 * the root buffer when `at` is inside the root region (frame 0 and same-region recursion
 * frames), else a small live process_vm_readv window bounded by `frame_end`. */
static size_t descend_probe(const dctx_t *c, uint64_t at, uint64_t frame_end,
                            int *is_call, int *is_ret) {
    const uint8_t *code;
    size_t clen;
    uint64_t off;
    uint8_t buf[32];
    if (at >= c->root_base && at < c->root_base + c->root_len) {
        code = c->root_code;
        clen = (size_t)c->root_len;
        off = at - c->root_base;
    } else {
        size_t want = sizeof buf;
        if (frame_end > at && (frame_end - at) < want)
            want = (size_t)(frame_end - at);
        struct iovec l = {buf, want};
        struct iovec r = {(void *)(uintptr_t)at, want};
        ssize_t got = process_vm_readv(c->pid, &l, 1, &r, 1, 0);
        if (got <= 0) {
            if (is_call)
                *is_call = 0;
            if (is_ret)
                *is_ret = 0;
            return 0;
        }
        code = buf;
        clen = (size_t)got;
        off = 0;
    }
    /* One Capstone decode returns length + is_call + is_ret (block-boundary length is
     * base-address-independent, so no base_addr is needed here). */
    return asmtest_disas_probe(PTRACE_TRACE_ARCH, code, clen, off, is_call,
                               is_ret);
}

/* Record the instruction about to execute at absolute `at` in shadow frame `sf`
 * (offset-relative to sf->base), mirroring it into the flat trace for frame 0. Updates
 * sf->last_was_call/ret + the pending-call bookkeeping the next landing consults. */
static void descend_record(dctx_t *c, dframe_t *sf, uint64_t at, uint64_t sp) {
    uint64_t off = at - sf->base;
    int is_call = 0, is_ret = 0;
    size_t ilen = descend_probe(c, at, sf->base + sf->len, &is_call, &is_ret);
    int new_block = asmtest_descent_frame_record(c->d, sf->idx, off, ilen);
    if (sf->depth == 0 && c->flat != NULL) {
        if (new_block)
            trace_append_block(c->flat, off);
        trace_append_insn(c->flat, off);
        if (ilen == 0)
            c->flat->truncated = true;
    }
    sf->last_was_ret = (is_ret && ilen > 0);
    if (is_call && ilen > 0) {
        sf->last_was_call = 1;
        sf->call_site = off;
        sf->call_len = ilen;
        sf->call_sp = sp;
    } else {
        sf->last_was_call = 0;
    }
}

/* Decide whether to descend into `callee` from a frame at `depth`. Returns 1 to descend
 * (setting cbase/clen to the callee extent), 0 to step over. L1 never descends; L2
 * descends resolvable callees (allow-set, then optional resolver); L3 descends everything
 * not denied, resolving the extent from the target's executable mapping. */
static int descend_decide(dctx_t *c, uint32_t depth, uint64_t callee,
                          uint64_t *cbase, uint64_t *clen) {
    asmtest_descent_t *d = c->d;
    if (d->level < ASMTEST_DESCENT_DESCEND_KNOWN)
        return 0;
    if (depth + 1 > d->max_depth) {
        asmtest_descent_mark_depth_capped(d);
        return 0;
    }
    if (d->level == ASMTEST_DESCENT_DESCEND_KNOWN) {
        if (asmtest_descent_region_contains(d->allow, d->allow_len, callee,
                                            cbase, clen))
            return 1;
        if (d->resolver != NULL) {
            uint64_t b = 0, l = 0;
            /* The extent MUST contain the callee: descend_record computes the frame offset
             * as (pc - base), so a resolver returning a region that does not cover `callee`
             * would underflow to a ~2^64 offset. The allow-set / proc-maps paths guarantee
             * containment; enforce it for the caller-supplied resolver too. */
            if (d->resolver(callee, d->resolver_user, &b, &l) && l > 0 &&
                callee >= b && callee < b + l) {
                *cbase = b;
                *clen = l;
                return 1;
            }
        }
        return 0;
    }
    /* ASMTEST_DESCENT_DESCEND_ALL */
    if (asmtest_descent_region_contains(d->deny, d->deny_len, callee, NULL,
                                        NULL))
        return 0;
    if (d->denylist != NULL && d->denylist(callee, d->denylist_user))
        return 0;
    void *mb = NULL;
    size_t ml = 0;
    if (asmtest_proc_region_by_addr(c->pid, (void *)(uintptr_t)callee, &mb,
                                    &ml) != ASMTEST_PTRACE_OK)
        return 0; /* unknown extent -> step over */
    *cbase = (uint64_t)(uintptr_t)mb;
    *clen = (uint64_t)ml;
    return 1;
}

/* Backend-owned real-time watchdog. A per-step clock check cannot preempt a tracee blocked
 * in a descended read/futex/mmap syscall — waitpid() blocks and CI would hang, not
 * truncate. So descent arms a repeating ITIMER_REAL whose SIGALRM handler (installed with
 * SA_RESTART cleared) makes the blocked waitpid return EINTR; the loop then checks the
 * deadline and terminates. Single-descent-at-a-time, so a file-scope flag is safe. The old
 * SIGALRM disposition + timer are saved and restored on disarm; the restore reinstalls the
 * caller's remaining timer value as-captured, so a caller's pending alarm() is DELAYED by up
 * to the descent's bounded duration (watchdog_ms), not cleared or clobbered. Armed only for
 * L3 (see the arm site), so L1/L2 leave the caller's signal state untouched. */
static volatile sig_atomic_t descend_alarm_fired;
static void descend_alarm_handler(int sig) {
    (void)sig;
    descend_alarm_fired = 1;
}
static void descend_watchdog_arm(uint32_t ms, struct sigaction *saved_sa,
                                 struct itimerval *saved_it) {
    descend_alarm_fired = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = descend_alarm_handler;
    sa.sa_flags =
        0; /* NO SA_RESTART: SIGALRM must interrupt a blocked waitpid */
    sigaction(SIGALRM, &sa, saved_sa);
    struct itimerval it;
    it.it_value.tv_sec = ms / 1000u;
    it.it_value.tv_usec = (long)(ms % 1000u) * 1000L;
    /* Re-fire periodically so a single missed signal still breaks a persistent block. */
    it.it_interval.tv_sec = 0;
    it.it_interval.tv_usec = 100 * 1000L;
    if (it.it_value.tv_sec == 0 && it.it_value.tv_usec == 0)
        it.it_value.tv_usec = 1000L;
    setitimer(ITIMER_REAL, &it, saved_it);
}
static void descend_watchdog_disarm(const struct sigaction *saved_sa,
                                    const struct itimerval *saved_it) {
    setitimer(ITIMER_REAL, saved_it, NULL);
    sigaction(SIGALRM, saved_sa, NULL);
}

/* 1 if the descent's real-time deadline has passed (0 if no deadline is set). Checked both
 * between steps and — so a blocked waitpid interrupted by ANY signal (the caller's own
 * alarm(), not just the L3 timer) can bound an L2 descent — inside the waitpid retry loop. */
static int descend_deadline_exceeded(const dctx_t *c) {
    if (!c->have_deadline)
        return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec > c->deadline.tv_sec ||
           (now.tv_sec == c->deadline.tv_sec &&
            now.tv_nsec >= c->deadline.tv_nsec);
}

/* The descending single-step loop. The tracee (`c->pid`) is already at a ptrace stop.
 * Fills c->d (frame 0 + descended frames + edges), mirrors frame 0 into c->flat, and sets
 * *c->result at the root return. Returns OK when the root returned or the tracee exited,
 * ETRACE on a ptrace failure / hard cap / watchdog. Never single-steps forever: bounded by
 * DESCEND_HARD_STEP_CAP and the optional real-time deadline. */
static int descend_core(dctx_t *c) {
    asmtest_descent_t *d = c->d;
    int32_t f0 =
        asmtest_descent_push_frame(d, c->root_base, c->root_len, 0, -1);
    if (f0 < 0)
        return ASMTEST_PTRACE_ETRACE;
    dframe_t stack[DESCEND_MAX_STACK];
    memset(&stack[0], 0, sizeof stack[0]);
    stack[0].idx = f0;
    stack[0].base = c->root_base;
    stack[0].len = c->root_len;
    int top_i = 0;

    uint64_t steps = 0;
    int rc = ASMTEST_PTRACE_OK, status = 0, pending_sig = 0, entered = 0;

    /* Record the initial in-region PC (the run_to attach convention leaves PC at offset 0;
     * a no-op for the fork path, which starts before the region). */
    {
        uint64_t pc0, sp0;
        if (read_pc_ret(c->pid, &pc0, NULL, &sp0, NULL) != 0)
            return ASMTEST_PTRACE_ETRACE;
        if (pc0 >= c->root_base && pc0 < c->root_base + c->root_len) {
            descend_record(c, &stack[0], pc0, sp0);
            entered = 1;
        }
    }

    /* The SIGALRM/ITIMER_REAL watchdog is only needed to break a BLOCKED syscall in a
     * descended callee — an L3 (DESCEND_ALL) concern. For L1/L2 the per-step CLOCK_MONOTONIC
     * deadline below already bounds a runaway (no blocking calls in a known method region),
     * so we do NOT install a signal handler there — keeping descent from perturbing a host
     * process's own signal disposition (Go/JVM/.NET runtimes) on every L2 trace. */
    struct sigaction saved_sa;
    struct itimerval saved_it;
    int watchdog = c->have_deadline && d->level >= ASMTEST_DESCENT_DESCEND_ALL;
    if (watchdog)
        descend_watchdog_arm(d->watchdog_ms, &saved_sa, &saved_it);

    for (;;) {
        if (++steps > DESCEND_HARD_STEP_CAP) {
            asmtest_descent_mark_truncated(d);
            if (c->flat)
                c->flat->truncated = true;
            rc =
                ASMTEST_PTRACE_ETRACE; /* abandoned the routine mid-flight; *result unset */
            break;
        }
        if (descend_deadline_exceeded(c)) {
            asmtest_descent_mark_truncated(d);
            asmtest_descent_mark_depth_capped(d);
            if (c->flat)
                c->flat->truncated = true;
            rc = ASMTEST_PTRACE_ETRACE; /* terminal: caller kills/detaches */
            break;
        }
        if (ptrace(PTRACE_SINGLESTEP, c->pid, NULL,
                   (void *)(uintptr_t)pending_sig) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        pending_sig = 0;
        pid_t w;
        /* Retry across EINTR, but let two things break a blocked wait: the L3 watchdog
         * (descend_alarm_fired, owned only when armed) and the real-time deadline reached
         * while some OTHER signal — e.g. the caller's own alarm() — interrupted us. An
         * unrelated signal before the deadline is just retried, never a spurious abort.
         * (Only the L3 watchdog owns the flag; L1/L2 may see a stale 1 from a prior run,
         * so it is gated on `watchdog`.) */
        for (;;) {
            w = waitpid(c->pid, &status, 0);
            if (w >= 0 || errno != EINTR)
                break;
            if ((watchdog && descend_alarm_fired) ||
                descend_deadline_exceeded(c))
                break;
        }
        if (w < 0) {
            if ((watchdog && descend_alarm_fired) ||
                descend_deadline_exceeded(c)) {
                asmtest_descent_mark_truncated(
                    d); /* watchdog / deadline broke the wait */
                asmtest_descent_mark_depth_capped(d);
                if (c->flat)
                    c->flat->truncated = true;
            }
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            c->reaped =
                1; /* this waitpid consumed the exit status; the PID is gone */
            break; /* tracee finished */
        }
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            int sig = WSTOPSIG(status);
            if (!c->forward_faults && (sig == SIGSEGV || sig == SIGBUS ||
                                       sig == SIGILL || sig == SIGFPE)) {
                asmtest_descent_mark_truncated(d);
                if (c->flat)
                    c->flat->truncated = true;
                rc =
                    ASMTEST_PTRACE_ETRACE; /* fork fixture faulted before returning */
                break;
            }
            pending_sig =
                sig; /* forward benign / managed-runtime control signals */
            continue;
        }

        uint64_t pc, ret, sp;
        if (read_pc_ret(c->pid, &pc, &ret, &sp, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }

        /* Not yet in the root region: keep stepping (the fork path starts in libc glue
         * before the call reaches `code`). Only after entering does an out-of-region PC at
         * depth 0 mean the root returned. */
        if (!entered) {
            if (pc >= c->root_base && pc < c->root_base + c->root_len)
                entered = 1;
            else
                continue;
        }

        /* (A) POP phase (runs first, before classifying pc): pop every frame whose return
         * or unwind we have reached — an exact normal return (SP restored to the caller's
         * pre-call SP, PC at the return address, the just-stepped insn a ret) or a
         * non-local exit that raised SP above the frame's return SP. This must precede the
         * in-frame check because a same-region recursion's return address lies WITHIN the
         * frame's own region, so "PC left the region" never fires for it. Frame 0 is never
         * popped (the root return is handled in (D)). */
        while (top_i > 0) {
            dframe_t *f = &stack[top_i];
            if (sp >
                f->sp_ret) { /* unwound past this frame (longjmp / exception) */
                top_i--;
                continue;
            }
            if (sp == f->sp_ret && pc == f->ret_addr && f->last_was_ret) {
                top_i--; /* exact normal return */
                continue;
            }
            break;
        }
        dframe_t *top = &stack[top_i];

        /* (B) Landing inside the current frame: record it, detecting a same-region call
         * (recursion / self-call) as a distinct frame — the latent level-0 fold bug. */
        if (pc >= top->base && pc < top->base + top->len) {
            if (top->last_was_call) {
                uint64_t fallthru = top->base + top->call_site + top->call_len;
                if (pc !=
                    fallthru) { /* a call whose target lands in this same frame */
                    int same = 0;
                    for (int i = 0; i <= top_i; i++)
                        if (stack[i].base == top->base)
                            same++;
                    if ((unsigned)(top_i + 1) < DESCEND_MAX_STACK &&
                        same < DESCEND_RECURSION_CAP &&
                        top->depth + 1 <= d->max_depth) {
                        int32_t ni = asmtest_descent_push_frame(
                            d, top->base, top->len, top->depth + 1, top->idx);
                        if (ni < 0) {
                            asmtest_descent_mark_truncated(d);
                        } else {
                            uint64_t raddr = fallthru, sret = top->call_sp;
                            uint32_t ndepth = top->depth + 1;
                            /* The parent's pending-call is consumed by this push; clearing
                             * it means a later non-return exit of the parent (a tail-jump)
                             * degrades to honest truncation, not a mis-parented frame. */
                            top->last_was_call = 0;
                            top_i++;
                            memset(&stack[top_i], 0, sizeof stack[top_i]);
                            stack[top_i].idx = ni;
                            stack[top_i].base = stack[top_i - 1].base;
                            stack[top_i].len = stack[top_i - 1].len;
                            stack[top_i].ret_addr = raddr;
                            stack[top_i].sp_ret = sret;
                            stack[top_i].depth = ndepth;
                            top = &stack[top_i];
                        }
                    } else {
                        asmtest_descent_mark_depth_capped(
                            d); /* recursion cap: fold */
                    }
                }
            }
            descend_record(c, top, pc, sp);
            continue;
        }

        /* (C) Genuine exit from `top` with no frame popped: a call-OUT, or a return. */
        if (top->last_was_call) {
            uint64_t call_site = top->call_site;
            uint64_t ret_off = call_site + top->call_len;
            if (ret_off < top->len) {
                uint64_t ret_addr = top->base + ret_off;
                uint64_t callee = pc;
                uint64_t cbase = 0, clen = 0;
                /* Decide independently of the budget so `depth_capped` is set ONLY when the
                 * budget actually suppressed a descent that would otherwise have happened —
                 * not for every call-out past the budget, most of which (unknown callees)
                 * would have been stepped over regardless. */
                int would =
                    descend_decide(c, top->depth, callee, &cbase, &clen);
                int budget_ok = (steps < d->insn_budget);
                int desc = budget_ok && would;
                if (!budget_ok && would)
                    asmtest_descent_mark_depth_capped(d);
                if (desc && clen > 0 &&
                    (unsigned)(top_i + 1) < DESCEND_MAX_STACK) {
                    int32_t ni = asmtest_descent_push_frame(
                        d, cbase, clen, top->depth + 1, top->idx);
                    if (ni >= 0) {
                        uint64_t sret = top->call_sp;
                        uint32_t ndepth = top->depth + 1;
                        top->last_was_call =
                            0; /* consumed by the push (see branch B) */
                        top_i++;
                        memset(&stack[top_i], 0, sizeof stack[top_i]);
                        stack[top_i].idx = ni;
                        stack[top_i].base = cbase;
                        stack[top_i].len = clen;
                        stack[top_i].ret_addr = ret_addr;
                        stack[top_i].sp_ret = sret;
                        stack[top_i].depth = ndepth;
                        descend_record(c, &stack[top_i], pc,
                                       sp); /* callee entry */
                        continue;
                    }
                    asmtest_descent_mark_truncated(
                        d); /* push failed: step over instead */
                }
                /* Step over: record an edge (L1+), run the callee at native speed. */
                if (d->level >= ASMTEST_DESCENT_RECORD_EDGES)
                    asmtest_descent_add_edge(d, call_site, callee, top->depth,
                                             top->idx);
                if (run_until(c->pid, ret_addr) != ASMTEST_PTRACE_OK) {
                    asmtest_descent_mark_truncated(d);
                    if (c->flat)
                        c->flat->truncated = true;
                    rc = ASMTEST_PTRACE_ETRACE;
                    break;
                }
                uint64_t rpc, rsp;
                if (read_pc_ret(c->pid, &rpc, NULL, &rsp, NULL) != 0) {
                    rc = ASMTEST_PTRACE_ETRACE;
                    break;
                }
                descend_record(c, top, ret_addr,
                               rsp); /* resume at the return address */
                continue;
            }
            /* ret_off outside the region: the call's fall-through leaves the frame — fall
             * through to the return handling below. */
        }

        /* (D) Return (or tail-jump out). Root return finishes; a descended frame pops. */
        if (top->depth == 0) {
            if (c->result != NULL)
                *c->result = (long)ret;
            break;
        }
        top_i--;
        top = &stack[top_i];
        if (pc >= top->base && pc < top->base + top->len)
            descend_record(c, top, pc, sp);
    }

    if (watchdog)
        descend_watchdog_disarm(&saved_sa, &saved_it);
    return rc;
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
    int pending_sig =
        0; /* signal to inject on the next step (forwarded, unrelated) */

    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        /* Could not reach the initial SIGSTOP. */
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    for (;;) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL,
                   (void *)(uintptr_t)pending_sig) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        pending_sig = 0;
        if (waitpid(pid, &status, 0) < 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break; /* tracee finished */
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            int sig = WSTOPSIG(status);
            /* A fault signal from the routine itself (SIGSEGV/BUS/ILL/FPE) is
             * terminal: mark the capture incomplete and reap the tracee — it is
             * stopped in signal-delivery, not exited, and PTRACE_O_EXITKILL only
             * fires when the *tracer* exits, so otherwise the stopped child leaks
             * (a suite of faulting routines would exhaust PIDs). Any OTHER signal
             * (a process-group SIGWINCH/SIGINT/SIGTSTP delivered to the shared
             * group) is unrelated to the routine — forward it and keep stepping,
             * as run_until does, rather than killing a healthy tracee and returning
             * an empty trace as complete. */
            if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                sig == SIGFPE) {
                overflow = 1; /* faulted before returning: incomplete capture */
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            pending_sig = sig; /* inject on the next PTRACE_SINGLESTEP */
            continue;
        }

        uint64_t pc, retval;
        if (read_pc_ret(pid, &pc, &retval, NULL, NULL) != 0) {
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
            exit_kind_t k =
                classify_region_exit(pid, (const uint8_t *)code, len, base_ip,
                                     stream[n - 1], &resume_off);
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
            /* We have the return value; the child's only remaining work is _exit().
             * CONT once, but if a signal stops it before it exits (a process-group
             * signal in that window), kill+reap so it can't survive as a stopped,
             * unreaped tracee (PTRACE_O_EXITKILL only fires on tracer exit). */
            ptrace(PTRACE_CONT, pid, NULL, NULL);
            if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status)) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            break;
        }
    }

    if (rc == ASMTEST_PTRACE_OK)
        normalize(trace, (const uint8_t *)code, base_ip, len, stream, n,
                  overflow);
    else {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    free(stream);
    return rc;
}

/* ------------------------------------------------------------------ */
/* BTF block-step (P2): PTRACE_SINGLEBLOCK — one #DB per TAKEN branch   */
/* instead of one per instruction (~4-10x fewer stops on compute        */
/* kernels), reconstructing the SAME per-instruction offset stream.     */
/* x86-64 only (SINGLEBLOCK is x86/ppc/s390; AArch64 has no equivalent  */
/* and the whole file's tracer is x86/arm64). Rootless, works under any  */
/* perf_event_paranoid — it is just ptrace of one's own child. See       */
/* docs/plans/amd-tracing-plan.md "BTF block-step is available on x86". */
/* ------------------------------------------------------------------ */
#if defined(__x86_64__)

#ifndef PTRACE_SINGLEBLOCK
#define PTRACE_SINGLEBLOCK 33 /* <sys/ptrace.h> omits it though the kernel wires it */
#endif

/* Hang-proof one-shot probe: does PTRACE_SINGLEBLOCK actually advance a child here?
 * It is unwired under some sandboxes/emulators (returns EIO), so probe rather than
 * assume. Mirrors probe_singlestep's WNOHANG-with-deadline shape so it can never hang
 * CI, and caches. */
static int probe_singleblock(void) {
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(0);
    }
    int st, got = 0, stepped = 0;
    for (int i = 0; i < 200; i++) {
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
    if (got && ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) == 0) {
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

int asmtest_ptrace_blockstep_available(void) {
    static int cached = -1;
    if (cached < 0)
        cached = probe_singleblock() && asmtest_disas_available();
    return cached;
}

/* Reconstruct the straight-line run of one basic block into `stream`. A block-step
 * #DB is TRAP-class: the stop RIP is the branch TARGET, so between the block start
 * `from_off` and the observed next stop `next_pc` the CPU ran from `from_off` up to a
 * terminating TAKEN branch. Walk instructions from `from_off`, appending each offset,
 * until the terminator: a `ret`/indirect branch (always taken) or a direct branch
 * whose target == next_pc (taken); a direct conditional whose target != next_pc was
 * NOT taken (BTF does not fire on it) so we fall through and keep walking. Returns 1 on
 * a clean terminator (with *last_off = its offset), 0 on overflow/undecodable/no
 * region terminator (caller marks truncated). */
static int blockstep_reconstruct(const uint8_t *code, size_t len, uint64_t base_ip,
                                 uint64_t from_off, uint64_t next_pc,
                                 uint64_t *stream, uint32_t *pn, uint64_t *last_off) {
    uint64_t walk = from_off;
    /* Bounded by the region instruction count: a block cannot revisit an offset, so
     * more than `len` steps means the walk is not converging. */
    for (size_t guard = 0; guard <= len; guard++) {
        if (walk >= len)
            return 0; /* ran off the region without a terminator */
        if (*pn >= PTRACE_STREAM_CAP)
            return 0; /* stream full */
        stream[(*pn)++] = walk;
        *last_off = walk;
        int is_call = 0, is_ret = 0;
        size_t l = asmtest_disas_probe(PTRACE_TRACE_ARCH, code, len, walk,
                                       &is_call, &is_ret);
        if (l == 0)
            return 0; /* undecodable */
        if (!asmtest_disas_is_branch(PTRACE_TRACE_ARCH, code, len, walk)) {
            walk += l;
            continue; /* straight-line instruction: block continues */
        }
        if (is_ret)
            return 1; /* return: indirect, terminator */
        uint64_t target = 0;
        if (!asmtest_disas_branch_target(PTRACE_TRACE_ARCH, code, len, base_ip,
                                         walk, &target))
            return 1; /* indirect jmp/call: unconditionally taken, terminator */
        if (target == next_pc)
            return 1; /* direct branch TAKEN to the observed next stop: terminator */
        walk += l;    /* direct conditional NOT taken: fall through, keep walking */
    }
    return 0;
}

int asmtest_ptrace_trace_call_blockstep(const void *code, size_t len,
                                        const long *args, int nargs, long *result,
                                        asmtest_trace_t *trace) {
    if (code == NULL || len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return ASMTEST_PTRACE_EINVAL;
    if (!asmtest_ptrace_blockstep_available())
        return ASMTEST_PTRACE_ENOSYS;

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
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)code)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }

    const uint64_t base_ip = (uint64_t)(uintptr_t)code;
    const uint64_t SENTINEL = ~(uint64_t)0;
    uint64_t prev_off = SENTINEL; /* start of the open (not-yet-terminated) block */
    uint32_t n = 0;
    int overflow = 0, returned = 0, rc = ASMTEST_PTRACE_OK, status = 0;
    int pending_sig = 0;

    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    for (;;) {
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL,
                   (void *)(uintptr_t)pending_sig) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        pending_sig = 0;
        if (waitpid(pid, &status, 0) < 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            int sig = WSTOPSIG(status);
            /* A #DB also fires on interrupts/exceptions (APM §13.2); a fault signal
             * from the routine is terminal, any other (process-group) signal is
             * unrelated — forward it and keep block-stepping, same policy as the
             * single-step loop. */
            if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                sig == SIGFPE) {
                overflow = 1;
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
            pending_sig = sig;
            continue;
        }

        uint64_t pc, retval;
        if (read_pc_ret(pid, &pc, &retval, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        int in_region = (pc >= base_ip && pc < base_ip + len);

        if (prev_off == SENTINEL) {
            /* Not entered yet: block-step through the entry glue at native speed.
             * The first in-region stop is the region entry (the call target). */
            if (in_region)
                prev_off = pc - base_ip;
            continue;
        }

        /* We have an open block starting at prev_off; the current stop `pc` is the
         * target its terminating taken branch reached. Reconstruct that block. */
        uint64_t last_off = 0;
        if (!blockstep_reconstruct((const uint8_t *)code, len, base_ip, prev_off,
                                   pc, stream, &n, &last_off)) {
            overflow = 1;
            break;
        }

        if (in_region) {
            prev_off = pc - base_ip; /* the block's target opens the next block */
            continue;
        }

        /* The block left the region: routine return or a call-out to a helper. Reuse
         * the single-step classifier (it plants a breakpoint at the call's return and
         * runs the callee at native speed), so a method calling runtime helpers still
         * traces, not just a pure leaf. */
        uint64_t resume_off = 0;
        exit_kind_t k = classify_region_exit(pid, (const uint8_t *)code, len,
                                             base_ip, last_off, &resume_off);
        if (k == EXIT_CALLOUT_RESUMED) {
            prev_off = resume_off; /* resume block-stepping from the call's return */
            continue;
        }
        if (k == EXIT_CALLOUT_LOST) {
            overflow = 1;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        if (result != NULL)
            *result = (long)retval;
        returned = 1;
        ptrace(PTRACE_CONT, pid, NULL, NULL);
        if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        break;
    }
    (void)returned;

    if (rc == ASMTEST_PTRACE_OK)
        normalize(trace, (const uint8_t *)code, base_ip, len, stream, n,
                  overflow);
    else {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    free(stream);
    return rc;
}

#else /* !__x86_64__ : no PTRACE_SINGLEBLOCK equivalent */
int asmtest_ptrace_blockstep_available(void) { return 0; }
int asmtest_ptrace_trace_call_blockstep(const void *code, size_t len,
                                        const long *args, int nargs, long *result,
                                        asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}
#endif /* __x86_64__ */

/* ------------------------------------------------------------------ */
/* §D3 — whole-window multi-region capture over a cross-process channel */
/* (this whole file section is already inside the linux + x86-64/arm64  */
/* guard; the non-supported stub is in the outer #else at end-of-file). */
/* ------------------------------------------------------------------ */

/* 1 if `pc` is inside the window frame OR any channel-published JIT region. */
static int in_region_set(uint64_t pc, uint64_t win_base, uint64_t win_len,
                         const asmtest_addr_rec_t *regs, uint32_t nreg) {
    if (pc >= win_base && pc < win_base + win_len)
        return 1;
    for (uint32_t i = 0; i < nreg; i++)
        if (regs[i].len && pc >= regs[i].base && pc < regs[i].base + regs[i].len)
            return 1;
    return 0;
}

/* Decode the instruction length at absolute `at` in the tracee via a small foreign
 * read (0 if unreadable/undecodable) — used to detect block boundaries in the
 * absolute-address stream a multi-region capture records. */
static size_t foreign_insn_len(pid_t pid, uint64_t at) {
    uint8_t buf[16];
    struct iovec l = {buf, sizeof buf};
    struct iovec r = {(void *)(uintptr_t)at, sizeof buf};
    ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
    if (got <= 0)
        return 0;
    return asmtest_disas(PTRACE_TRACE_ARCH, buf, (size_t)got, 0, 0, NULL, 0);
}

int asmtest_ptrace_trace_attached_windowed(pid_t pid, const void *win_base_p,
                                           size_t win_len,
                                           asmtest_addr_channel_t *chan,
                                           long *result, asmtest_trace_t *trace) {
    if (win_base_p == NULL || win_len == 0 || trace == NULL)
        return ASMTEST_PTRACE_EINVAL;
    const uint64_t win_base = (uint64_t)(uintptr_t)win_base_p;

    /* The caller has run_to'd win_base (the window frame's entry). Capture the frame's
     * RETURN address so we can end the window when control returns to it — the
     * region-agnostic window boundary (the alternative, "pc left the region," has no
     * meaning across a set of regions the JIT keeps growing). */
    uint64_t pc0 = 0, sp0 = 0, win_ret = 0;
    if (read_pc_ret(pid, &pc0, NULL, &sp0, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;
#if defined(__x86_64__)
    { /* just after `call`, [rsp] holds the return address */
        struct iovec l = {&win_ret, sizeof win_ret};
        struct iovec r = {(void *)(uintptr_t)sp0, sizeof win_ret};
        if (process_vm_readv(pid, &l, 1, &r, 1, 0) != (ssize_t)sizeof win_ret)
            return ASMTEST_PTRACE_ETRACE;
    }
#else
    read_pc_ret(pid, NULL, NULL, NULL, &win_ret); /* AArch64: LR holds the return */
#endif

    /* Accumulated published regions (window frame + every JIT method the parent
     * streams). Drained from the channel at entry and after every step. */
    asmtest_addr_rec_t regs[ASMTEST_ADDR_CHAN_CAP];
    uint32_t nreg = 0;
    if (chan != NULL)
        nreg = asmtest_addr_channel_drain(chan, regs, ASMTEST_ADDR_CHAN_CAP);

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL)
        return ASMTEST_PTRACE_ETRACE;
    uint32_t n = 0;
    int overflow = 0, rc = ASMTEST_PTRACE_OK, status = 0;
    long retval_final = 0;

    if (in_region_set(pc0, win_base, win_len, regs, nreg) && n < PTRACE_STREAM_CAP)
        stream[n++] = pc0; /* record the window entry */

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
            break;
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            overflow = 1;
            break;
        }
        uint64_t pc = 0, rax = 0;
        if (read_pc_ret(pid, &pc, &rax, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        /* Pick up regions the parent published since the last step (a method JIT'd
         * mid-window) — this is the cross-process address channel doing its job. */
        if (chan != NULL && nreg < ASMTEST_ADDR_CHAN_CAP)
            nreg += asmtest_addr_channel_drain(chan, regs + nreg,
                                               ASMTEST_ADDR_CHAN_CAP - nreg);

        if (pc == win_ret) { /* control returned to the window's caller: done */
            retval_final = (long)rax;
            break;
        }
        if (in_region_set(pc, win_base, win_len, regs, nreg)) {
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = pc; /* record ABSOLUTE — the caller classifies by region */
            else {
                overflow = 1;
                break;
            }
        }
        /* pc outside every published region: runtime/glue between managed methods.
         * Not recorded (the noise the managed capture elides); single-stepping still
         * carries us through it back to the next published region. */
    }
    if (result != NULL)
        *result = retval_final;

    /* Materialize the ABSOLUTE-address stream: a new block starts at a discontinuity
     * (this address is not the fall-through of the previous recorded one), matching
     * the block partition of every other backend. Lengths come from foreign reads. */
    if (rc == ASMTEST_PTRACE_OK) {
        int have_prev = 0;
        uint64_t expected_next = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t at = stream[i];
            if (!have_prev || at != expected_next)
                trace_append_block(trace, at);
            trace_append_insn(trace, at);
            size_t l = foreign_insn_len(pid, at);
            if (l == 0) {
                trace->truncated = true;
                break;
            }
            expected_next = at + l;
            have_prev = 1;
        }
        if (overflow)
            trace->truncated = true;
    }
    free(stream);
    return rc;
}

/* Shared body for the live-snapshot and time-versioned variants below. The ONLY
 * difference between them is the byte source handed to the block normalizer: a single
 * process_vm_readv (img == NULL) versus the time-correct bytes the code-image recorder
 * had live at sequence `when` (img != NULL). The single-step loop is identical. */
static int trace_attached_impl(pid_t pid, const void *base, size_t len,
                               struct asmtest_codeimage *img, uint64_t when,
                               long *result, asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || trace == NULL)
        return ASMTEST_PTRACE_EINVAL;

    /* Obtain the region bytes. Versioned: borrow the recorder's time-correct copy (the
     * right bytes even if the address was re-JITted/reused). Live: read FROM THE TARGET
     * via process_vm_readv (a debugger-style foreign read), owning a fresh buffer. */
    const uint8_t *code = NULL;
    uint8_t *owned = NULL; /* freed iff we allocated it (the live path) */
    if (img != NULL) {
        const uint8_t *vb = NULL;
        size_t avail = 0;
        if (asmtest_codeimage_bytes_at(img, base, when, &vb, &avail) !=
                ASMTEST_CI_OK ||
            avail < len)
            return ASMTEST_PTRACE_ENOENT; /* region not tracked at/by `when` */
        code = vb;
    } else {
        owned = (uint8_t *)malloc(len);
        if (owned == NULL)
            return ASMTEST_PTRACE_ETRACE;
        struct iovec liov = {owned, len};
        struct iovec riov = {(void *)(uintptr_t)base, len};
        if (process_vm_readv(pid, &liov, 1, &riov, 1, 0) != (ssize_t)len) {
            free(owned);
            return ASMTEST_PTRACE_ETRACE;
        }
        code = owned;
    }

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL) {
        free(owned);
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
        if (read_pc_ret(pid, &pc0, &ret0, NULL, NULL) != 0) {
            free(owned);
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
        if (read_pc_ret(pid, &pc, &retval, NULL, NULL) != 0) {
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
                overflow =
                    1; /* the callee never returned to the region — truncate */
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
    free(owned);
    return rc;
}

int asmtest_ptrace_trace_attached(pid_t pid, const void *base, size_t len,
                                  long *result, asmtest_trace_t *trace) {
    return trace_attached_impl(pid, base, len, NULL, 0, result, trace);
}

int asmtest_ptrace_trace_attached_versioned(pid_t pid, const void *base,
                                            size_t len,
                                            struct asmtest_codeimage *img,
                                            uint64_t when, long *result,
                                            asmtest_trace_t *trace) {
    return trace_attached_impl(pid, base, len, img, when, result, trace);
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

/* ---- Descending variants (call-descent). ---- */

/* Fill a dctx watchdog deadline from the descent handle (level >= DESCEND_KNOWN only, so
 * L0/L1 runs are unbounded-in-time like today). */
static void descend_set_deadline(dctx_t *c, const asmtest_descent_t *d) {
    if (d->level < ASMTEST_DESCENT_DESCEND_KNOWN || d->watchdog_ms == 0)
        return;
    clock_gettime(CLOCK_MONOTONIC, &c->deadline);
    c->deadline.tv_sec += (time_t)(d->watchdog_ms / 1000u);
    c->deadline.tv_nsec += (long)(d->watchdog_ms % 1000u) * 1000000L;
    if (c->deadline.tv_nsec >= 1000000000L) {
        c->deadline.tv_sec++;
        c->deadline.tv_nsec -= 1000000000L;
    }
    c->have_deadline = 1;
}

/* Fork path: fork a tracee that calls `code`, single-step-descend it, reap it. */
static int trace_call_descend(const void *code, size_t len, const long *args,
                              int nargs, long *result, asmtest_trace_t *trace,
                              asmtest_descent_t *descent) {
    long a[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < nargs; i++)
        a[i] = args[i];

    pid_t pid = fork();
    if (pid < 0)
        return ASMTEST_PTRACE_ETRACE;
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)code)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    dctx_t c;
    memset(&c, 0, sizeof c);
    c.pid = pid;
    c.d = descent;
    c.flat = trace;
    c.result = result;
    c.root_base = (uint64_t)(uintptr_t)code;
    c.root_len = len;
    c.root_code = (const uint8_t *)code;
    c.forward_faults =
        0; /* controlled single-threaded callee: a fault is terminal */
    descend_set_deadline(&c, descent);

    int rc = descend_core(&c);
    /* We own this tracee; reap it — UNLESS descend_core's own waitpid already consumed the
     * exit status (WIFEXITED/WIFSIGNALED). Killing an already-reaped PID would, in a
     * concurrent test harness that forks between the reap and here, SIGKILL an unrelated
     * process that recycled the PID; the flat trace/result are already captured. */
    if (!c.reaped) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    return rc;
}

/* Attached path: descend a region in an already-attached foreign target (live or
 * code-image-versioned bytes). Leaves the target ptrace-stopped for the caller to detach. */
static int trace_attached_descend(pid_t pid, const void *base, size_t len,
                                  struct asmtest_codeimage *img, uint64_t when,
                                  long *result, asmtest_trace_t *trace,
                                  asmtest_descent_t *descent) {
    const uint8_t *code = NULL;
    uint8_t *owned = NULL;
    if (img != NULL) {
        const uint8_t *vb = NULL;
        size_t avail = 0;
        if (asmtest_codeimage_bytes_at(img, base, when, &vb, &avail) !=
                ASMTEST_CI_OK ||
            avail < len)
            return ASMTEST_PTRACE_ENOENT;
        code = vb;
    } else {
        owned = (uint8_t *)malloc(len);
        if (owned == NULL)
            return ASMTEST_PTRACE_ETRACE;
        struct iovec liov = {owned, len};
        struct iovec riov = {(void *)(uintptr_t)base, len};
        if (process_vm_readv(pid, &liov, 1, &riov, 1, 0) != (ssize_t)len) {
            free(owned);
            return ASMTEST_PTRACE_ETRACE;
        }
        code = owned;
    }

    dctx_t c;
    memset(&c, 0, sizeof c);
    c.pid = pid;
    c.d = descent;
    c.flat = trace;
    c.result = result;
    c.root_base = (uint64_t)(uintptr_t)base;
    c.root_len = len;
    c.root_code = code;
    c.forward_faults =
        1; /* live managed runtime: forward its safepoint/GC signals */
    descend_set_deadline(&c, descent);

    int rc = descend_core(&c);
    free(owned);
    return rc;
}

int asmtest_ptrace_trace_call_ex(const void *code, size_t len, const long *args,
                                 int nargs, long *result,
                                 asmtest_trace_t *trace,
                                 asmtest_descent_t *descent) {
    if (code == NULL || len == 0 || nargs < 0 || nargs > 6 ||
        (nargs > 0 && args == NULL))
        return ASMTEST_PTRACE_EINVAL;
    /* Descent needs Capstone (call/return detection); without it, or at OFF, fall back to
     * the level-0 loop and leave the handle empty. */
    if (descent == NULL || descent->level == ASMTEST_DESCENT_OFF ||
        !asmtest_disas_available())
        return asmtest_ptrace_trace_call(code, len, args, nargs, result, trace);
    return trace_call_descend(code, len, args, nargs, result, trace, descent);
}

int asmtest_ptrace_trace_attached_ex(pid_t pid, const void *base, size_t len,
                                     long *result, asmtest_trace_t *trace,
                                     asmtest_descent_t *descent) {
    if (base == NULL || len == 0)
        return ASMTEST_PTRACE_EINVAL;
    if (descent == NULL || descent->level == ASMTEST_DESCENT_OFF ||
        !asmtest_disas_available())
        return trace_attached_impl(pid, base, len, NULL, 0, result, trace);
    return trace_attached_descend(pid, base, len, NULL, 0, result, trace,
                                  descent);
}

int asmtest_ptrace_trace_attached_versioned_ex(pid_t pid, const void *base,
                                               size_t len,
                                               struct asmtest_codeimage *img,
                                               uint64_t when, long *result,
                                               asmtest_trace_t *trace,
                                               asmtest_descent_t *descent) {
    if (base == NULL || len == 0)
        return ASMTEST_PTRACE_EINVAL;
    if (descent == NULL || descent->level == ASMTEST_DESCENT_OFF ||
        !asmtest_disas_available())
        return trace_attached_impl(pid, base, len, img, when, result, trace);
    return trace_attached_descend(pid, base, len, img, when, result, trace,
                                  descent);
}

#else /* stepper unsupported: not Linux, or not x86-64 / AArch64 */

int asmtest_ptrace_available(void) { return 0; }

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg =
        "out-of-process ptrace stepper needs Linux x86-64 or AArch64";
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

int asmtest_ptrace_blockstep_available(void) { return 0; }

int asmtest_ptrace_trace_call_blockstep(const void *code, size_t len,
                                        const long *args, int nargs, long *result,
                                        asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached_windowed(pid_t pid, const void *win_base_p,
                                           size_t win_len,
                                           asmtest_addr_channel_t *chan,
                                           long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)win_base_p;
    (void)win_len;
    (void)chan;
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

int asmtest_ptrace_trace_attached_versioned(pid_t pid, const void *base,
                                            size_t len,
                                            struct asmtest_codeimage *img,
                                            uint64_t when, long *result,
                                            asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
    (void)img;
    (void)when;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_run_to(pid_t pid, const void *addr) {
    (void)pid;
    (void)addr;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_call_ex(const void *code, size_t len, const long *args,
                                 int nargs, long *result,
                                 asmtest_trace_t *trace,
                                 asmtest_descent_t *descent) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    (void)descent;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached_ex(pid_t pid, const void *base, size_t len,
                                     long *result, asmtest_trace_t *trace,
                                     asmtest_descent_t *descent) {
    (void)pid;
    (void)base;
    (void)len;
    (void)result;
    (void)trace;
    (void)descent;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached_versioned_ex(pid_t pid, const void *base,
                                               size_t len,
                                               struct asmtest_codeimage *img,
                                               uint64_t when, long *result,
                                               asmtest_trace_t *trace,
                                               asmtest_descent_t *descent) {
    (void)pid;
    (void)base;
    (void)len;
    (void)img;
    (void)when;
    (void)result;
    (void)trace;
    (void)descent;
    return ASMTEST_PTRACE_ENOSYS;
}

#endif /* stepper */
