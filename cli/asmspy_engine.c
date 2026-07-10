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

/* Every syscall name the compiling host's headers know, indexed by number. The
 * SC() list is generated from <sys/syscall.h> (cli/gen-syscall-names.sh), and
 * the NUMBER comes from the same headers via __NR_ — so this cannot drift out
 * of step with the kernel the way a hand-written table would. Sparse: numbers
 * with no entry stay NULL and fall back to the numeric form. */
static const char *const sc_names[] = {
#define SC(n) [__NR_##n] = #n,
#include "asmspy_syscall_names.inc"
#undef SC
};

static const char *scname(long nr) {
    if (nr < 0 || (size_t)nr >= sizeof sc_names / sizeof sc_names[0])
        return NULL;
    return sc_names[nr];
}

/* Where a syscall keeps its primary PATH argument — decoding it is what turns
 * an opaque "syscall#262(0x7f.., 0x7f.., ..)" into a line worth reading.
 * execve/execveat are deliberately absent: we format at syscall EXIT, by which
 * point a successful exec has replaced the address space the path lived in. */
typedef enum { PATH_NONE = 0, PATH_RDI, PATH_AT_RSI } path_kind_t;

static path_kind_t path_kind(long nr) {
    switch (nr) {
    /* path is the first argument */
    case SYS_open:
    case SYS_stat:
    case SYS_lstat:
    case SYS_access:
    case SYS_unlink:
    case SYS_rmdir:
    case SYS_chdir:
    case SYS_readlink:
    case SYS_statfs:
    case SYS_truncate:
    case SYS_chmod:
    case SYS_chown:
    case SYS_lchown:
    case SYS_mkdir:
    case SYS_rename:
    case SYS_link:
    case SYS_symlink:  /* (target, linkpath) — target is the datum */
    case SYS_symlinkat:/* (target, newdirfd, linkpath) — likewise */
    case SYS_creat:
    case SYS_mknod:
    case SYS_utimes:
    case SYS_chroot:
    case SYS_acct:
    case SYS_mount:    /* (source, target, ...) */
    case SYS_umount2:
    case SYS_swapon:
    case SYS_swapoff: return PATH_RDI;
    /* (dirfd, path, ...) — openat has its own case, with flag formatting */
    case SYS_newfstatat:
    case SYS_unlinkat:
    case SYS_faccessat:
    case SYS_readlinkat:
    case SYS_mkdirat:
    case SYS_mknodat:
    case SYS_fchownat:
    case SYS_fchmodat:
    case SYS_renameat:
    case SYS_futimesat:
    case SYS_linkat:
#ifdef SYS_renameat2
    case SYS_renameat2:
#endif
#ifdef SYS_statx
    case SYS_statx:
#endif
#ifdef SYS_openat2
    case SYS_openat2:
#endif
#ifdef SYS_faccessat2
    case SYS_faccessat2:
#endif
        return PATH_AT_RSI;
    default: return PATH_NONE;
    }
}

/* A dirfd prints as AT_FDCWD far more often than as a number. */
static size_t ap_dirfd(char *b, size_t cap, size_t o, long long raw) {
    if ((int)raw == AT_FDCWD) /* -100, and it arrives zero-extended */
        return apf(b, cap, o, "AT_FDCWD");
    return apf(b, cap, o, "%d", (int)raw);
}

/* Print "fd=N", and — like `strace -y` — the thing it points at, read from the
 * TARGET's /proc/<pid>/fd/<N>. A regular file readlinks to its path; a socket or
 * pipe to "socket:[inode]" / "pipe:[inode]" (what strace shows too). Best-effort:
 * an fd already gone by the time we format (close() at its exit-stop) or one we
 * cannot readlink just prints "fd=N" with no suffix. */
static size_t ap_fd(char *b, size_t cap, size_t o, pid_t pid, long long fd) {
    o = apf(b, cap, o, "fd=%lld", fd);
    if (fd < 0)
        return o;
    char link[64];
    snprintf(link, sizeof link, "/proc/%d/fd/%lld", (int)pid, fd);
    char tgt[DUMP_CAP];
    ssize_t k = readlink(link, tgt, sizeof tgt - 1);
    if (k > 0) {
        tgt[k] = '\0';
        o = apf(b, cap, o, "<%s>", tgt);
    }
    return o;
}

static void format_syscall(char *b, size_t cap, char *sout, size_t scap,
                           pid_t pid, long nr,
                           const struct user_regs_struct *e, long ret) {
    size_t o = 0;
    sout[0] = '\0';
    switch (nr) {
    case SYS_write:
        o = apf(b, cap, o, "write(");
        o = ap_fd(b, cap, o, pid, (long long)e->rdi);
        o = apf(b, cap, o, ", ");
        o = ap_data(b, cap, o, pid, e->rsi, (uint32_t)e->rdx);
        o = apf(b, cap, o, ", %llu) = %ld", (unsigned long long)e->rdx, ret);
        decode_data(pid, e->rsi, (uint32_t)e->rdx, sout, scap);
        break;
    case SYS_read:
        o = apf(b, cap, o, "read(");
        o = ap_fd(b, cap, o, pid, (long long)e->rdi);
        o = apf(b, cap, o, ", %llu) = ", (unsigned long long)e->rdx);
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
        o = ap_dirfd(b, cap, o, (long long)e->rdi);
        o = apf(b, cap, o, ", ");
        o = ap_cstr(b, cap, o, pid, e->rsi);
        o = apf(b, cap, o, ", 0x%llx) = %ld", (unsigned long long)e->rdx, ret);
        decode_cstr(pid, e->rsi, sout, scap);
        break;
    case SYS_close:
        o = apf(b, cap, o, "close(");
        o = ap_fd(b, cap, o, pid, (long long)e->rdi);
        o = apf(b, cap, o, ") = %ld", ret);
        break;
    default: {
        const char *nm = scname(nr);
        path_kind_t pk = nm ? path_kind(nr) : PATH_NONE;
        if (nm)
            o = apf(b, cap, o, "%s(", nm);
        else
            o = apf(b, cap, o, "syscall#%ld(", nr);

        if (pk == PATH_RDI) {
            o = ap_cstr(b, cap, o, pid, e->rdi);
            o = apf(b, cap, o, ", 0x%llx, 0x%llx) = %ld",
                    (unsigned long long)e->rsi, (unsigned long long)e->rdx, ret);
            decode_cstr(pid, e->rdi, sout, scap);
        } else if (pk == PATH_AT_RSI) {
            o = ap_dirfd(b, cap, o, (long long)e->rdi);
            o = apf(b, cap, o, ", ");
            o = ap_cstr(b, cap, o, pid, e->rsi);
            o = apf(b, cap, o, ", 0x%llx) = %ld", (unsigned long long)e->rdx,
                    ret);
            decode_cstr(pid, e->rsi, sout, scap);
        } else {
            o = apf(b, cap, o, "0x%llx, 0x%llx, 0x%llx) = %ld",
                    (unsigned long long)e->rdi, (unsigned long long)e->rsi,
                    (unsigned long long)e->rdx, ret);
        }
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

/* ------------------------------------------------------------------ */
/* Whole-process live instruction stream                               */
/* ------------------------------------------------------------------ */

int asmspy_engine_stream(pid_t pid, long max, atomic_bool *stop,
                         const asmspy_symtab_t *syms, asmspy_stream_sink sink,
                         void *ctx) {
    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0)
        return ASMTEST_PTRACE_ETRACE;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return ASMTEST_PTRACE_ETRACE;
    }

    int deliver = 0;
    long done = 0;
    while ((max < 0 || done < max) && !(stop && atomic_load(stop))) {
        if (ptrace(PTRACE_SINGLESTEP, pid, NULL, (void *)(long)deliver) != 0)
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
        if (sig != SIGTRAP) {
            deliver = sig; /* forward a real signal to the target */
            continue;
        }

        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
            break;
        uint64_t rip = regs.rip;

        /* disassemble the instruction about to retire (bytes read from target) */
        uint8_t code[16];
        char dis[160] = "";
        struct iovec liov = {code, sizeof code};
        struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
        ssize_t got = process_vm_readv(pid, &liov, 1, &riov, 1, 0);
        if (got >= 1 && asmtest_disas_available())
            asmtest_disas(ASMTEST_ARCH_X86_64, code, (size_t)got, rip, 0, dis,
                          sizeof dis);

        /* resolve the function this address lands in */
        char loc[96];
        const asmspy_sym_t *s = syms ? asmspy_symtab_at(syms, rip) : NULL;
        if (s)
            snprintf(loc, sizeof loc, "%s+0x%llx [%s]", s->name,
                     (unsigned long long)(rip - s->addr), s->module);
        else
            snprintf(loc, sizeof loc, "0x%llx", (unsigned long long)rip);

        char line[320];
        snprintf(line, sizeof line, "%-44.44s %s", loc, dis[0] ? dis : "(?)");
        if (sink)
            sink(ctx, line);
        done++;
    }

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return 0;
}
