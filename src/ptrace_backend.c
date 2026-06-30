/*
 * ptrace_backend.c — out-of-process single-step native-trace backend (W2).
 * See asmtest_ptrace.h and docs/plans/zen2-singlestep-trace-plan.md (Phase 5, W2).
 *
 * A tracer PARENT PTRACE_SINGLESTEPs a forked tracee that calls the registered code,
 * reading RIP from the child's register file at each stop. It produces the same
 * exact/complete asmtest_trace_t offsets as the in-process EFLAGS.TF stepper
 * (src/ss_backend.c) — and reuses that backend's single-entry/ends-at-branch block
 * normalization — but collects them entirely out of band, so the tracee's signal
 * disposition and code cache are never touched (the property a JIT/GC managed
 * runtime needs, and the only single-step form available on AArch64).
 *
 * The parent observes every step through ptrace, so no shared memory is needed: it
 * fills the caller-owned trace directly from the register reads.
 */
#define _GNU_SOURCE

#include "asmtest_ptrace.h"
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__linux__) && defined(__x86_64__)

#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

/* Ordered in-region RIP-offset capture buffer; overflow is flagged truncated, never
 * emitted as complete. Sized for the small-routine envelope, like ss_backend. */
#ifndef PTRACE_STREAM_CAP
#define PTRACE_STREAM_CAP (1u << 16) /* 65536 offsets */
#endif

int asmtest_ptrace_available(void) { return 1; }

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "available";
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
        size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, base, len, base_ip, off, NULL,
                                 0);
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
         * to six integer args. Extra register args are ignored by the callee per the
         * SysV ABI. _exit avoids running atexit/stdio in the stepped child. */
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

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        uint64_t rip = (uint64_t)regs.rip;

        if (rip >= base_ip && rip < base_ip + len) {
            entered = 1;
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = rip - base_ip;
            else
                overflow = 1;
        } else if (entered && !returned) {
            /* First step out of the region after entering = the routine returned
             * (supported target is a pure-compute routine that does not call out, so
             * this transition is the ret). RAX holds the return value. */
            if (result != NULL)
                *result = (long)regs.rax;
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

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0) {
            rc = ASMTEST_PTRACE_ETRACE;
            break;
        }
        uint64_t rip = (uint64_t)regs.rip;

        if (rip >= base_ip && rip < base_ip + len) {
            entered = 1;
            if (n < PTRACE_STREAM_CAP)
                stream[n++] = rip - base_ip;
            else
                overflow = 1;
        } else if (entered && !returned) {
            if (result != NULL)
                *result = (long)regs.rax;
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

#else /* not Linux x86-64 — link-compatible stubs */

int asmtest_ptrace_available(void) { return 0; }

void asmtest_ptrace_skip_reason(char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    const char *msg = "out-of-process ptrace stepper is Linux x86-64 only";
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

#endif
