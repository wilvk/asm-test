/*
 * ptrace_backend.c — out-of-process single-step native-trace backend (W2).
 * See asmtest_ptrace.h and docs/internal/plans/zen2-singlestep-trace-plan.md (Phase 5, W2).
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
#include "asmtest_blockstep_internal.h"
#include "asmtest_codeimage.h"
#include "asmtest_descent_internal.h"
#include "asmtest_ptrace.h"
#include "asmtest_trace.h"
#include "bs_recon.h"

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

#include <dlfcn.h> /* RTLD_DEFAULT: the built-in descent denylist symbols */
#include <elf.h>   /* NT_PRSTATUS */
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static ssize_t ptrace_read_mem(pid_t pid, void *dest, const void *src,
                               size_t len) {
    struct iovec l = {dest, len};
    struct iovec r = {(void *)(uintptr_t)src, len};
    ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
    if (got > 0) {
        return got;
    }

    /* Fallback to ptrace PTRACE_PEEKDATA */
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    size_t i = 0;
    while (i < len) {
        uintptr_t addr = (uintptr_t)(s + i);
        uintptr_t align_addr = addr & ~(sizeof(long) - 1);
        errno = 0;
        long val = ptrace(PTRACE_PEEKDATA, pid, (void *)align_addr, NULL);
        if (val == -1 && errno != 0) {
            return i > 0 ? (ssize_t)i : -1;
        }
        size_t offset = addr - align_addr;
        size_t chunk = sizeof(long) - offset;
        if (chunk > len - i) {
            chunk = len - i;
        }
        memcpy(d + i, (unsigned char *)&val + offset, chunk);
        i += chunk;
    }
    return (ssize_t)len;
}

/* Mirrors dfp_sigtrap_is_app / asmspy's sigtrap_is_app: a SIGTRAP the tracee raised
 * ITSELF (executed int3 -> SI_KERNEL; its own hw breakpoint -> TRAP_HWBKPT). TRAP_TRACE
 * and TRAP_BRKPT are OUR step/#DB completing (TRAP_BRKPT = a step across a syscall,
 * MEASURED — see cli/asmspy_engine.c), NOT an app breakpoint. A GETSIGINFO failure or
 * any other si_code => ours (the pre-fix behaviour — a false negative is a regression to
 * the prior discard-not-deliver default, not a new hazard). Shared by every ptrace loop
 * in this file (single-step and block-step, both architectures): none of them plant an
 * int3 of their own at a tracee-visible SIGTRAP stop (breakpoints are handled by their
 * own hit/target match before this is consulted), so a positive here is unambiguously
 * the target's own trap — a JVM safepoint poll, a .NET breakpoint. */
static int bs_sigtrap_is_app(pid_t pid) {
    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, pid, NULL, &si) != 0)
        return 0;
    return si.si_code == SI_KERNEL || si.si_code == TRAP_HWBKPT;
}

/* Ordered in-region PC-offset capture buffer; overflow is flagged truncated, never
 * emitted as complete. Sized for the small-routine envelope, like ss_backend. */
#ifndef PTRACE_STREAM_CAP
#define PTRACE_STREAM_CAP (1u << 16) /* 65536 offsets */
#endif

/* Hard step backstop for the WHOLE-WINDOW capture: PTRACE_STREAM_CAP bounds the RECORDED
 * (in-region) instructions, but a window single-steps unbounded runtime/glue BETWEEN the
 * published regions with no recorded-insn increment, so the recorded cap alone never trips
 * on glue. This bounds total steps (mirrors the descent path's DESCEND_HARD_STEP_CAP) so a
 * runaway managed window self-truncates in bounded wall time rather than stepping forever. */
#ifndef PTRACE_WINDOW_STEP_CAP
#define PTRACE_WINDOW_STEP_CAP (1u << 22) /* ~4.2M steps */
#endif

/* Exported (non-inline) address-channel shims so language bindings that cannot call
 * the header-only inline API (asmtest_addr_channel.h) can still create, publish into,
 * and free a channel for the windowed capture. Defined unconditionally (the channel is
 * plain memory — it works even where the stepper itself is ENOSYS). A channel made here
 * is process-local heap: enough for the fork-internal asmtest_ptrace_trace_window_call,
 * where the caller pre-publishes the regions and the forked child inherits a copy it
 * never writes. For a LIVE producer publishing while a SEPARATE stepper drains, map the
 * struct in shared memory instead (asmtest_ptrace_trace_attached_windowed). */
asmtest_addr_channel_t *asmtest_addr_channel_new(void) {
    asmtest_addr_channel_t *c = (asmtest_addr_channel_t *)malloc(sizeof *c);
    if (c != NULL)
        asmtest_addr_channel_init(c);
    return c;
}
void asmtest_addr_channel_publish_rec(asmtest_addr_channel_t *c, uint64_t base,
                                      uint64_t len, uint64_t version) {
    asmtest_addr_channel_publish(c, base, len, version);
}
void asmtest_addr_channel_free(asmtest_addr_channel_t *c) { free(c); }

/* SHARED-memory channel for the §D3 whole-window stepper: the producer (the runtime's
 * JIT listener) publishes into it WHILE a forked helper drains it live. MAP_SHARED so the
 * fork sees the producer's writes; the fork preserves the address, so the same pointer is
 * valid in both. Free with asmtest_addr_channel_free_shared (munmap, not free). */
asmtest_addr_channel_t *asmtest_addr_channel_new_shared(void) {
#if defined(__linux__)
    void *m = mmap(NULL, sizeof(asmtest_addr_channel_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED)
        return NULL;
    asmtest_addr_channel_init((asmtest_addr_channel_t *)m);
    return (asmtest_addr_channel_t *)m;
#else
    return NULL;
#endif
}
void asmtest_addr_channel_free_shared(asmtest_addr_channel_t *c) {
#if defined(__linux__)
    if (c != NULL)
        munmap(c, sizeof(asmtest_addr_channel_t));
#else
    (void)c;
#endif
}

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
    asmtest_blockseq_t seq = {0};
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off = stream[i];
        size_t l =
            asmtest_disas(PTRACE_TRACE_ARCH, base, len, base_ip, off, NULL, 0);
        if (asmtest_blockseq_boundary(&seq, off, l))
            trace_append_block(t, off);
        trace_append_insn(t, off);
        if (l == 0) {
            t->truncated = true;
            return;
        }
    }
    if (overflow)
        t->truncated = true;
}

/* Blocking waitpid for the initial post-fork PTRACE_TRACEME handshake (the child's
 * raise(SIGSTOP)), retrying across EINTR. The tracee stops essentially immediately,
 * so the ONLY reason this wait returns EINTR is an UNRELATED signal delivered to the
 * tracer — a host process's own repeating timer / alarm, or a test's signal storm.
 * The bare waitpid used to treat that EINTR as a failed handshake and abort the whole
 * trace with ETRACE (racy: it fired ~70% of the time on a fast box under a 200us
 * SIGALRM storm). The step loop already retries across EINTR; the handshake must too,
 * so descent honours its "undisturbed by the caller's signal flow" contract from the
 * very first wait. (A real child-death still surfaces: waitpid returns the exit status
 * and !WIFSTOPPED, or -1 with errno!=EINTR.) */
static pid_t waitpid_handshake(pid_t pid, int *status) {
    for (;;) {
        pid_t w = waitpid(pid, status, 0);
        if (w >= 0 || errno != EINTR)
            return w;
    }
}

/* Plant a software breakpoint at `target`, PTRACE_CONT the tracee until it reaches it
 * AT THE MATCHING STACK DEPTH, remove the breakpoint, and (x86) rewind the PC so the
 * tracee is left stopped exactly at `target`. PTRACE_POKETEXT patches even an r-x text
 * page the way a debugger does (process_vm_writev would be refused on it). Splices the
 * breakpoint into the low PTRACE_BP_LEN bytes of the peeked word, preserving the rest.
 * Returns OK (stopped at target), ENOENT (the tracee exited before reaching it), or
 * ETRACE. Shared by asmtest_ptrace_run_to (run an attached target to a resolved method)
 * and the trace loops (run native-speed OVER a call-out to its return address).
 * Unrelated signals are forwarded so the tracee's own signal flow is undisturbed.
 * `first_sig` (0 for the common case) is attached to the FIRST PTRACE_CONT — used to
 * deliver a SIGTRAP the caller already reaped (an application int3/breakpoint) rather
 * than swallow it. `expected_sp` (0 = today's first-arrival behaviour, for
 * asmtest_ptrace_run_to and descent's own already-SP-guarded pop predicate) rejects a
 * hit at the right address but the WRONG depth — a call-out helper that recurses into
 * or calls back through the return-address breakpoint hits it first from a deeper
 * frame; accepting that hit resumes the trace inside the wrong invocation. On a
 * depth mismatch the breakpoint's own instruction is stepped over natively (never
 * re-armed with a signal attached — this is our own step, not signal delivery) and
 * the wait resumes. */
static int run_until_sp(pid_t pid, uint64_t target, uint64_t expected_sp,
                        int first_sig) {
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
    long planted = 0;
    const int force_hw = getenv("ASMTEST_PTRACE_HW_BP") != NULL;

    if (!force_hw) {
        errno = 0;
        orig = ptrace(PTRACE_PEEKTEXT, pid, (void *)(uintptr_t)target, NULL);
        if (orig == -1 && errno != 0)
            return ASMTEST_PTRACE_ETRACE;
        const unsigned long mask = PTRACE_BP_LEN >= (int)sizeof(long)
                                       ? ~0UL
                                       : ((1UL << (PTRACE_BP_LEN * 8)) - 1);
        planted =
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

    int rc = ASMTEST_PTRACE_OK, status = 0, sig = first_sig;
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
        uint64_t pc, sp;
        if (read_pc_ret(pid, &pc, NULL, &sp, NULL) != 0) {
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
        if (hit != target) {
            /* Not our breakpoint. A planted software int3 ALSO reports SI_KERNEL, so
             * the address match above must be tested first (the asmspy ordering) —
             * only a miss here can be the tracee's own app breakpoint. Forward it via
             * the next PTRACE_CONT rather than silently discard it; anything else
             * (unrelated trap noise) keeps the prior silent-continue behaviour. */
            if (bs_sigtrap_is_app(pid))
                sig = SIGTRAP;
            continue;
        }

        if (expected_sp != 0 && sp != expected_sp) {
            /* Right address, wrong depth: a re-entrant call-out (recursion, or a
             * callback into the traced region) hit this same return-address
             * breakpoint from a deeper frame first. Do not accept the hit — step
             * past it at native cost and keep waiting for the matching depth. */
            if (hw) {
                /* The CPU sets EFLAGS.RF on debug-exception entry, so a plain CONT
                 * does not immediately re-trap on this same instruction. */
                continue;
            }
            if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target,
                       (void *)orig) != 0) {
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
#if defined(__x86_64__)
            if (set_pc(pid, target) != 0) {
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
#endif
            if (ptrace(PTRACE_SINGLESTEP, pid, NULL, NULL) != 0) {
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
            if (waitpid(pid, &status, 0) < 0) {
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                rc = ASMTEST_PTRACE_ENOENT;
                break;
            }
            if (WIFSTOPPED(status) && WSTOPSIG(status) != SIGTRAP)
                sig = WSTOPSIG(status); /* forward on the next CONT */
            if (ptrace(PTRACE_POKETEXT, pid, (void *)(uintptr_t)target,
                       (void *)planted) != 0) {
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
            continue;
        }

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

static int run_until_sig(pid_t pid, uint64_t target, int first_sig) {
    return run_until_sp(pid, target, 0, first_sig);
}

static int run_until(pid_t pid, uint64_t target) {
    return run_until_sig(pid, target, 0);
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
 * the previous behaviour). `entry_sp` is the stack pointer at the callee's entry — the
 * current stop, already read by the caller — from which the expected post-return depth
 * is computed and enforced by run_until_sp, so a helper that recurses into or calls back
 * through the region's own return-address breakpoint does not hijack the resume (the
 * same call_sp/sp_ret arithmetic the descent shadow stack relies on, see dframe_t
 * below). */
static exit_kind_t classify_region_exit(pid_t pid, const uint8_t *code,
                                        size_t len, uint64_t base_ip,
                                        uint64_t last_off, uint64_t entry_sp,
                                        uint64_t *resume_off) {
    if (!asmtest_disas_available() ||
        !asmtest_disas_is_call(PTRACE_TRACE_ARCH, code, len, last_off))
        return EXIT_RETURNED;
    size_t cl =
        asmtest_disas(PTRACE_TRACE_ARCH, code, len, base_ip, last_off, NULL, 0);
    uint64_t ret_off = last_off + cl;
    if (cl == 0 || ret_off >= len)
        return EXIT_RETURNED; /* the call's fall-through is outside the region */
#if defined(__x86_64__)
    uint64_t expected_sp =
        entry_sp + 8; /* `call` pushed the return address; `ret` pops it */
#else
    uint64_t expected_sp =
        entry_sp; /* aarch64 `bl` writes the link register, no push */
#endif
    if (run_until_sp(pid, base_ip + ret_off, expected_sp, 0) !=
        ASMTEST_PTRACE_OK)
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
/* See docs/internal/archive/plans/call-descent-plan.md (Correctness core).           */
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

/* L3 (DESCEND_ALL) call-out extent resolution: /proc/<pid>/maps parsed ONCE per descent
 * into a flat array of executable [start,end) ranges, instead of once per call-out (a
 * call-out-heavy L3 descent used to re-parse the whole maps file on every single
 * descend_decide, dominating profile time). A lookup MISS re-parses once and retries — a
 * JIT may map new code mid-descent — then reports miss (step over) exactly as before. */
typedef struct {
    uint64_t start, end;
} descend_maps_range_t;

typedef struct {
    descend_maps_range_t *ranges;
    size_t len, cap;
    int built;
} descend_maps_cache_t;

static uint64_t
    descend_maps_parse_count; /* T5 test hook: real maps-file parses */

uint64_t asmtest_descend_maps_parse_count(void) {
    return descend_maps_parse_count;
}
void asmtest_descend_maps_parse_count_reset(void) {
    descend_maps_parse_count = 0;
}

static void descend_maps_cache_parse(descend_maps_cache_t *mc, pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return;
    mc->len = 0;
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) != -1) {
        unsigned long start, end;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3 ||
            perms[2] != 'x')
            continue;
        if (mc->len == mc->cap) {
            size_t ncap = mc->cap ? mc->cap * 2 : 16;
            descend_maps_range_t *nr =
                (descend_maps_range_t *)realloc(mc->ranges, ncap * sizeof *nr);
            if (nr == NULL)
                break;
            mc->ranges = nr;
            mc->cap = ncap;
        }
        mc->ranges[mc->len].start = (uint64_t)start;
        mc->ranges[mc->len].end = (uint64_t)end;
        mc->len++;
    }
    free(line);
    fclose(f);
    mc->built = 1;
    descend_maps_parse_count++;
}

static int descend_maps_cache_find(const descend_maps_cache_t *mc,
                                   uint64_t addr, uint64_t *base_out,
                                   uint64_t *len_out) {
    for (size_t i = 0; i < mc->len; i++)
        if (addr >= mc->ranges[i].start && addr < mc->ranges[i].end) {
            if (base_out)
                *base_out = mc->ranges[i].start;
            if (len_out)
                *len_out = mc->ranges[i].end - mc->ranges[i].start;
            return 1;
        }
    return 0;
}

static int descend_maps_cache_lookup(descend_maps_cache_t *mc, pid_t pid,
                                     uint64_t addr, uint64_t *base_out,
                                     uint64_t *len_out) {
    if (!mc->built)
        descend_maps_cache_parse(mc, pid);
    if (descend_maps_cache_find(mc, addr, base_out, len_out))
        return 1;
    descend_maps_cache_parse(mc, pid); /* MISS: maybe a JIT mapped new code */
    return descend_maps_cache_find(mc, addr, base_out, len_out);
}

static void descend_maps_cache_free(descend_maps_cache_t *mc) {
    free(mc->ranges);
    mc->ranges = NULL;
    mc->len = mc->cap = 0;
    mc->built = 0;
}

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
    descend_maps_cache_t
        maps_cache; /* L3 call-out extent resolution, see above */
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
        ssize_t got = ptrace_read_mem(c->pid, buf, (void *)(uintptr_t)at, want);
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
    if (!descend_maps_cache_lookup(&c->maps_cache, c->pid, callee, cbase, clen))
        return 0; /* unknown extent -> step over */
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
 * L3 (see the arm site), so L1/L2 leave the caller's signal state untouched.
 *
 * The "single-descent-at-a-time" assumption above used to be just a comment: two L3
 * descents overlapping in the same process would silently clobber each other's
 * ITIMER_REAL/SIGALRM disposition on arm and, on disarm, restore the WRONG saved state.
 * `descend_active` makes the assumption load-bearing: a second arm while one is already
 * active is REFUSED (returns 0, timer/handler left exactly as the first descent set them)
 * instead of silently clashing; the refused descent still terminates correctly — the
 * per-step CLOCK_MONOTONIC deadline check bounds it even with no watchdog — it is just
 * marked truncated + depth_capped up front (honest degradation, not a silent clash). */
static volatile sig_atomic_t descend_alarm_fired;
static volatile sig_atomic_t descend_active;
static void descend_alarm_handler(int sig) {
    (void)sig;
    descend_alarm_fired = 1;
}
static int descend_watchdog_arm(uint32_t ms, struct sigaction *saved_sa,
                                struct itimerval *saved_it) {
    if (descend_active)
        return 0; /* refused: another descent already owns the timer */
    descend_active = 1;
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
    return 1;
}
static void descend_watchdog_disarm(const struct sigaction *saved_sa,
                                    const struct itimerval *saved_it) {
    setitimer(ITIMER_REAL, saved_it, NULL);
    sigaction(SIGALRM, saved_sa, NULL);
    descend_active = 0;
}

/* T5 test hooks: thin wrappers so a test can drive "arm; arm again (refused); disarm"
 * directly, without two genuinely concurrent tracees. */
int asmtest_descend_watchdog_arm_test(struct sigaction *saved_sa,
                                      struct itimerval *saved_it, uint32_t ms) {
    return descend_watchdog_arm(ms, saved_sa, saved_it);
}
void asmtest_descend_watchdog_disarm_test(const struct sigaction *saved_sa,
                                          const struct itimerval *saved_it) {
    descend_watchdog_disarm(saved_sa, saved_it);
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
    if (watchdog &&
        !descend_watchdog_arm(d->watchdog_ms, &saved_sa, &saved_it)) {
        /* A second L3 descent arrived while one is already active: run WITHOUT the
         * ITIMER_REAL watchdog (the per-step deadline check below still bounds it) and
         * mark the honest degradation up front rather than silently clashing timers. */
        watchdog = 0;
        asmtest_descent_mark_truncated(d);
        asmtest_descent_mark_depth_capped(d);
    }

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

        if (bs_sigtrap_is_app(c->pid)) {
            /* The tracee executed its OWN int3 (a JVM safepoint poll, a .NET
             * breakpoint) — descent cannot safely SINGLESTEP across a signal
             * delivery (measured fatal: the re-armed trap fires inside a masked
             * handler), so the descent ends here, honestly, rather than mis-attribute
             * a handler's instructions to the traced region. */
            asmtest_descent_mark_truncated(d);
            if (c->flat)
                c->flat->truncated = true;
            if (!c->forward_faults) {
                /* Fork-owned: we hold the only reference to this SIGTRAP; forward it
                 * via PTRACE_CONT (never SINGLESTEP with a signal attached), then
                 * reap — same policy as the per-instruction region driver. */
                if (ptrace(PTRACE_CONT, c->pid, NULL,
                           (void *)(uintptr_t)SIGTRAP) == 0 &&
                    waitpid(c->pid, &status, 0) >= 0) {
                    if (WIFEXITED(status) || WIFSIGNALED(status))
                        c->reaped = 1;
                    else if (WIFSTOPPED(status)) {
                        kill(c->pid, SIGKILL);
                        waitpid(c->pid, &status, 0);
                        c->reaped = 1;
                    }
                }
            }
            /* Foreign (c->forward_faults): leave the target in its SIGTRAP
             * signal-delivery stop — the caller owns detach/signal policy. */
            rc = ASMTEST_PTRACE_ETRACE;
            break;
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
    descend_maps_cache_free(&c->maps_cache);
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

    if (waitpid_handshake(pid, &status) < 0 || !WIFSTOPPED(status)) {
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

        if (bs_sigtrap_is_app(pid)) {
            /* The tracee executed its OWN int3 (a JVM safepoint poll, a .NET
             * breakpoint). Nothing to back out — the stop PC was already recorded
             * pre-execution at the previous stop. Never PTRACE_SINGLESTEP with the
             * signal attached (measured fatal): forward it via PTRACE_CONT and reap,
             * the same dance the EXIT_RETURNED path below already uses. */
            overflow = 1;
            ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)SIGTRAP);
            if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status)) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            break;
        }

        uint64_t pc, retval, sp;
        if (read_pc_ret(pid, &pc, &retval, &sp, NULL) != 0) {
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
                                     stream[n - 1], sp, &resume_off);
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
/* docs/internal/plans/amd-tracing-plan.md "BTF block-step is available on x86". */
/* ------------------------------------------------------------------ */
#if defined(__x86_64__)

#ifndef PTRACE_SINGLEBLOCK
#define PTRACE_SINGLEBLOCK                                                     \
    33 /* <sys/ptrace.h> omits it though the kernel wires it */
#endif

/* Hang-proof one-shot FUNCTIONAL probe: PTRACE_SINGLEBLOCK must run a straight-line
 * block to its terminating branch in ONE step, not merely advance the child. Some
 * hypervisors mask DEBUGCTL.BTF (GitHub-hosted runners do), silently degrading
 * SINGLEBLOCK to per-instruction TF stepping — the child moves, so a did-it-advance
 * check passes, but reconstruction then sees a stop after every insn and emits
 * overlapping walks. It is also unwired outright under some sandboxes/emulators
 * (returns EIO). So: the child int3-traps at the head of an mmap'd nop run + ret;
 * from that stop (RIP = blob+1) one SINGLEBLOCK must leave the blob entirely (BTF
 * stops at the ret's TARGET, back in the caller). Stopping inside the nop run is
 * the degraded single-step signature -> unavailable. Mirrors probe_singlestep's
 * WNOHANG-with-deadline shape so it can never hang CI, and caches. */
static int wait_stop_sigtrap(pid_t pid) {
    int st;
    for (int i = 0; i < 200; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid)
            return WIFSTOPPED(st) && WSTOPSIG(st) == SIGTRAP;
        if (w < 0)
            return 0;
        struct timespec ts = {0, 1000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

static int probe_singleblock(void) {
    static const uint8_t blob[] = {0xCC, 0x90, 0x90, 0x90,
                                   0x90, 0x90, 0xC3}; /* int3; 5x nop; ret */
    void *p = mmap(NULL, sizeof blob, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    memcpy(p, blob, sizeof blob);
    pid_t pid = fork();
    if (pid < 0) {
        munmap(p, sizeof blob);
        return 0;
    }
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        ((void (*)(void))p)(); /* int3: SIGTRAP stop with RIP = blob+1 */
        _exit(0);
    }
    int functional = 0;
    uint64_t pc = 0;
    if (wait_stop_sigtrap(pid) &&
        read_pc_ret(pid, &pc, NULL, NULL, NULL) == 0 &&
        pc == (uint64_t)(uintptr_t)p + 1 &&
        ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) == 0 &&
        wait_stop_sigtrap(pid) && read_pc_ret(pid, &pc, NULL, NULL, NULL) == 0)
        functional = pc < (uint64_t)(uintptr_t)p ||
                     pc >= (uint64_t)(uintptr_t)p + sizeof blob;
    int st;
    kill(pid, SIGKILL);
    waitpid(pid, &st, 0);
    munmap(p, sizeof blob);
    return functional;
}

int asmtest_ptrace_blockstep_available(void) {
    static int cached = -1;
    if (cached < 0)
        cached = probe_singleblock() && asmtest_disas_available();
    return cached;
}

/* Record the straight-line run [from_off, term_off] into `stream`. Every instruction is
 * in-region by construction here (the region form's snapshot IS the region). Sets
 * *rep_out (may be NULL) if any recorded instruction is a `rep`-prefixed string op — it
 * retires N times but is recorded ONCE, so the caller must mark the capture truncated. */
static int bs_record_run(const uint8_t *code, size_t len, uint64_t from_off,
                         uint64_t term_off, uint64_t *stream, uint32_t *pn,
                         uint64_t *last_off, int *rep_out) {
    uint64_t walk = from_off;
    for (size_t guard = 0; guard <= len; guard++) {
        if (walk >= len || walk > term_off)
            return 0;
        if (*pn >= PTRACE_STREAM_CAP)
            return 0; /* stream full */
        stream[(*pn)++] = walk;
        *last_off = walk;
        if (rep_out != NULL &&
            asmtest_disas_is_rep_string(PTRACE_TRACE_ARCH, code, len, walk))
            *rep_out = 1;
        size_t l =
            asmtest_disas(PTRACE_TRACE_ARCH, code, len, 0, walk, NULL, 0);
        if (l == 0)
            return 0;
        if (walk == term_off)
            return 1;
        walk += l;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* T7 — IBS covered-block pre-cover: memoize blockstep_reconstruct's decode.       */
/* See include/asmtest_blockstep_internal.h. Region-relative leader offsets only, */
/* so one table serves both the region and attached drivers (both index by the   */
/* same region-relative from_off). Everything here is pure Capstone decode over  */
/* the SAME primitives classify_branch uses — a cache hit is provably what a     */
/* fresh scan would have decided, because it comes from the same bytes via the   */
/* same calls, just done once instead of on every #DB stop.                      */
/* ------------------------------------------------------------------ */

/* Facts about one instruction of a cached run that a fresh classify_branch(...,
 * next_pc,...) call would recompute — everything EXCEPT the next_pc-dependent
 * hit/miss verdict, so one cached run answers for any observed next_pc. */
typedef enum {
    BS_PC_NONE = 0,     /* not a branch: the run continues through it        */
    BS_PC_HARD_UNKNOWN, /* ret / indirect: always taken, no static target     */
    BS_PC_UNCOND,       /* direct call/jmp: always taken, static target known */
    BS_PC_COND,         /* conditional direct branch: static target known    */
} bs_pc_kind_t;

typedef struct {
    uint64_t off;
    size_t len;
    bs_pc_kind_t kind;
    uint64_t
        target; /* meaningful iff kind == BS_PC_UNCOND || kind == BS_PC_COND */
    int is_rep;
} bs_precover_insn_t;

typedef struct {
    uint64_t
        leader; /* region-relative block-start offset (matches a from_off) */
    bs_precover_insn_t *insns;
    size_t n;
} bs_precover_run_t;

struct asmtest_bs_precover {
    bs_precover_run_t
        *runs; /* ascending by leader (covered->blocks is ascending;
                              * build only ever skips entries, never reorders) */
    size_t n;
};

static const asmtest_bs_precover_t *g_bs_precover_current = NULL;
static uint64_t g_bs_precover_hits = 0;

void asmtest_bs_precover_set_current(const asmtest_bs_precover_t *p) {
    g_bs_precover_current = p;
}

void asmtest_bs_stats(uint64_t *probe_calls, uint64_t *precover_hits) {
    if (probe_calls != NULL)
        *probe_calls = asmtest_bs_recon_probe_calls();
    if (precover_hits != NULL)
        *precover_hits = g_bs_precover_hits;
}

void asmtest_bs_stats_reset(void) {
    asmtest_bs_recon_probe_calls_reset();
    g_bs_precover_hits = 0;
}

asmtest_bs_precover_t *
asmtest_bs_precover_build(const uint8_t *code, size_t len, uint64_t base_ip,
                          const asmtest_ibs_blocks_t *covered) {
    if (code == NULL || len == 0 || covered == NULL || covered->n == 0)
        return NULL;
    asmtest_bs_precover_t *p = (asmtest_bs_precover_t *)calloc(1, sizeof *p);
    if (p == NULL)
        return NULL;
    p->runs = (bs_precover_run_t *)calloc(covered->n, sizeof *p->runs);
    if (p->runs == NULL) {
        free(p);
        return NULL;
    }

    for (size_t i = 0; i < covered->n; i++) {
        uint64_t leader = covered->blocks[i].start;
        if (leader >= len)
            continue; /* out of range: never a real from_off, dead weight if kept */

        bs_precover_insn_t *tmp = NULL;
        size_t tn = 0, tcap = 0;
        uint64_t walk = leader;
        for (size_t guard = 0; guard <= len; guard++) {
            if (walk >= len)
                break;
            int is_call = 0, is_ret = 0;
            size_t l = asmtest_disas_probe(PTRACE_TRACE_ARCH, code, len, walk,
                                           &is_call, &is_ret);
            if (l == 0)
                break; /* undecodable: run ends here, incomplete but honest */
            bs_pc_kind_t kind = BS_PC_NONE;
            uint64_t target = 0;
            if (asmtest_disas_is_branch(PTRACE_TRACE_ARCH, code, len, walk)) {
                if (is_ret) {
                    kind = BS_PC_HARD_UNKNOWN;
                } else if (!asmtest_disas_branch_target(PTRACE_TRACE_ARCH, code,
                                                        len, base_ip, walk,
                                                        &target)) {
                    kind = BS_PC_HARD_UNKNOWN; /* indirect jmp/call */
                } else if (is_call || asmtest_disas_is_uncond_jump(
                                          PTRACE_TRACE_ARCH, code, len, walk)) {
                    kind = BS_PC_UNCOND;
                } else {
                    kind = BS_PC_COND;
                }
            }
            if (tn == tcap) {
                size_t ncap = tcap ? tcap * 2 : 8;
                bs_precover_insn_t *grown =
                    (bs_precover_insn_t *)realloc(tmp, ncap * sizeof *tmp);
                if (grown == NULL)
                    break;
                tmp = grown;
                tcap = ncap;
            }
            tmp[tn].off = walk;
            tmp[tn].len = l;
            tmp[tn].kind = kind;
            tmp[tn].target = target;
            tmp[tn].is_rep =
                asmtest_disas_is_rep_string(PTRACE_TRACE_ARCH, code, len, walk);
            tn++;
            if (kind == BS_PC_HARD_UNKNOWN || kind == BS_PC_UNCOND)
                break; /* always-taken: nothing past it can belong to this run */
            walk += l;
        }
        if (tn == 0) {
            free(tmp);
            continue; /* leader itself undecodable: never cached (hostile leader) */
        }
        p->runs[p->n].leader = leader;
        p->runs[p->n].insns = tmp;
        p->runs[p->n].n = tn;
        p->n++;
    }
    if (p->n == 0) {
        free(p->runs);
        free(p);
        return NULL;
    }
    return p;
}

void asmtest_bs_precover_free(asmtest_bs_precover_t *p) {
    if (p == NULL)
        return;
    for (size_t i = 0; i < p->n; i++)
        free(p->runs[i].insns);
    free(p->runs);
    free(p);
}

static const bs_precover_run_t *bs_precover_find(const asmtest_bs_precover_t *p,
                                                 uint64_t from_off) {
    if (p == NULL)
        return NULL;
    size_t lo = 0, hi = p->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->runs[mid].leader == from_off)
            return &p->runs[mid];
        if (p->runs[mid].leader < from_off)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

/* Replays asmtest_bs_scan_terminator's exact decision procedure (see bs_recon.c) over
 * a cached run instead of re-decoding: the run's instructions were walked in the same
 * order classify_branch would visit them, so this reproduces the same OK/AMBIGUOUS/FAIL
 * verdict and terminator offset for any next_pc, with zero Capstone calls. */
static int bs_precover_scan_terminator(const bs_precover_run_t *run,
                                       uint64_t next_pc, uint64_t *term_out) {
    uint64_t first_cand = 0;
    unsigned ncand = 0;
    for (size_t i = 0; i < run->n; i++) {
        const bs_precover_insn_t *e = &run->insns[i];
        if (e->kind == BS_PC_NONE)
            continue;
        if (e->kind == BS_PC_COND) {
            if (e->target == next_pc && ncand++ == 0)
                first_cand = e->off;
            continue;
        }
        if (e->kind == BS_PC_HARD_UNKNOWN) {
            if (ncand == 0) {
                *term_out = e->off;
                return ASMTEST_BS_OK;
            }
            break;
        }
        /* BS_PC_UNCOND: always taken. */
        if (e->target == next_pc && ncand++ == 0)
            first_cand = e->off;
        break;
    }
    if (ncand == 0)
        return ASMTEST_BS_FAIL;
    *term_out = first_cand;
    return ncand == 1 ? ASMTEST_BS_OK : ASMTEST_BS_AMBIGUOUS;
}

/* Emits the stream from a cache hit: identical shape to blockstep_reconstruct, but
 * every offset/length/rep-flag comes from the cached run instead of asmtest_disas. */
static int blockstep_reconstruct_precover(const bs_precover_run_t *run,
                                          uint64_t next_pc, uint64_t *stream,
                                          uint32_t *pn, uint64_t *last_off) {
    uint64_t term = 0;
    int r = bs_precover_scan_terminator(run, next_pc, &term);
    if (r == ASMTEST_BS_FAIL)
        return ASMTEST_BS_FAIL;
    int rep = 0;
    for (size_t i = 0; i < run->n; i++) {
        const bs_precover_insn_t *e = &run->insns[i];
        if (*pn >= PTRACE_STREAM_CAP)
            return ASMTEST_BS_FAIL;
        stream[(*pn)++] = e->off;
        *last_off = e->off;
        if (e->is_rep)
            rep = 1;
        if (e->off == term) {
            if (rep && r == ASMTEST_BS_OK)
                r = ASMTEST_BS_AMBIGUOUS;
            return r;
        }
    }
    return ASMTEST_BS_FAIL; /* unreachable: term is always one of run's offsets */
}

/* Reconstruct the straight-line run of one basic block into `stream`. Returns ASMTEST_BS_OK
 * (with *last_off = the terminator's offset), ASMTEST_BS_AMBIGUOUS (the definite prefix is
 * recorded, *last_off = its last instruction, caller marks truncated and must not treat
 * *last_off as a real terminator), or ASMTEST_BS_FAIL (caller marks truncated). */
static int blockstep_reconstruct(const uint8_t *code, size_t len,
                                 uint64_t base_ip, uint64_t from_off,
                                 uint64_t next_pc, uint64_t *stream,
                                 uint32_t *pn, uint64_t *last_off) {
    const bs_precover_run_t *cached =
        bs_precover_find(g_bs_precover_current, from_off);
    if (cached != NULL) {
        g_bs_precover_hits++;
        return blockstep_reconstruct_precover(cached, next_pc, stream, pn,
                                              last_off);
    }
    uint64_t term = 0;
    int r = asmtest_bs_scan_terminator(PTRACE_TRACE_ARCH, code, len, base_ip,
                                       from_off, next_pc, &term);
    if (r == ASMTEST_BS_FAIL)
        return ASMTEST_BS_FAIL;
    int rep = 0;
    if (!bs_record_run(code, len, from_off, term, stream, pn, last_off, &rep))
        return ASMTEST_BS_FAIL;
    /* A rep-prefixed string op retires N times but was recorded ONCE: the stream can
     * no longer be byte-identical to per-instruction stepping, so downgrade an
     * otherwise-clean block to AMBIGUOUS (the caller marks truncated, keeps tracing). */
    if (rep && r == ASMTEST_BS_OK)
        r = ASMTEST_BS_AMBIGUOUS;
    return r;
}

/* Record the straight-line run [from_off, cut_off) into `stream`, requiring the walk
 * to land EXACTLY on cut_off. Used when the tracee's OWN int3 traps mid-block: the
 * trap-stop's PC is one past the 0xCC byte, so the executed run — the int3 included —
 * ends exactly at cut_off. Any overshoot (cut_off fell inside an instruction) or an
 * undecodable byte returns ASMTEST_BS_FAIL. The region-buffer analog of window_block_walk's
 * at_mode cut; BTF cannot bridge the kernel-injected transfer into the handler, so the
 * caller marks the capture truncated. */
static int blockstep_reconstruct_cut(const uint8_t *code, size_t len,
                                     uint64_t from_off, uint64_t cut_off,
                                     uint64_t *stream, uint32_t *pn,
                                     uint64_t *last_off) {
    uint64_t walk = from_off;
    for (size_t guard = 0; guard <= len; guard++) {
        if (walk == cut_off)
            return ASMTEST_BS_OK; /* landed exactly on the trap PC */
        if (walk > cut_off || walk >= len)
            return ASMTEST_BS_FAIL;
        if (*pn >= PTRACE_STREAM_CAP)
            return ASMTEST_BS_FAIL;
        stream[(*pn)++] = walk;
        *last_off = walk;
        size_t l =
            asmtest_disas(PTRACE_TRACE_ARCH, code, len, 0, walk, NULL, 0);
        if (l == 0)
            return ASMTEST_BS_FAIL;
        walk += l;
    }
    return ASMTEST_BS_FAIL;
}

int asmtest_ptrace_trace_call_blockstep(const void *code, size_t len,
                                        const long *args, int nargs,
                                        long *result, asmtest_trace_t *trace) {
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
    uint64_t prev_off =
        SENTINEL; /* start of the open (not-yet-terminated) block */
    uint32_t n = 0;
    int overflow = 0, returned = 0, rc = ASMTEST_PTRACE_OK, status = 0;
    int pending_sig = 0;

    if (waitpid_handshake(pid, &status) < 0 || !WIFSTOPPED(status)) {
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

        uint64_t pc, retval, sp;
        if (read_pc_ret(pid, &pc, &retval, &sp, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        int in_region = (pc >= base_ip && pc < base_ip + len);

        if (bs_sigtrap_is_app(pid)) {
            /* The TARGET executed its OWN int3 (a JVM safepoint poll, a .NET
             * breakpoint — the tier's stated managed-runtime target), NOT a BTF
             * block completion. BTF cannot bridge the kernel-injected transfer into
             * the handler, so record the executed run up to the trap (the int3
             * included) and truncate honestly — never fabricate the instructions
             * after it. Forward the signal to the tracee via PTRACE_CONT: NEVER
             * SINGLEBLOCK/SINGLESTEP with the signal attached (measured fatal — the
             * re-armed trap fires inside the masked handler). With no handler the
             * default SIGTRAP terminates the child (the fixture case). */
            overflow = 1;
            if (prev_off != SENTINEL && in_region) {
                uint64_t cut_last = 0;
                blockstep_reconstruct_cut((const uint8_t *)code, len, prev_off,
                                          pc - base_ip, stream, &n, &cut_last);
            }
            if (ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)SIGTRAP) !=
                0) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                rc = ASMTEST_PTRACE_ETRACE;
                break;
            }
            if (waitpid(pid, &status, 0) >= 0 && WIFSTOPPED(status)) {
                /* A handler ran and re-stopped the child; we are already truncated,
                 * so reap and stop rather than resume into post-handler code. */
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            }
            break;
        }

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
        int br = blockstep_reconstruct((const uint8_t *)code, len, base_ip,
                                       prev_off, pc, stream, &n, &last_off);
        if (br == ASMTEST_BS_FAIL) {
            /* Stream full / undecodable / no in-region terminator: the child is
             * still ptrace-stopped and owned here, and rc stays OK so the
             * post-loop cleanup will not reap it — kill+reap now, as the other
             * overflow breaks do (PTRACE_O_EXITKILL only fires on tracer exit). */
            overflow = 1;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        if (br == ASMTEST_BS_AMBIGUOUS) {
            /* The definite prefix is recorded and the capture is truncated. We still
             * know exactly where the tracee IS (`pc`), so keep tracing from there —
             * an honest gap beats abandoning the rest. But `last_off` is only the
             * prefix's end, not a real terminator, so it must NOT be fed to
             * classify_region_exit: if the block also left the region, stop here. */
            overflow = 1;
            if (!in_region) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                break;
            }
        }

        if (in_region) {
            prev_off =
                pc - base_ip; /* the block's target opens the next block */
            continue;
        }

        /* The block left the region: routine return or a call-out to a helper. Reuse
         * the single-step classifier (it plants a breakpoint at the call's return and
         * runs the callee at native speed), so a method calling runtime helpers still
         * traces, not just a pure leaf. */
        uint64_t resume_off = 0;
        exit_kind_t k =
            classify_region_exit(pid, (const uint8_t *)code, len, base_ip,
                                 last_off, sp, &resume_off);
        if (k == EXIT_CALLOUT_RESUMED) {
            prev_off =
                resume_off; /* resume block-stepping from the call's return */
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

/* Block-step variant of asmtest_ptrace_trace_attached: block-step a SEPARATE,
 * already-ptrace-stopped process from its current stop — one #DB per TAKEN branch
 * instead of one per instruction — reconstructing the same per-instruction stream.
 * Same contract as the per-instruction attached tracer: the caller owns
 * attach/detach, the target's bytes are read via process_vm_readv, the target is
 * NEVER killed (it is foreign), and on a region return it is left stopped past the
 * region for the caller. The rootless managed-runtime path at a fraction of the
 * stops (the completeness fallback the AMD plan's Phase 2 scopes). */
int asmtest_ptrace_trace_attached_blockstep(pid_t pid, const void *base,
                                            size_t len, long *result,
                                            asmtest_trace_t *trace) {
    if (base == NULL || len == 0 || trace == NULL)
        return ASMTEST_PTRACE_EINVAL;
    if (!asmtest_ptrace_blockstep_available())
        return ASMTEST_PTRACE_ENOSYS;

    /* Foreign target: read the region bytes debugger-style. */
    uint8_t *code = (uint8_t *)malloc(len);
    if (code == NULL)
        return ASMTEST_PTRACE_ETRACE;
    if (ptrace_read_mem(pid, code, (void *)(uintptr_t)base, len) !=
        (ssize_t)len) {
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
    const uint64_t SENTINEL = ~(uint64_t)0;
    uint64_t prev_off =
        SENTINEL; /* start of the open (not-yet-terminated) block */
    uint32_t n = 0;
    int overflow = 0, rc = ASMTEST_PTRACE_OK, status = 0;

    /* Both entry conventions of the per-instruction attached tracer: stopped
     * EXACTLY at the region entry (asmtest_ptrace_run_to) — the current PC opens
     * the first block; stopped before the region — block-step through the glue at
     * native branch speed until the first in-region stop. */
    {
        uint64_t pc0, ret0;
        if (read_pc_ret(pid, &pc0, &ret0, NULL, NULL) != 0) {
            free(code);
            free(stream);
            return ASMTEST_PTRACE_ETRACE;
        }
        if (pc0 >= base_ip && pc0 < base_ip + len)
            prev_off = pc0 - base_ip;
    }

    for (;;) {
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) != 0) {
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
            /* Same policy as the per-instruction attached loop: a non-trap signal
             * mid-region truncates; the caller owns signal policy for its target. */
            if (prev_off != SENTINEL)
                overflow = 1;
            break;
        }

        uint64_t pc, retval, sp;
        if (read_pc_ret(pid, &pc, &retval, &sp, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        int in_region = (pc >= base_ip && pc < base_ip + len);

        if (bs_sigtrap_is_app(pid)) {
            /* The foreign target executed its OWN int3 (a JVM safepoint poll, a
             * .NET breakpoint), NOT a BTF block completion. Record the executed run
             * up to the trap (int3 included) and truncate honestly, then leave the
             * target in its SIGTRAP signal-delivery stop for the caller — it is
             * foreign and is NEVER killed. A caller that wants the target's own
             * breakpoint semantics to proceed detaches with
             * PTRACE_DETACH(pid, 0, SIGTRAP). */
            overflow = 1;
            if (prev_off != SENTINEL && in_region) {
                uint64_t cut_last = 0;
                blockstep_reconstruct_cut(code, len, prev_off, pc - base_ip,
                                          stream, &n, &cut_last);
            }
            break;
        }

        if (prev_off == SENTINEL) {
            if (in_region)
                prev_off =
                    pc - base_ip; /* region entry opens the first block */
            continue;
        }

        /* Open block from prev_off terminated by a taken branch reaching `pc`:
         * reconstruct its straight-line run (trap-class #DB — pc is the TARGET). */
        uint64_t last_off = 0;
        int br = blockstep_reconstruct(code, len, base_ip, prev_off, pc, stream,
                                       &n, &last_off);
        if (br == ASMTEST_BS_FAIL) {
            overflow = 1;
            break;
        }
        if (br == ASMTEST_BS_AMBIGUOUS) {
            /* Honest gap: prefix recorded, capture truncated, keep tracing from the
             * stop we know. `last_off` is not a real terminator, so it must not reach
             * classify_region_exit — if the block also left the region, stop here.
             * The target is foreign: never killed, just left where it is. */
            overflow = 1;
            if (!in_region)
                break;
        }

        if (in_region) {
            prev_off =
                pc - base_ip; /* the block's target opens the next block */
            continue;
        }

        /* Left the region: a call-out to a helper (step over at native speed and
         * resume), or the routine's return (leave the target stopped there). */
        uint64_t resume_off = 0;
        exit_kind_t k = classify_region_exit(pid, code, len, base_ip, last_off,
                                             sp, &resume_off);
        if (k == EXIT_CALLOUT_RESUMED) {
            prev_off = resume_off;
            continue;
        }
        if (k == EXIT_CALLOUT_LOST) {
            overflow = 1; /* callee never returned to the region — truncate */
            break;
        }
        if (result != NULL)
            *result = (long)retval;
        break; /* region returned: target stays stopped for the caller */
    }

    if (rc == ASMTEST_PTRACE_OK)
        normalize(trace, code, base_ip, len, stream, n, overflow);
    free(stream);
    free(code);
    return rc;
}

#else  /* !__x86_64__ : no PTRACE_SINGLEBLOCK equivalent */
int asmtest_ptrace_blockstep_available(void) { return 0; }
int asmtest_ptrace_trace_call_blockstep(const void *code, size_t len,
                                        const long *args, int nargs,
                                        long *result, asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}
int asmtest_ptrace_trace_attached_blockstep(pid_t pid, const void *base,
                                            size_t len, long *result,
                                            asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

/* IBS is Linux+x86-64+AMD only (asmtest_ibs.h), so a covered-block set never exists
 * here: the build always reports "nothing to cache" rather than fabricate one. */
asmtest_bs_precover_t *
asmtest_bs_precover_build(const uint8_t *code, size_t len, uint64_t base_ip,
                          const asmtest_ibs_blocks_t *covered) {
    (void)code;
    (void)len;
    (void)base_ip;
    (void)covered;
    return NULL;
}
void asmtest_bs_precover_free(asmtest_bs_precover_t *p) { (void)p; }
void asmtest_bs_precover_set_current(const asmtest_bs_precover_t *p) {
    (void)p;
}
void asmtest_bs_stats(uint64_t *probe_calls, uint64_t *precover_hits) {
    if (probe_calls != NULL)
        *probe_calls = 0;
    if (precover_hits != NULL)
        *precover_hits = 0;
}
void asmtest_bs_stats_reset(void) {}
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
        if (regs[i].len && pc >= regs[i].base &&
            pc < regs[i].base + regs[i].len)
            return 1;
    return 0;
}

/* Decode the instruction length at absolute `at` in the tracee via a small foreign
 * read (0 if unreadable/undecodable) — used to detect block boundaries in the
 * absolute-address stream a multi-region capture records. */
static size_t foreign_insn_len(pid_t pid, uint64_t at) {
    uint8_t buf[16];
    ssize_t got = ptrace_read_mem(pid, buf, (void *)(uintptr_t)at, sizeof buf);
    if (got <= 0)
        return 0;
    return asmtest_disas(PTRACE_TRACE_ARCH, buf, (size_t)got, 0, 0, NULL, 0);
}

/* Forward a SIGTRAP the target raised ITSELF (an app int3/breakpoint) rather than
 * discard it or re-arm it inside a masked handler (measured fatal): the inline form
 * (`stop == NULL`) has a known window-end address, so run to it at native speed via
 * PTRACE_CONT and recover the result there; the stop-flag form has no such address,
 * so it can only deliver the signal via PTRACE_CONT and let the target run — the
 * window is truncated either way (the caller sets *overflow before calling this).
 * ENOENT (target exited before the window end) is not a ptrace error. */
static void window_loop_forward_app_trap(pid_t pid, uint64_t win_ret,
                                         volatile int *stop, long *retval_final,
                                         int *reached_end, int *rc) {
    if (stop == NULL) {
        int rc2 = run_until_sig(pid, win_ret, SIGTRAP);
        if (rc2 == ASMTEST_PTRACE_OK) {
            uint64_t rax = 0;
            if (read_pc_ret(pid, NULL, &rax, NULL, NULL) == 0)
                *retval_final = (long)rax;
            *reached_end = 1;
        } else if (rc2 != ASMTEST_PTRACE_ENOENT) {
            *rc = ASMTEST_PTRACE_ETRACE;
        }
    } else {
        ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)SIGTRAP);
    }
}

/* Shared inner loop for trace_attached_windowed and trace_attached_window_stop —
 * and the per-instruction remainder the block-step windowed driver hands off to when a
 * signal cuts a block (pass that signal as `pending_sig0`, so the first resume delivers
 * it; the two windowed entries pass 0, having no signal in hand). */
static int trace_attached_window_loop(
    pid_t pid, uint64_t win_base, size_t win_len, uint64_t win_ret,
    asmtest_addr_channel_t *chan, asmtest_addr_rec_t *regs, uint32_t *nreg_io,
    volatile int *stop, long *result, uint64_t *stream, uint32_t *stream_len,
    int *overflow_out, int pending_sig0) {
    uint32_t n = *stream_len;
    int overflow = 0;
    int rc = ASMTEST_PTRACE_OK;
    int status = 0;
    long retval_final = 0;
    uint64_t steps = 0;
    int pending_sig = pending_sig0;
    int reached_end =
        0; /* set only at a CLEAN terminator (*stop / pc==win_ret) */
    uint32_t nreg = *nreg_io;
    const int handed_off_trap = (pending_sig0 == SIGTRAP);

    if (handed_off_trap) {
        /* A caller (e.g. the block-step driver's app-int3 handoff) is handing off a
         * SIGTRAP it has not yet delivered. Never PTRACE_SINGLESTEP with it attached
         * (measured fatal — the re-armed trap fires inside a masked handler): forward
         * it via PTRACE_CONT instead, exactly as the mid-window app-trap case below
         * does, and skip the step loop entirely. */
        pending_sig = 0;
        overflow = 1;
        window_loop_forward_app_trap(pid, win_ret, stop, &retval_final,
                                     &reached_end, &rc);
    }

    while (!handed_off_trap) {
        if (stop != NULL && *stop != 0) {
            reached_end = 1; /* caller ended the window normally */
            break;
        }
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
            break;
        if (!WIFSTOPPED(status))
            continue;
        if (WSTOPSIG(status) != SIGTRAP) {
            pending_sig = WSTOPSIG(status);
            continue;
        }
        if (bs_sigtrap_is_app(pid)) {
            /* The target executed its OWN int3/breakpoint. Never SINGLESTEP across a
             * signal delivery (measured fatal: the re-armed trap fires inside a masked
             * handler): forward it via PTRACE_CONT instead. The inline form (stop ==
             * NULL) has a known window-end address, so run to it at native speed and
             * recover the result there; the stop-flag form has no such address, so it
             * can only truncate and let the target run. */
            overflow = 1;
            window_loop_forward_app_trap(pid, win_ret, stop, &retval_final,
                                         &reached_end, &rc);
            break;
        }
        if (++steps > PTRACE_WINDOW_STEP_CAP) {
            overflow = 1;
            break;
        }
        uint64_t pc = 0, rax = 0;
        if (read_pc_ret(pid, &pc, &rax, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (chan != NULL && nreg < ASMTEST_ADDR_CHAN_CAP)
            nreg += asmtest_addr_channel_drain(chan, regs + nreg,
                                               ASMTEST_ADDR_CHAN_CAP - nreg);

        if (stop == NULL && pc == win_ret) {
            retval_final = (long)rax;
            reached_end = 1; /* inline window returned to its caller */
            break;
        }
        if (in_region_set(pc, win_base, win_len, regs, nreg)) {
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = pc;
            else {
                overflow = 1;
                break;
            }
        }
    }
    /* The ONLY clean exits are *stop (async) and pc==win_ret (inline). Any other
     * termination that did not already set rc/overflow — the tracee exited or was
     * killed before the window end (WIFEXITED/WIFSIGNALED), or an unexpected wait
     * status — cut the window short: flag it so a partial stream is never rendered
     * as a complete capture. */
    if (rc == ASMTEST_PTRACE_OK && !overflow && !reached_end)
        overflow = 1;
    *nreg_io = nreg;
    *stream_len = n;
    *overflow_out = overflow;
    if (result != NULL)
        *result = retval_final;
    return rc;
}

static void materialize_stream_to_trace(pid_t pid, const uint64_t *stream,
                                        uint32_t n, int overflow,
                                        asmtest_trace_t *trace) {
    asmtest_blockseq_t seq = {0};
    for (uint32_t i = 0; i < n; i++) {
        uint64_t at = stream[i];
        size_t l = foreign_insn_len(pid, at);
        if (asmtest_blockseq_boundary(&seq, at, l))
            trace_append_block(trace, at);
        trace_append_insn(trace, at);
        if (l == 0) {
            trace->truncated = true; /* partition imprecise past here */
            continue; /* keep the address; do NOT drop the tail */
        }
    }
    if (overflow)
        trace->truncated = true;
}

int asmtest_ptrace_trace_attached_windowed(pid_t pid, const void *win_base_p,
                                           size_t win_len,
                                           asmtest_addr_channel_t *chan,
                                           long *result,
                                           asmtest_trace_t *trace) {
    if (win_base_p == NULL || win_len == 0 || trace == NULL)
        return ASMTEST_PTRACE_EINVAL;
    const uint64_t win_base = (uint64_t)(uintptr_t)win_base_p;

    uint64_t pc0 = 0, sp0 = 0, win_ret = 0;
    if (read_pc_ret(pid, &pc0, NULL, &sp0, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;
#if defined(__x86_64__)
    {
        if (ptrace_read_mem(pid, &win_ret, (void *)(uintptr_t)sp0,
                            sizeof win_ret) != (ssize_t)sizeof win_ret)
            return ASMTEST_PTRACE_ETRACE;
    }
#else
    read_pc_ret(pid, NULL, NULL, NULL, &win_ret);
#endif

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL)
        return ASMTEST_PTRACE_ETRACE;
    uint32_t n = 0;

    asmtest_addr_rec_t regs[ASMTEST_ADDR_CHAN_CAP];
    uint32_t nreg = 0;
    if (chan != NULL)
        nreg = asmtest_addr_channel_drain(chan, regs, ASMTEST_ADDR_CHAN_CAP);

    if (in_region_set(pc0, win_base, win_len, regs, nreg) &&
        n < PTRACE_STREAM_CAP)
        stream[n++] = pc0;

    int overflow = 0;
    int rc = trace_attached_window_loop(pid, win_base, win_len, win_ret, chan,
                                        regs, &nreg, NULL, result, stream, &n,
                                        &overflow, 0);

    if (rc == ASMTEST_PTRACE_OK) {
        materialize_stream_to_trace(pid, stream, n, overflow, trace);
    }
    free(stream);
    return rc;
}

/* ------------------------------------------------------------------ */
/* §D3 W-1 — the BTF block-step DRIVER for the whole-window capture.    */
/* PTRACE_SINGLEBLOCK stops once per TAKEN branch instead of once per   */
/* instruction; the instructions BETWEEN two stops are recovered by     */
/* disassembling the straight-line run out of the target. x86-64 only   */
/* (SINGLEBLOCK is x86/ppc/s390; AArch64 has no equivalent).            */
/* ------------------------------------------------------------------ */
#if defined(__x86_64__)

/* Ceiling on the straight-line walk of ONE reconstructed block. A basic block with more
 * than this many instructions and no taken branch is not real code — it means the walk
 * desynced, so it is failed (truncated) rather than walked forever. */
#ifndef PTRACE_BLOCK_WALK_CAP
#define PTRACE_BLOCK_WALK_CAP (1u << 16)
#endif

/* A refillable window of TARGET bytes, so walking a block costs one foreign read per
 * ~256 bytes instead of one per instruction. Unlike the REGION block-stepper (which
 * snapshots its whole [base, len) up front) a window has no bounded byte extent: it
 * spans the frame plus every channel-published JIT region, at absolute addresses. */
typedef struct {
    pid_t pid;
    uint64_t base; /* address buf[0] holds  */
    size_t len;    /* valid bytes in buf    */
    uint8_t buf[256];
} foreign_bytes_t;

/* Bytes of the target at absolute `at`, with *avail readable bytes following (NULL if
 * unreadable). Refills when `at` is uncovered or sits too near the end of the cache for
 * a maximal x86 instruction; a short read (a mapping ending) yields a short *avail,
 * which the decoder rejects as undecodable rather than reading past the mapping. */
static const uint8_t *foreign_bytes_at(foreign_bytes_t *fb, uint64_t at,
                                       size_t *avail) {
    if (at < fb->base || at >= fb->base + fb->len ||
        (fb->base + fb->len) - at < 16) {
        ssize_t got = ptrace_read_mem(fb->pid, fb->buf, (void *)(uintptr_t)at,
                                      sizeof fb->buf);
        if (got <= 0) {
            *avail = 0;
            return NULL;
        }
        fb->base = at;
        fb->len = (size_t)got;
    }
    *avail = (size_t)((fb->base + fb->len) - at);
    return fb->buf + (at - fb->base);
}

/* bs_scan_terminator's foreign-memory twin — same rule, same outcomes, walking absolute
 * addresses through a refillable byte window instead of a snapshot of a bounded region.
 * See bs_scan_terminator for the reasoning; the only differences are that the ceiling is
 * PTRACE_BLOCK_WALK_CAP rather than the region length, and that an unreadable address
 * (not just an undecodable one) is a scan stop. */
static int window_scan_terminator(foreign_bytes_t *fb, uint64_t from_pc,
                                  uint64_t next_pc, uint64_t *term_out) {
    uint64_t walk = from_pc;
    uint64_t first_cand = 0;
    unsigned ncand = 0;
    for (uint32_t guard = 0; guard < PTRACE_BLOCK_WALK_CAP; guard++) {
        size_t avail = 0;
        const uint8_t *p = foreign_bytes_at(fb, walk, &avail);
        if (p == NULL)
            break; /* unreadable */
        size_t l = 0;
        br_kind_t k =
            classify_branch(PTRACE_TRACE_ARCH, p, avail, walk, 0, next_pc, &l);
        if (l == 0)
            break; /* undecodable */
        if (k == BR_COND_HIT || k == BR_HARD_HIT) {
            if (ncand++ == 0)
                first_cand = walk;
            if (k == BR_HARD_HIT)
                goto decided; /* always taken: nothing past it runs */
            walk += l;
            continue; /* conditional: keep scanning to prove uniqueness */
        }
        if (k == BR_HARD_UNKNOWN) {
            if (ncand == 0) {
                *term_out = walk; /* ret/indirect: always taken, must be it */
                return ASMTEST_BS_OK;
            }
            goto decided;
        }
        if (k == BR_HARD_MISS)
            goto decided; /* always taken, but elsewhere: the run ended earlier */
        walk += l;        /* BR_NONE / BR_COND_MISS: the run continues */
    }
    if (ncand == 0)
        return ASMTEST_BS_FAIL; /* no terminator reaches next_pc: desynced */
    *term_out = first_cand;
    return ASMTEST_BS_AMBIGUOUS; /* a candidate, but uniqueness unproven: do not guess */
decided:
    if (ncand == 0)
        return ASMTEST_BS_FAIL;
    *term_out = first_cand;
    return ncand == 1 ? ASMTEST_BS_OK : ASMTEST_BS_AMBIGUOUS;
}

/* Reconstruct the straight-line run of ONE block of a windowed capture, appending the
 * absolute address of every instruction in it that falls in the window frame or a
 * published region (the same in_region_set filter the per-instruction loop applies at
 * each stop) — so the reconstructed stream is identical to the stepped one.
 *
 * Three ways a run ends, all observed with the tracee stopped at `to_pc`:
 *   at_mode == 0                    a TAKEN branch whose target is `to_pc` (a block-step
 *                                   #DB is trap-class: the stop PC is the TARGET).
 *                                   window_scan_terminator finds that branch — or
 *                                   reports the run ambiguous.
 *   at_mode == 1, exclusive_cut==0  the run reached `to_pc` and was cut there by a
 *                                   signal-delivery stop (a fault/other-signal, or the
 *                                   window's own return). No taken branch can precede
 *                                   to_pc (BTF would have stopped first), so the
 *                                   terminator IS to_pc: walk by length to it INCLUSIVE
 *                                   — matching the per-instruction path, which records
 *                                   to_pc at the TF stop preceding the signal stop.
 *   at_mode == 1, exclusive_cut==1  the tracee's OWN int3 cut the block: to_pc is one
 *                                   past the 0xCC byte (the trap-stop's PC), so the
 *                                   executed run ends EXACTLY there and the instruction
 *                                   AT to_pc has NOT executed — walk EXCLUSIVE of it
 *                                   (the window analog of blockstep_reconstruct_cut).
 * Overshooting to_pc (either mode) means the walk desynced: fail.
 * Returns ASMTEST_BS_OK, ASMTEST_BS_AMBIGUOUS (definite prefix recorded; caller truncates but may keep
 * tracing from the known stop) or ASMTEST_BS_FAIL (caller truncates). */
static int window_block_walk(foreign_bytes_t *fb, int at_mode,
                             int exclusive_cut, uint64_t from_pc,
                             uint64_t to_pc, uint64_t win_base,
                             uint64_t win_len, const asmtest_addr_rec_t *regs,
                             uint32_t nreg, uint64_t *stream, uint32_t *pn) {
    uint64_t term = to_pc;
    int r = ASMTEST_BS_OK;
    if (!at_mode) {
        r = window_scan_terminator(fb, from_pc, to_pc, &term);
        if (r == ASMTEST_BS_FAIL)
            return ASMTEST_BS_FAIL;
    } else if (to_pc < from_pc) {
        return ASMTEST_BS_FAIL; /* a straight-line run cannot end BEFORE it started */
    }
    /* Record [from_pc, term] (inclusive) or [from_pc, term) (exclusive_cut) — every
     * instruction recorded definitely executed. */
    uint64_t walk = from_pc;
    int rep = 0;
    for (uint32_t guard = 0; guard < PTRACE_BLOCK_WALK_CAP; guard++) {
        if (exclusive_cut && walk == term)
            return (rep && r == ASMTEST_BS_OK) ? ASMTEST_BS_AMBIGUOUS : r;
        if (walk > term)
            return ASMTEST_BS_FAIL; /* walked past the terminator: desynced */
        size_t avail = 0;
        const uint8_t *p = foreign_bytes_at(fb, walk, &avail);
        if (p == NULL)
            return ASMTEST_BS_FAIL;
        size_t l = asmtest_disas(PTRACE_TRACE_ARCH, p, avail, 0, 0, NULL, 0);
        if (l == 0)
            return ASMTEST_BS_FAIL; /* undecodable */
        if (in_region_set(walk, win_base, win_len, regs, nreg)) {
            if (*pn >= PTRACE_STREAM_CAP)
                return ASMTEST_BS_FAIL; /* stream full */
            stream[(*pn)++] = walk;
            /* A recorded rep-prefixed string op retires N times but appears ONCE, so
             * the reconstructed stream can no longer be byte-identical -> truncate. */
            if (asmtest_disas_is_rep_string(PTRACE_TRACE_ARCH, p, avail, 0))
                rep = 1;
        }
        if (!exclusive_cut && walk == term)
            return (rep && r == ASMTEST_BS_OK) ? ASMTEST_BS_AMBIGUOUS : r;
        walk += l;
    }
    return ASMTEST_BS_FAIL; /* the walk ceiling */
}

int asmtest_ptrace_trace_attached_windowed_blockstep(
    pid_t pid, const void *win_base_p, size_t win_len,
    asmtest_addr_channel_t *chan, long *result, asmtest_trace_t *trace) {
    if (win_base_p == NULL || win_len == 0 || trace == NULL)
        return ASMTEST_PTRACE_EINVAL;
    if (!asmtest_ptrace_blockstep_available())
        return ASMTEST_PTRACE_ENOSYS;
    const uint64_t win_base = (uint64_t)(uintptr_t)win_base_p;

    /* Identical window delimiting to the per-instruction entry: at the frame entry
     * [rsp] holds the return into the caller — the region-agnostic window end. */
    uint64_t pc0 = 0, sp0 = 0, win_ret = 0;
    if (read_pc_ret(pid, &pc0, NULL, &sp0, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;
    if (ptrace_read_mem(pid, &win_ret, (void *)(uintptr_t)sp0,
                        sizeof win_ret) != (ssize_t)sizeof win_ret)
        return ASMTEST_PTRACE_ETRACE;

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL)
        return ASMTEST_PTRACE_ETRACE;
    uint32_t n = 0;

    asmtest_addr_rec_t regs[ASMTEST_ADDR_CHAN_CAP];
    uint32_t nreg = 0;
    if (chan != NULL)
        nreg = asmtest_addr_channel_drain(chan, regs, ASMTEST_ADDR_CHAN_CAP);

    foreign_bytes_t fb;
    fb.pid = pid;
    fb.base = 0;
    fb.len = 0;

    /* The current stop opens the first block; reconstructing it records pc0 itself —
     * which is exactly the address the per-instruction entry seeds its stream with. */
    uint64_t prev_pc = pc0;
    int overflow = 0, reached_end = 0, rc = ASMTEST_PTRACE_OK, status = 0;
    long retval_final = 0;
    uint64_t steps = 0;
    int handoff_sig = -1;

    for (;;) {
        if (ptrace(PTRACE_SINGLEBLOCK, pid, NULL, NULL) != 0) {
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
        /* Bounds WALL TIME exactly as the per-instruction loop does: the budget is per
         * ptrace round-trip, and a block-step round-trip costs what a single-step one
         * costs — it just covers more instructions. */
        if (++steps > PTRACE_WINDOW_STEP_CAP) {
            overflow = 1;
            break;
        }
        uint64_t pc = 0, rax = 0;
        if (read_pc_ret(pid, &pc, &rax, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (chan != NULL && nreg < ASMTEST_ADDR_CHAN_CAP)
            nreg += asmtest_addr_channel_drain(chan, regs + nreg,
                                               ASMTEST_ADDR_CHAN_CAP - nreg);

        /* A non-SIGTRAP stop is a signal delivery: the block was cut at `pc` by a
         * transfer BTF cannot see. A SIGTRAP stop can ALSO be a cut: the tracee's own
         * int3/breakpoint (a JVM safepoint poll, a .NET breakpoint) is trap-class too,
         * indistinguishable from a BTF #DB except by si_code. Either way, reconstruct
         * what ran, then finish the window on the per-instruction loop (exact across
         * delivery and sigreturn — and, for an app trap, forwards the signal via
         * PTRACE_CONT rather than re-arm it inside a masked handler). */
        int sig = WSTOPSIG(status);
        int app_trap = (sig == SIGTRAP) && bs_sigtrap_is_app(pid);
        int at_mode = (sig != SIGTRAP) || app_trap;

        /* A block that STARTS outside every region contributes nothing: control enters
         * a region only through a taken branch, and that stops AT the region entry. So
         * the runtime/glue between regions costs no walk at all, and an undecodable
         * byte out there cannot truncate the capture (the per-instruction path never
         * decodes glue either). */
        if (in_region_set(prev_pc, win_base, win_len, regs, nreg)) {
            /* An app trap's cut is EXCLUSIVE of `pc` (the instruction there has not
             * executed — see window_block_walk); every other at_mode cut is inclusive. */
            int wr =
                window_block_walk(&fb, at_mode, app_trap, prev_pc, pc, win_base,
                                  win_len, regs, nreg, stream, &n);
            if (wr == ASMTEST_BS_FAIL) {
                overflow = 1;
                break;
            }
            if (wr == ASMTEST_BS_AMBIGUOUS)
                overflow =
                    1; /* honest gap: the definite prefix is recorded, the
                               * capture is truncated, and we know where the tracee
                               * IS (`pc`) — so keep capturing the rest. */
        }
        if (at_mode) {
            handoff_sig = sig;
            break;
        }
        /* The frame's `ret` is a taken branch, so the window end is always a block-step
         * stop — checked AFTER the reconstruction above, which records that `ret`. */
        if (pc == win_ret) {
            retval_final = (long)rax;
            reached_end = 1;
            break;
        }
        prev_pc = pc; /* the block's target opens the next block */
    }

    if (handoff_sig >= 0) {
        int ov2 = 0;
        int rc2 = trace_attached_window_loop(
            pid, win_base, win_len, win_ret, chan, regs, &nreg, NULL,
            &retval_final, stream, &n, &ov2, handoff_sig);
        if (rc2 != ASMTEST_PTRACE_OK)
            rc = rc2;
        else if (ov2)
            overflow = 1;
    } else if (rc == ASMTEST_PTRACE_OK && !overflow && !reached_end) {
        /* pc == win_ret is the only clean end; a tracee that exited or died first cut
         * the window short — never render a partial stream as a complete capture. */
        overflow = 1;
    }
    if (result != NULL)
        *result = retval_final;

    if (rc == ASMTEST_PTRACE_OK)
        materialize_stream_to_trace(pid, stream, n, overflow, trace);
    free(stream);
    return rc;
}

#else  /* !__x86_64__ : no PTRACE_SINGLEBLOCK equivalent on AArch64 */
int asmtest_ptrace_trace_attached_windowed_blockstep(
    pid_t pid, const void *win_base_p, size_t win_len,
    asmtest_addr_channel_t *chan, long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)win_base_p;
    (void)win_len;
    (void)chan;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}
#endif /* __x86_64__ */

int asmtest_ptrace_trace_attached_window_stop(pid_t pid,
                                              asmtest_addr_channel_t *chan,
                                              volatile int *stop,
                                              asmtest_trace_t *trace) {
    if (trace == NULL || stop == NULL)
        return ASMTEST_PTRACE_EINVAL;

    uint64_t pc0 = 0;
    if (read_pc_ret(pid, &pc0, NULL, NULL, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;

    uint64_t *stream =
        (uint64_t *)malloc((size_t)PTRACE_STREAM_CAP * sizeof(uint64_t));
    if (stream == NULL)
        return ASMTEST_PTRACE_ETRACE;
    uint32_t n = 0;

    asmtest_addr_rec_t regs[ASMTEST_ADDR_CHAN_CAP];
    uint32_t nreg = 0;
    if (chan != NULL)
        nreg = asmtest_addr_channel_drain(chan, regs, ASMTEST_ADDR_CHAN_CAP);

    if (in_region_set(pc0, 0, 0, regs, nreg) && n < PTRACE_STREAM_CAP)
        stream[n++] = pc0;

    int overflow = 0;
    int rc = trace_attached_window_loop(pid, 0, 0, 0, chan, regs, &nreg, stop,
                                        NULL, stream, &n, &overflow, 0);

    if (rc == ASMTEST_PTRACE_OK) {
        materialize_stream_to_trace(pid, stream, n, overflow, trace);
    }
    free(stream);
    return rc;
}

/* §D3 whole-window capture that OWNS its tracee — the fork-internal analog of
 * asmtest_ptrace_trace_call, so a caller (e.g. a managed binding that cannot fork
 * safely) gets a whole-window out-of-process trace with no tracee/attach/run_to
 * bookkeeping. Forks a child that calls `code(args…)`; the parent run_to's the window
 * frame entry, then single-steps recording the ABSOLUTE address of every instruction in
 * the window frame [code, code+len) OR any region pre-published on `chan` (the leaves
 * the frame calls, resolved by the caller before this call). Runtime/glue between
 * regions is stepped through but not recorded. The window ends when control returns to
 * the frame's caller. *result gets the frame's return value; trace->insns hold absolute
 * addresses (classify by region). Because the child is a fork, the code is at the SAME
 * addresses in this process, so instruction lengths for block partition are decoded from
 * LOCAL memory (robust even after the child is reaped). Budget: PTRACE_STREAM_CAP
 * instructions, past which trace->truncated is set (never a partial claimed complete).
 * ENOSYS off x86-64/AArch64 Linux; EINVAL on bad args. */
int asmtest_ptrace_trace_window_call(const void *code, size_t len,
                                     const long *args, int nargs,
                                     asmtest_addr_channel_t *chan, long *result,
                                     asmtest_trace_t *trace) {
    if (code == NULL || len == 0 || trace == NULL || nargs < 0 || nargs > 6 ||
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
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) != 0)
            _exit(127);
        raise(SIGSTOP);
        volatile long r = ((fn6_t)code)(a[0], a[1], a[2], a[3], a[4], a[5]);
        (void)r;
        _exit(0);
    }

    const uint64_t win_base = (uint64_t)(uintptr_t)code;
    int status = 0, rc = ASMTEST_PTRACE_OK, overflow = 0, reached_end = 0;
    uint32_t n = 0;
    long retval_final = 0;

    if (waitpid_handshake(pid, &status) < 0 || !WIFSTOPPED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL);

    /* Skip the glibc raise/return glue at native speed: breakpoint the window entry. */
    if (asmtest_ptrace_run_to(pid, code) != ASMTEST_PTRACE_OK) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }

    /* At the window entry [rsp] (x86) / LR (arm64) holds the return into the child
     * body — the region-agnostic window boundary. */
    uint64_t pc0 = 0, sp0 = 0, win_ret = 0;
    if (read_pc_ret(pid, &pc0, NULL, &sp0, NULL) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        free(stream);
        return ASMTEST_PTRACE_ETRACE;
    }
#if defined(__x86_64__)
    {
        if (ptrace_read_mem(pid, &win_ret, (void *)(uintptr_t)sp0,
                            sizeof win_ret) != (ssize_t)sizeof win_ret) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            free(stream);
            return ASMTEST_PTRACE_ETRACE;
        }
    }
#else
    read_pc_ret(pid, NULL, NULL, NULL, &win_ret);
#endif

    asmtest_addr_rec_t regs[ASMTEST_ADDR_CHAN_CAP];
    uint32_t nreg = 0;
    if (chan != NULL)
        nreg = asmtest_addr_channel_drain(chan, regs, ASMTEST_ADDR_CHAN_CAP);
    if (in_region_set(pc0, win_base, len, regs, nreg) && n < PTRACE_STREAM_CAP)
        stream[n++] = pc0;

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
        if (bs_sigtrap_is_app(pid)) {
            /* The tracee executed its OWN int3 (a JVM safepoint poll, a .NET
             * breakpoint). Never PTRACE_SINGLESTEP with the signal attached
             * (measured fatal): forward it via PTRACE_CONT instead. The unconditional
             * kill+reap below (this function owns the fork regardless of how the loop
             * ends) tolerates whatever stop/exit that CONT produces. */
            overflow = 1;
            ptrace(PTRACE_CONT, pid, NULL, (void *)(uintptr_t)SIGTRAP);
            waitpid(pid, &status, 0);
            break;
        }
        uint64_t pc = 0, rax = 0;
        if (read_pc_ret(pid, &pc, &rax, NULL, NULL) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        if (chan != NULL && nreg < ASMTEST_ADDR_CHAN_CAP)
            nreg += asmtest_addr_channel_drain(chan, regs + nreg,
                                               ASMTEST_ADDR_CHAN_CAP - nreg);
        if (pc == win_ret) {
            retval_final = (long)rax;
            reached_end = 1; /* window body returned to its caller */
            break;
        }
        if (in_region_set(pc, win_base, len, regs, nreg)) {
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = pc;
            else {
                overflow = 1;
                break;
            }
        }
    }
    /* pc==win_ret is the only clean window end; if the body exited/died before
     * returning (WIFEXITED/WIFSIGNALED, e.g. a call to exit()) the window was cut
     * short — never render the partial stream as a complete capture. */
    if (rc == ASMTEST_PTRACE_OK && !overflow && !reached_end)
        overflow = 1;
    if (result != NULL)
        *result = retval_final;
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);

    /* Materialize the ABSOLUTE-address stream; block starts at a fall-through
     * discontinuity. The fork guarantees `code` and the published leaves live at the
     * same addresses HERE, so lengths decode from LOCAL memory (no foreign read of a
     * now-reaped child). */
    if (rc == ASMTEST_PTRACE_OK) {
        asmtest_blockseq_t seq = {0};
        for (uint32_t i = 0; i < n; i++) {
            uint64_t at = stream[i];
            size_t l =
                asmtest_disas(PTRACE_TRACE_ARCH, (const uint8_t *)(uintptr_t)at,
                              16, 0, 0, NULL, 0);
            if (asmtest_blockseq_boundary(&seq, at, l))
                trace_append_block(trace, at);
            trace_append_insn(trace, at);
            if (l == 0) {
                trace->truncated = true;
                break;
            }
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
        if (ptrace_read_mem(pid, owned, (void *)(uintptr_t)base, len) !=
            (ssize_t)len) {
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

        if (bs_sigtrap_is_app(pid)) {
            /* The foreign target executed its OWN int3/breakpoint. Truncate
             * honestly and leave it in its SIGTRAP signal-delivery stop — it is
             * foreign and is NEVER killed; the caller owns signal/detach policy
             * (mirrors the attached block-step driver's app-trap handling). */
            if (entered)
                overflow = 1;
            break;
        }

        uint64_t pc, retval, sp;
        if (read_pc_ret(pid, &pc, &retval, &sp, NULL) != 0) {
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
            exit_kind_t k = classify_region_exit(
                pid, code, len, base_ip, stream[n - 1], sp, &resume_off);
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

/* The built-in L3 default denylist (call-descent plan Phase 5): populate the
 * handle's deny pool from the tracee once its pid is known. Two sources:
 *
 * 1. /proc/<pid>/maps — executable mappings whose name marks code L3 must not
 *    single-step into: the dynamic linker (the lazy-binding PLT resolver
 *    mutates the GOT under the tracer), [vdso]/[vsyscall], and managed-runtime
 *    GC/JIT modules (a stepped GC entry can move the very code being traced).
 * 2. When the tracee shares this process's address layout (the fork path),
 *    dlsym entry points of the classic blocking libc/pthread calls — one-byte
 *    deny regions, so only a call landing exactly on the entry is refused.
 *    The watchdog would eventually break a blocked descended syscall anyway;
 *    denying descent keeps the trace honest (an edge) instead of truncated.
 *
 * Failure here is deliberately soft: no /proc or no dlsym just leaves fewer
 * default regions — budget + watchdog remain the hard backstops. */
static const char *const descend_deny_modules[] = {
    "ld-linux",   "ld-musl",   "/ld.so",   "[vdso]",      "[vsyscall]",
    "libcoreclr", "libclrjit", "libclrgc", "libmonosgen", "libmono-",
    "libjvm",     "libart",    "libv8",    "libgc.",
};
static const char *const descend_deny_syms[] = {
    "poll",
    "ppoll",
    "select",
    "pselect",
    "epoll_wait",
    "epoll_pwait",
    "nanosleep",
    "clock_nanosleep",
    "sleep",
    "usleep",
    "pause",
    "sigwait",
    "sigwaitinfo",
    "sigtimedwait",
    "wait",
    "waitpid",
    "wait4",
    "waitid",
    "accept",
    "accept4",
    "connect",
    "recv",
    "recvfrom",
    "recvmsg",
    "sem_wait",
    "sem_timedwait",
    "pthread_cond_wait",
    "pthread_cond_timedwait",
    "pthread_join",
    "pthread_mutex_lock",
    "read",
    "write",
};

static void descend_apply_default_denylist(asmtest_descent_t *d, pid_t pid,
                                           int same_layout) {
    if (d == NULL || !d->use_default_denylist || d->default_denylist_applied ||
        d->level < ASMTEST_DESCENT_DESCEND_ALL)
        return;
    d->default_denylist_applied = 1;

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) != -1) {
            unsigned long start, end;
            char perms[8];
            int noff = 0;
            if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &start, &end, perms,
                       &noff) < 3 ||
                perms[2] != 'x' || noff == 0)
                continue;
            const char *name = line + noff;
            for (size_t i = 0;
                 i < sizeof descend_deny_modules / sizeof *descend_deny_modules;
                 i++)
                if (strstr(name, descend_deny_modules[i]) != NULL) {
                    asmtest_descent_deny_region(d, (void *)(uintptr_t)start,
                                                (size_t)(end - start));
                    break;
                }
        }
        free(line);
        fclose(f);
    }

    if (same_layout) {
        for (size_t i = 0;
             i < sizeof descend_deny_syms / sizeof *descend_deny_syms; i++) {
            void *sym = dlsym(RTLD_DEFAULT, descend_deny_syms[i]);
            if (sym != NULL)
                asmtest_descent_deny_region(d, sym, 1);
        }
    }
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
    if (waitpid_handshake(pid, &status) < 0 || !WIFSTOPPED(status)) {
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
    /* Forked tracee: same address layout, so the symbol-based defaults apply. */
    descend_apply_default_denylist(descent, pid, 1);

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
        if (ptrace_read_mem(pid, owned, (void *)(uintptr_t)base, len) !=
            (ssize_t)len) {
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
    /* Foreign tracee: mapping-based defaults only (local symbol addresses
     * would not be valid in its address space). */
    descend_apply_default_denylist(descent, pid, 0);

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
                                        const long *args, int nargs,
                                        long *result, asmtest_trace_t *trace) {
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
                                           long *result,
                                           asmtest_trace_t *trace) {
    (void)pid;
    (void)win_base_p;
    (void)win_len;
    (void)chan;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached_windowed_blockstep(
    pid_t pid, const void *win_base_p, size_t win_len,
    asmtest_addr_channel_t *chan, long *result, asmtest_trace_t *trace) {
    (void)pid;
    (void)win_base_p;
    (void)win_len;
    (void)chan;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached_window_stop(pid_t pid,
                                              asmtest_addr_channel_t *chan,
                                              volatile int *stop,
                                              asmtest_trace_t *trace) {
    (void)pid;
    (void)chan;
    (void)stop;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_window_call(const void *code, size_t len,
                                     const long *args, int nargs,
                                     asmtest_addr_channel_t *chan, long *result,
                                     asmtest_trace_t *trace) {
    (void)code;
    (void)len;
    (void)args;
    (void)nargs;
    (void)chan;
    (void)result;
    (void)trace;
    return ASMTEST_PTRACE_ENOSYS;
}

int asmtest_ptrace_trace_attached_blockstep(pid_t pid, const void *base,
                                            size_t len, long *result,
                                            asmtest_trace_t *trace) {
    (void)pid;
    (void)base;
    (void)len;
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

/* IBS is Linux+x86-64+AMD only, so this platform never has a covered-block set
 * to build from. */
asmtest_bs_precover_t *
asmtest_bs_precover_build(const uint8_t *code, size_t len, uint64_t base_ip,
                          const asmtest_ibs_blocks_t *covered) {
    (void)code;
    (void)len;
    (void)base_ip;
    (void)covered;
    return NULL;
}
void asmtest_bs_precover_free(asmtest_bs_precover_t *p) { (void)p; }
void asmtest_bs_precover_set_current(const asmtest_bs_precover_t *p) {
    (void)p;
}
void asmtest_bs_stats(uint64_t *probe_calls, uint64_t *precover_hits) {
    if (probe_calls != NULL)
        *probe_calls = 0;
    if (precover_hits != NULL)
        *precover_hits = 0;
}
void asmtest_bs_stats_reset(void) {}

#endif /* stepper */
