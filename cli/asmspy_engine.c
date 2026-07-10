/*
 * asmspy_engine.c — the two out-of-process tracer engines behind asmspy.
 *
 *   asmspy_engine_syscalls() — PTRACE_SYSCALL stream, decoding data (a strace)
 *   asmspy_engine_region()   — run_to + trace_attached_ex sampler (asm + calls)
 *
 * Both attach, drive, and detach ENTIRELY on the calling thread — ptrace is
 * per-thread, so the TUI hands each engine to a dedicated tracer thread. A quit
 * is delivered by the UI setting *stop and pthread_kill(tracer, SIGALRM): the
 * empty handler (installed here, SA_RESTART cleared) makes the pending waitpid
 * return EINTR, the engine sees *stop, detaches, and returns.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "asmspy.h"

#define DUMP_CAP 200 /* bytes of a buffer/path decoded per syscall */

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static void on_alarm(int s) { (void)s; } /* no-op; just interrupts waitpid */

/* Install the EINTR-making SIGALRM handler and unblock it on THIS thread, so a
 * pthread_kill(SIGALRM) from the UI can wake a blocked waitpid here. */
static void arm_quit_wake(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; /* sa_flags = 0 -> no SA_RESTART */
    sigaction(SIGALRM, &sa, NULL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

static int rd(pid_t pid, uint64_t addr, void *buf, size_t n) {
    struct iovec l = {buf, n};
    struct iovec r = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(pid, &l, 1, &r, 1, 0) == (ssize_t)n ? 0 : -1;
}

void asmspy_strerror(int rc, char *buf, size_t buflen) {
    const char *m;
    switch (rc) {
    case 0: m = "ok"; break;
    case ASMTEST_PTRACE_EINVAL: m = "invalid argument"; break;
    case ASMTEST_PTRACE_EUNAVAIL: m = "tracer unavailable on this host"; break;
    case ASMTEST_PTRACE_ENOSYS: m = "backend not built in"; break;
    case ASMTEST_PTRACE_ENOENT: m = "region/symbol not found"; break;
    case ASMTEST_PTRACE_ETRACE: m = "ptrace/attach failure (permission? ptrace_scope?)"; break;
    default: m = "attach failed"; break;
    }
    snprintf(buf, buflen, "%s", m);
}

/* ------------------------------------------------------------------ */
/* Syscall stream engine                                               */
/* ------------------------------------------------------------------ */

/* bounded snprintf-append; returns the new length (never exceeds cap-1). */
static size_t apf(char *b, size_t cap, size_t o, const char *fmt, ...) {
    if (o >= cap)
        return cap - 1;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(b + o, cap - o, fmt, ap);
    va_end(ap);
    if (k < 0)
        return o;
    o += (size_t)k;
    return o > cap - 1 ? cap - 1 : o;
}

/* append a C-escaped quoted view of up to `n` bytes at target `addr`. */
static size_t ap_data(char *b, size_t cap, size_t o, pid_t pid, uint64_t addr,
                      uint32_t n) {
    uint32_t want = n > DUMP_CAP ? DUMP_CAP : n;
    unsigned char tmp[DUMP_CAP];
    if (want && rd(pid, addr, tmp, want) != 0)
        want = 0;
    o = apf(b, cap, o, "\"");
    for (uint32_t i = 0; i < want; i++) {
        unsigned char c = tmp[i];
        if (c == '\n')
            o = apf(b, cap, o, "\\n");
        else if (c == '\t')
            o = apf(b, cap, o, "\\t");
        else if (c == '"' || c == '\\')
            o = apf(b, cap, o, "\\%c", c);
        else if (c >= 0x20 && c < 0x7f)
            o = apf(b, cap, o, "%c", c);
        else
            o = apf(b, cap, o, "\\x%02x", c);
    }
    o = apf(b, cap, o, "\"");
    if (n > want)
        o = apf(b, cap, o, "...");
    return o;
}

static size_t ap_cstr(char *b, size_t cap, size_t o, pid_t pid, uint64_t addr) {
    char tmp[DUMP_CAP + 1];
    struct iovec l = {tmp, DUMP_CAP};
    struct iovec r = {(void *)(uintptr_t)addr, DUMP_CAP};
    ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
    if (got < 0)
        got = 0;
    tmp[got] = '\0';
    return apf(b, cap, o, "\"%.*s\"", (int)got, tmp);
}

/* Decode a syscall's primary STRING (write/read buffer, or a path) into `out`,
 * escaped but UNquoted — for the TUI's decoded-strings pane. */
static void decode_data(pid_t pid, uint64_t addr, uint32_t n, char *out,
                        size_t cap) {
    uint32_t want = n > DUMP_CAP ? DUMP_CAP : n;
    unsigned char tmp[DUMP_CAP];
    if (want && rd(pid, addr, tmp, want) != 0)
        want = 0;
    size_t o = 0;
    for (uint32_t i = 0; i < want; i++) {
        unsigned char c = tmp[i];
        if (c == '\n')
            o = apf(out, cap, o, "\\n");
        else if (c == '\t')
            o = apf(out, cap, o, "\\t");
        else if (c >= 0x20 && c < 0x7f)
            o = apf(out, cap, o, "%c", c);
        else
            o = apf(out, cap, o, "\\x%02x", c);
    }
    if (n > want)
        o = apf(out, cap, o, "...");
    (void)o;
}

static void decode_cstr(pid_t pid, uint64_t addr, char *out, size_t cap) {
    char tmp[DUMP_CAP + 1];
    struct iovec l = {tmp, DUMP_CAP};
    struct iovec r = {(void *)(uintptr_t)addr, DUMP_CAP};
    ssize_t got = process_vm_readv(pid, &l, 1, &r, 1, 0);
    if (got < 0)
        got = 0;
    tmp[got] = '\0';
    snprintf(out, cap, "%s", tmp);
}

static const char *scname(long nr) {
    switch (nr) {
    case SYS_getpid: return "getpid";
    case SYS_nanosleep: return "nanosleep";
    case SYS_clock_nanosleep: return "clock_nanosleep";
    case SYS_restart_syscall: return "restart_syscall";
    case SYS_lseek: return "lseek";
    case SYS_fstat: return "fstat";
    case SYS_mmap: return "mmap";
    case SYS_futex: return "futex";
    case SYS_ioctl: return "ioctl";
    case SYS_writev: return "writev";
    case SYS_readv: return "readv";
    case SYS_poll: return "poll";
    case SYS_ppoll: return "ppoll";
    default: return NULL;
    }
}

static void format_syscall(char *b, size_t cap, char *sout, size_t scap,
                           pid_t pid, long nr,
                           const struct user_regs_struct *e, long ret) {
    size_t o = 0;
    sout[0] = '\0';
    switch (nr) {
    case SYS_write:
        o = apf(b, cap, o, "write(fd=%llu, ", (unsigned long long)e->rdi);
        o = ap_data(b, cap, o, pid, e->rsi, (uint32_t)e->rdx);
        o = apf(b, cap, o, ", %llu) = %ld", (unsigned long long)e->rdx, ret);
        decode_data(pid, e->rsi, (uint32_t)e->rdx, sout, scap);
        break;
    case SYS_read:
        o = apf(b, cap, o, "read(fd=%llu, %llu) = ", (unsigned long long)e->rdi,
                (unsigned long long)e->rdx);
        if (ret > 0) {
            o = ap_data(b, cap, o, pid, e->rsi, (uint32_t)ret);
            o = apf(b, cap, o, " [%ld]", ret);
            decode_data(pid, e->rsi, (uint32_t)ret, sout, scap);
        } else {
            o = apf(b, cap, o, "%ld", ret);
        }
        break;
    case SYS_openat:
        o = apf(b, cap, o, "openat(");
        if ((int)e->rdi == AT_FDCWD)
            o = apf(b, cap, o, "AT_FDCWD");
        else
            o = apf(b, cap, o, "%d", (int)e->rdi);
        o = apf(b, cap, o, ", ");
        o = ap_cstr(b, cap, o, pid, e->rsi);
        o = apf(b, cap, o, ", 0x%llx) = %ld", (unsigned long long)e->rdx, ret);
        decode_cstr(pid, e->rsi, sout, scap);
        break;
    case SYS_close:
        o = apf(b, cap, o, "close(fd=%llu) = %ld", (unsigned long long)e->rdi,
                ret);
        break;
    default: {
        const char *nm = scname(nr);
        if (nm)
            o = apf(b, cap, o, "%s(", nm);
        else
            o = apf(b, cap, o, "syscall#%ld(", nr);
        o = apf(b, cap, o, "0x%llx, 0x%llx, 0x%llx) = %ld",
                (unsigned long long)e->rdi, (unsigned long long)e->rsi,
                (unsigned long long)e->rdx, ret);
        break;
    }
    }
    (void)o;
}

int asmspy_engine_syscalls(pid_t pid, long max, atomic_bool *stop,
                           asmspy_syscall_sink sink, void *ctx) {
    arm_quit_wake();

    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return ASMTEST_PTRACE_ETRACE;
    }
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)PTRACE_O_TRACESYSGOOD);

    int at_entry = 1, deliver = 0;
    long ent_nr = -1, done = 0;
    struct user_regs_struct entry_regs;
    memset(&entry_regs, 0, sizeof entry_regs);

    while ((max < 0 || done < max) && !(stop && atomic_load(stop))) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)deliver) != 0)
            break;
        deliver = 0;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR && stop && atomic_load(stop))
                break;
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            break;
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        if (sig == (SIGTRAP | 0x80)) {
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
                break;
            if (at_entry) {
                ent_nr = (long)regs.orig_rax;
                entry_regs = regs;
                at_entry = 0;
            } else {
                char line[1024], sdata[512];
                format_syscall(line, sizeof line, sdata, sizeof sdata, pid,
                               ent_nr, &entry_regs, (long)regs.rax);
                if (sink)
                    sink(ctx, line, sdata[0] ? sdata : NULL);
                at_entry = 1;
                done++;
            }
        } else if (sig != SIGTRAP) {
            deliver = sig; /* forward a real signal */
        }
    }

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Region-sample engine                                                */
/* ------------------------------------------------------------------ */

int asmspy_engine_region(pid_t pid, uint64_t base, size_t len, long max,
                         atomic_bool *stop, asmspy_region_sink sink,
                         void *ctx) {
    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;
    if (len == 0 || len > (64u << 20))
        return ASMTEST_PTRACE_EINVAL;

    arm_quit_wake();

    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return ASMTEST_PTRACE_ETRACE;
    }

    /* region bytes for the disassembler (read once; static code) */
    uint8_t *code = malloc(len);
    if (code && rd(pid, base, code, len) != 0) {
        free(code);
        code = NULL;
    }

    unsigned sample = 0;
    int idle = 0; /* consecutive rounds that produced NO sample */
    while ((max < 0 || (long)sample < max) && !(stop && atomic_load(stop))) {
        int rc = asmtest_ptrace_run_to(pid, (void *)(uintptr_t)base);
        if (rc != ASMTEST_PTRACE_OK) {
            if (stop && atomic_load(stop))
                break;
            if (rc == ASMTEST_PTRACE_ENOENT) /* target exited */
                break;
            if (++idle > 20)
                break;
            continue;
        }

        asmtest_trace_t *tr = asmtest_trace_new(8192, 512);
        asmtest_descent_t *dsc =
            asmtest_descent_new(ASMTEST_DESCENT_RECORD_EDGES);
        long result = 0;
        rc = asmtest_ptrace_trace_attached_ex(pid, (void *)(uintptr_t)base, len,
                                              &result, tr, dsc);
        int produced = (rc == ASMTEST_PTRACE_OK && tr &&
                        asmtest_emu_trace_insns_total(tr) >= 1);
        if (produced) {
            sample++;
            idle = 0;
            if (sink)
                sink(ctx, sample, result, tr, dsc, code, len, base);
        }
        asmtest_trace_free(tr);
        asmtest_descent_free(dsc);

        if (!produced) {
            /* run_to succeeded but the trace failed/was empty; count it toward
             * the idle bailout so a bad region can't spin the loop forever. */
            if (stop && atomic_load(stop))
                break;
            if (rc == ASMTEST_PTRACE_ENOENT)
                break;
            if (++idle > 20)
                break;
        }
    }

    free(code);
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}
