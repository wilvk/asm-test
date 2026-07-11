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
#include <dirent.h>
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
    case ASMSPY_REGION_NEVER_RAN:
        m = "region never observed executing (multi-threaded target? --trace "
            "follows only the main thread)";
        break;
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

/* ================================================================== */
/* Multi-thread ptrace following (shared by the syscall + stream loops) */
/*                                                                      */
/* ptrace is per-thread, so to watch a whole process we PTRACE_SEIZE     */
/* every thread and — via PTRACE_O_TRACECLONE — every thread it later    */
/* spawns, then drive them all from one waitpid(-1, __WALL) loop on this */
/* thread. SEIZE (not ATTACH) lets new threads auto-attach and lets us   */
/* set options atomically at attach.                                     */
/* ================================================================== */

#ifndef __WALL
#define __WALL 0x40000000 /* wait for children AND threads (clone tasks) */
#endif

/* PTRACE_GET_SYSCALL_INFO (Linux 5.3+) says authoritatively whether a
 * syscall-stop is an ENTRY or an EXIT — immune to the desync a bare entry/exit
 * toggle suffers when we SEIZE a thread already blocked inside a syscall (its
 * next stop is that syscall's EXIT, which a toggle would miscount as an entry). */
#ifndef PTRACE_GET_SYSCALL_INFO
#define PTRACE_GET_SYSCALL_INFO 0x420e
#endif
#ifndef PTRACE_SYSCALL_INFO_ENTRY
#define PTRACE_SYSCALL_INFO_ENTRY 1
#endif
#ifndef PTRACE_SYSCALL_INFO_EXIT
#define PTRACE_SYSCALL_INFO_EXIT 2
#endif

/* 1 = entry, 0 = exit, -1 = unknown (pre-5.3 kernel; caller uses its toggle). */
static int syscall_stop_is_entry(pid_t tid) {
    unsigned char info[64] = {0}; /* .op is byte 0; that is all we read */
    long r = ptrace(PTRACE_GET_SYSCALL_INFO, tid, (void *)sizeof info, info);
    if (r <= 0)
        return -1;
    if (info[0] == PTRACE_SYSCALL_INFO_ENTRY)
        return 1;
    if (info[0] == PTRACE_SYSCALL_INFO_EXIT)
        return 0;
    return -1;
}

/* Per-thread syscall entry/exit bookkeeping (the stream loop uses only .tid). */
typedef struct {
    pid_t tid;
    int at_entry;                  /* fallback toggle: next stop is an ENTRY */
    long ent_nr;                   /* syscall nr captured at entry, -1 if none */
    struct user_regs_struct entry; /* register snapshot at that entry */
    int pending_call;   /* graph engine: an INDIRECT call awaits its callee entry */
    uint64_t call_site; /* graph engine: call-site addr of that pending call     */
    int depth;          /* tree engine: live call depth (push on call, pop on ret) */
    unsigned long long inv; /* procs engine: per-task invocation count (syscalls/calls) */
} thr_t;

typedef struct {
    thr_t *v;
    size_t n, cap;
} thr_tab_t;

static int thr_find(const thr_tab_t *t, pid_t tid) {
    for (size_t i = 0; i < t->n; i++)
        if (t->v[i].tid == tid)
            return (int)i;
    return -1;
}

/* Find `tid`, adding a fresh entry if absent. NULL only on allocation failure. */
static thr_t *thr_get(thr_tab_t *t, pid_t tid) {
    int i = thr_find(t, tid);
    if (i >= 0)
        return &t->v[i];
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 16;
        thr_t *nv = realloc(t->v, nc * sizeof *nv);
        if (!nv)
            return NULL;
        t->v = nv;
        t->cap = nc;
    }
    thr_t *e = &t->v[t->n++];
    e->tid = tid;
    e->at_entry = 1;
    e->ent_nr = -1;
    memset(&e->entry, 0, sizeof e->entry);
    e->pending_call = 0;
    e->call_site = 0;
    e->depth = 0;
    e->inv = 0;
    return e;
}

static void thr_del(thr_tab_t *t, pid_t tid) {
    int i = thr_find(t, tid);
    if (i >= 0)
        t->v[i] = t->v[--t->n]; /* order is irrelevant */
}

/* SEIZE every thread of `pid` with `opts` and INTERRUPT each so it stops and can
 * be kicked into tracing. Re-scan /proc/<pid>/task until a full pass adds none,
 * closing the race where a not-yet-seized thread spawns another (a thread
 * spawned by an ALREADY-seized thread is auto-caught by PTRACE_O_TRACECLONE).
 * Returns 0 once the MAIN thread is seized (a secondary thread that vanished
 * meanwhile is skipped); -1 if even the main thread cannot be seized. */
static int seize_threads(pid_t pid, long opts, thr_tab_t *tab) {
    if (ptrace(PTRACE_SEIZE, pid, NULL, (void *)opts) != 0)
        return -1;
    ptrace(PTRACE_INTERRUPT, pid, NULL, NULL);
    if (!thr_get(tab, pid)) {
        /* OOM tabling the leader: DETACH so the target is not left seize-stopped
         * for asmspy's lifetime (the empty table means detach_threads can't). */
        ptrace(PTRACE_DETACH, pid, NULL, NULL);
        return -1;
    }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)pid);
    for (int pass = 0; pass < 16; pass++) {
        DIR *d = opendir(path);
        if (!d)
            break;
        int added = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            pid_t tid = (pid_t)strtol(de->d_name, NULL, 10);
            if (tid <= 0 || thr_find(tab, tid) >= 0)
                continue;
            if (ptrace(PTRACE_SEIZE, tid, NULL, (void *)opts) == 0) {
                ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
                if (thr_get(tab, tid))
                    added = 1;
                else
                    /* OOM: can't track it — DETACH rather than strand it seized. */
                    ptrace(PTRACE_DETACH, tid, NULL, NULL);
            }
        }
        closedir(d);
        if (!added)
            break; /* stable: a full pass found no new thread */
    }
    return 0;
}

/* x86 EFLAGS Trap Flag: set by PTRACE_SINGLESTEP to fire a #DB after one insn. */
#define ASMSPY_EFLAGS_TF 0x100UL

/* Clear the Trap Flag on a stopped tracee before detaching. A single-step engine
 * leaves each thread resumed with PTRACE_SINGLESTEP (TF set); if we INTERRUPT and
 * DETACH while a thread's step is still armed, it resumes WITH TF set, executes
 * one instruction, and takes a #DB -> SIGTRAP with no tracer to absorb it, which
 * TERMINATES the tracee (fatal SIGTRAP). Debuggers avoid this by clearing TF
 * before detach; the kernel's own clear on detach does not cover a detach from
 * the INTERRUPT group-stop of a mid-step thread. NB: only clears TF the tracer
 * forced — if the app itself was single-stepping (rare) TF stays as it was, since
 * we never re-armed it here. */
static void clear_trap_flag(pid_t tid) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0)
        return;
    if (regs.eflags & ASMSPY_EFLAGS_TF) {
        regs.eflags &= ~ASMSPY_EFLAGS_TF;
        ptrace(PTRACE_SETREGS, tid, NULL, &regs);
    }
}

/* Stop tracing: INTERRUPT each tracee, drain to its next ptrace-stop, clear any
 * armed single-step (TF), then DETACH so it runs on normally. A thread already
 * gone is simply reaped. Frees the table. */
static void detach_threads(thr_tab_t *tab) {
    /* TWO-PHASE detach. A whole-process single-step run leaves every thread SEIZEd
     * and step-armed. Detaching them one-at-a-time lets an already-detached thread
     * resume and RUN while its siblings are still stopped mid-step — in a JIT /
     * managed runtime (V8/Node, JVM, …) that transient cross-thread inconsistency
     * trips an internal self-check that IMMEDIATE_CRASHes via int3 -> a fatal
     * SIGTRAP that kills the whole process. Phase 1 stops EVERY thread (clearing
     * any armed single-step); phase 2 releases them all, so none runs until all
     * are quiescent — mirroring the kernel's own all-at-once detach on tracer
     * death (which is precisely why a killed tracer leaves the target alive). */
    for (size_t i = 0; i < tab->n; i++) { /* phase 1: interrupt + drain to a stop */
        pid_t tid = tab->v[i].tid;
        ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
        for (;;) {
            int st;
            pid_t r = waitpid(tid, &st, __WALL);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                break; /* ECHILD — already gone */
            }
            if (WIFEXITED(st) || WIFSIGNALED(st))
                break; /* gone; nothing to detach */
            if (WIFSTOPPED(st)) {
                clear_trap_flag(tid); /* drop any armed single-step (defensive) */
                break;                /* leave it STOPPED; release in phase 2 */
            }
        }
    }
    for (size_t i = 0; i < tab->n; i++) /* phase 2: release all at once */
        ptrace(PTRACE_DETACH, tab->v[i].tid, NULL, NULL);
    free(tab->v);
    tab->v = NULL;
    tab->n = tab->cap = 0;
}

/* A SIGTRAP the tracee delivered to ITSELF — it executed its own int3 (a JIT or
 * debugger software breakpoint) or hit its own hardware breakpoint — as opposed
 * to the single-step trap WE induced. Distinguished by the stop's si_code
 * (PTRACE_GETSIGINFO). Because we single-step, our own traps report si_code
 * TRAP_TRACE (an ordinary step) or, on x86, TRAP_BRKPT (a step COMPLETING ACROSS
 * a syscall — MEASURED on this host: rax holds the return value; it is NOT an app
 * breakpoint). A genuinely executed int3 reports SI_KERNEL on x86 (the kernel's
 * force_sig path, NOT TRAP_BRKPT), and an app hardware breakpoint reports
 * TRAP_HWBKPT. So we deliver ONLY on that positive, unambiguous evidence and
 * swallow everything else — TRAP_TRACE, TRAP_BRKPT, and SI_USER (which also
 * covers the execve trap), SI_TKILL, SI_QUEUE, GETSIGINFO failure. The asymmetry
 * is deliberate: masking a rare externally-injected SIGTRAP is recoverable, but
 * injecting a spurious SIGTRAP into a target with no handler KILLS it (the
 * fatal-SIGTRAP class the crash-safe two-phase detach fought). Returns 1 to
 * deliver, 0 to swallow (treat as our step). */
static int sigtrap_is_app(pid_t tid) {
    siginfo_t si;
    if (ptrace(PTRACE_GETSIGINFO, tid, NULL, &si) != 0)
        return 0; /* can't tell — assume ours; mis-delivery is the fatal direction */
    return si.si_code == SI_KERNEL || si.si_code == TRAP_HWBKPT;
}

/* Re-inject an app-delivered SIGTRAP so the tracee handles its own breakpoint.
 * MUST use PTRACE_CONT, never PTRACE_SINGLESTEP+signal: re-arming the Trap Flag
 * fires a #DB on the first instruction INSIDE the app's SIGTRAP handler, where
 * SIGTRAP is masked (default sa_mask), which the kernel forces to SIG_DFL and
 * KILLS the whole target (MEASURED, deterministic — the same fatal-SIGTRAP class
 * as a step-armed detach). CONT delivers the signal without re-arming TF; the
 * thread then runs free until its next ptrace-stop, where the engine resumes
 * single-stepping it. */
static void deliver_app_sigtrap(pid_t tid) {
    ptrace(PTRACE_CONT, tid, NULL, (void *)(long)SIGTRAP);
}

int asmspy_engine_syscalls(pid_t pid, long max, atomic_bool *stop,
                           asmspy_syscall_sink sink, void *ctx) {
    arm_quit_wake();

    thr_tab_t tab = {0};
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE;
    if (seize_threads(pid, opts, &tab) != 0) {
        detach_threads(&tab); /* frees the (empty) table */
        return ASMTEST_PTRACE_ETRACE;
    }

    /* Tag each line with its tid once we follow more than one thread, so a
     * multi-threaded target stays legible (like `strace -f`); a single-threaded
     * target keeps the clean, unprefixed line the callers already expect. */
    int multi = tab.n > 1;
    long done = 0;

    while ((max < 0 || done < max) && !(stop && atomic_load(stop))) {
        int status;
        pid_t tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            if (errno == EINTR) {
                if (stop && atomic_load(stop))
                    break;
                continue;
            }
            break; /* ECHILD — every tracee is gone */
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thr_del(&tab, tid);
            if (tab.n == 0)
                break;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child) {
                thr_get(&tab, (pid_t)child); /* also fine if we saw it first */
                multi = 1;
            }
            ptrace(PTRACE_SYSCALL, tid, NULL, NULL); /* resume the parent */
            continue;
        }

        if (sig == (SIGTRAP | 0x80)) { /* syscall-stop (TRACESYSGOOD) */
            thr_t *ts = thr_get(&tab, tid);
            struct user_regs_struct regs;
            if (ts && ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                int entry = syscall_stop_is_entry(tid);
                if (entry < 0)
                    entry = ts->at_entry; /* pre-5.3 fallback toggle */
                if (entry) {
                    ts->ent_nr = (long)regs.orig_rax;
                    ts->entry = regs;
                    ts->at_entry = 0;
                } else if (ts->ent_nr >= 0) { /* skip an exit we saw no entry for */
                    char line[1024], sdata[512], out[1088];
                    /* decode via the leader `pid` (shared mm + fd table); the
                     * per-thread label is added below, not inside the decoder */
                    format_syscall(line, sizeof line, sdata, sizeof sdata, pid,
                                   ts->ent_nr, &ts->entry, (long)regs.rax);
                    const char *emit = line;
                    if (multi) {
                        snprintf(out, sizeof out, "[%d] %s", (int)tid, line);
                        emit = out;
                    }
                    if (sink)
                        sink(ctx, emit, sdata[0] ? sdata : NULL);
                    ts->at_entry = 1;
                    ts->ent_nr = -1;
                    done++;
                } else {
                    ts->at_entry = 1;
                }
            }
            ptrace(PTRACE_SYSCALL, tid, NULL, NULL);
            continue;
        }

        /* A job-control group-stop under SEIZE (^Z / SIGSTOP / tty SIGTTIN/TTOU)
         * arrives as PTRACE_EVENT_STOP with the stopping signal. Resuming it with
         * PTRACE_SYSCALL would keep the target running while it should be suspended;
         * PTRACE_LISTEN leaves it stopped (honoring ^Z) yet traced — it wakes on
         * SIGCONT. */
        if (event == PTRACE_EVENT_STOP &&
            (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
             sig == SIGTTOU)) {
            thr_get(&tab, tid);
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        /* Otherwise: the initial INTERRUPT/EVENT_STOP, a clone child's first
         * stop, an exec-stop, or a real signal-delivery-stop. Register a
         * first-seen tid and resume; forward only a genuine signal. An
         * app-delivered SIGTRAP is intentionally not re-injected (indistinguishable
         * here from a ptrace-synthesized one) — a documented spy limitation. */
        int deliver = 0;
        if (event == 0 && sig != SIGTRAP && sig != (SIGTRAP | 0x80))
            deliver = sig;
        thr_get(&tab, tid);
        ptrace(PTRACE_SYSCALL, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(&tab);
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
    /* Detached cleanly but never saw the region run (and the user didn't quit):
     * report it distinctly so the caller can hint that a multi-threaded target may
     * execute the function on a worker thread — run_to only steps the leader here. */
    if (sample == 0 && !(stop && atomic_load(stop)))
        return ASMSPY_REGION_NEVER_RAN;
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

    /* Same whole-process following as the syscall stream, but single-stepping:
     * SEIZE every thread, then PTRACE_SINGLESTEP each so the live instruction
     * stream is genuinely whole-process, interleaving all threads (not just the
     * one we happened to attach). No TRACESYSGOOD — every stop here is a step. */
    thr_tab_t tab = {0};
    if (seize_threads(pid, PTRACE_O_TRACECLONE, &tab) != 0) {
        detach_threads(&tab);
        return ASMTEST_PTRACE_ETRACE;
    }

    int multi = tab.n > 1;
    asmspy_jitmap_t jit; /* name JIT / managed-runtime frames from the perf-map */
    asmspy_jitmap_init(&jit, pid);
    asmspy_jitmap_refresh(&jit); /* pick up methods already compiled at attach */
    long done = 0;

    while ((max < 0 || done < max) && !(stop && atomic_load(stop))) {
        int status;
        pid_t tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            if (errno == EINTR) {
                if (stop && atomic_load(stop))
                    break;
                continue;
            }
            break; /* ECHILD — every tracee is gone */
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thr_del(&tab, tid);
            if (tab.n == 0)
                break;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child) {
                thr_get(&tab, (pid_t)child);
                multi = 1;
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (sig == SIGTRAP && event == 0) { /* our step, or the app's own trap */
            if (sigtrap_is_app(tid)) { /* target's own int3/hw bp — deliver it */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                uint64_t rip = regs.rip;

                /* disassemble the instruction about to retire */
                uint8_t code[16];
                char dis[160] = "";
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
                /* read via the thread-group leader `pid`: all threads share the
                 * address space, and process_vm_readv on a non-leader tid can be
                 * refused, which would blank the disassembly */
                ssize_t got = process_vm_readv(pid, &liov, 1, &riov, 1, 0);
                if (got >= 1 && asmtest_disas_available())
                    asmtest_disas(ASMTEST_ARCH_X86_64, code, (size_t)got, rip, 0,
                                  dis, sizeof dis);

                /* resolve the function this address lands in (ELF, else JIT) */
                char loc[96];
                const asmspy_sym_t *s = asmspy_resolve(syms, &jit, rip);
                if (s)
                    snprintf(loc, sizeof loc, "%s+0x%llx [%s]", s->name,
                             (unsigned long long)(rip - s->addr), s->module);
                else
                    snprintf(loc, sizeof loc, "0x%llx",
                             (unsigned long long)rip);

                char line[320], out[352];
                snprintf(line, sizeof line, "%-44.44s %s", loc,
                         dis[0] ? dis : "(?)");
                const char *emit = line;
                if (multi) {
                    snprintf(out, sizeof out, "[%d] %s", (int)tid, line);
                    emit = out;
                }
                if (sink)
                    sink(ctx, emit);
                done++;
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        /* A job-control group-stop (^Z / SIGSTOP / tty) under SEIZE arrives as
         * PTRACE_EVENT_STOP with the stopping signal: PTRACE_LISTEN so the target
         * stays suspended (honoring ^Z) instead of being single-stepped onward. */
        if (event == PTRACE_EVENT_STOP &&
            (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
             sig == SIGTTOU)) {
            thr_get(&tab, tid);
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        /* the initial INTERRUPT stop, a clone child's first stop, an exec-stop, or
         * a real signal-delivery-stop: resume stepping and forward only a genuine
         * signal. An app-delivered SIGTRAP is intentionally not re-injected
         * (indistinguishable here from the single-step trap). */
        int deliver = 0;
        if (event == 0 && sig != SIGTRAP)
            deliver = sig;
        thr_get(&tab, tid);
        ptrace(PTRACE_SINGLESTEP, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(&tab);
    asmspy_jitmap_free(&jit);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Whole-process call-graph builder                                    */
/* ------------------------------------------------------------------ */

/* An accumulated call edge: caller/callee are indices into graph_t.node. */
typedef struct {
    size_t caller, callee;
    unsigned long long count;
} gedge_t;

typedef struct {
    asmspy_gnode_t *node;
    size_t nn, ncap;
    gedge_t *edge;
    size_t ne, ecap;
} graph_t;

/* Basename of the target's own executable (for the internal/external split);
 * `out` gets "" on any failure (then every node is treated as internal). */
static void exe_basename(pid_t pid, char *out, size_t cap) {
    char path[64], link[512];
    snprintf(path, sizeof path, "/proc/%d/exe", (int)pid);
    ssize_t n = readlink(path, link, sizeof link - 1);
    if (n <= 0) {
        out[0] = '\0';
        return;
    }
    link[n] = '\0';
    const char *b = strrchr(link, '/');
    snprintf(out, cap, "%s", b ? b + 1 : link);
}

/* Find (or create) the node for the function CONTAINING `addr`. Nodes dedup by
 * the containing function's base, so every call site inside one function folds
 * into a single caller node (and every entry of a callee into one node).
 * Returns the node index, or -1 on OOM. */
static long gnode_get(graph_t *g, const asmspy_symtab_t *syms,
                      asmspy_jitmap_t *jit, const char *exebase, uint64_t addr) {
    const asmspy_sym_t *s = asmspy_resolve(syms, jit, addr);
    uint64_t key = s ? s->addr : addr;
    for (size_t i = 0; i < g->nn; i++)
        if (g->node[i].addr == key)
            return (long)i;
    if (g->nn == g->ncap) {
        size_t nc = g->ncap ? g->ncap * 2 : 64;
        asmspy_gnode_t *nv = realloc(g->node, nc * sizeof *nv);
        if (!nv)
            return -1;
        g->node = nv;
        g->ncap = nc;
    }
    asmspy_gnode_t *e = &g->node[g->nn];
    memset(e, 0, sizeof *e);
    e->addr = key;
    if (s) {
        snprintf(e->name, sizeof e->name, "%s", s->name);
        snprintf(e->module, sizeof e->module, "%s", s->module);
        /* external = a shared/system library, OR a PLT thunk to an imported
         * function (the stub lives in the exe but the call leaves to a library);
         * internal = the target's own executable code */
        size_t nl = strlen(s->name);
        int is_plt = nl >= 4 && strcmp(s->name + nl - 4, "@plt") == 0;
        /* JIT/managed code (module "jit") is the app's OWN logic, not a system
         * library, so it counts as internal despite having no exe-backed file. */
        int is_jit = strcmp(s->module, "jit") == 0;
        e->external =
            !is_jit && (is_plt || (exebase && exebase[0] &&
                                   strcmp(s->module, exebase) != 0));
    } else {
        snprintf(e->name, sizeof e->name, "0x%llx", (unsigned long long)key);
        snprintf(e->module, sizeof e->module, "?"); /* row shows [?] */
        e->external = 0;
    }
    return (long)g->nn++;
}

/* Record one caller->callee call. Returns 1 if a call was counted (0 on OOM). */
static int grecord(graph_t *g, const asmspy_symtab_t *syms,
                   asmspy_jitmap_t *jit, const char *exebase,
                   uint64_t caller_addr, uint64_t callee_addr) {
    long ci = gnode_get(g, syms, jit, exebase, caller_addr);
    long ki = gnode_get(g, syms, jit, exebase, callee_addr);
    if (ci < 0 || ki < 0)
        return 0;
    for (size_t i = 0; i < g->ne; i++) {
        if (g->edge[i].caller == (size_t)ci && g->edge[i].callee == (size_t)ki) {
            g->edge[i].count++;
            g->node[ci].out_calls++;
            g->node[ki].invocations++;
            return 1;
        }
    }
    if (g->ne == g->ecap) {
        size_t nc = g->ecap ? g->ecap * 2 : 128;
        gedge_t *nv = realloc(g->edge, nc * sizeof *nv);
        if (!nv)
            return 0;
        g->edge = nv;
        g->ecap = nc;
    }
    g->edge[g->ne].caller = (size_t)ci;
    g->edge[g->ne].callee = (size_t)ki;
    g->edge[g->ne].count = 1;
    g->ne++;
    g->node[ci].out_calls++;
    g->node[ci].fanout++; /* a callee not seen from this caller before */
    g->node[ki].invocations++;
    return 1;
}

int asmspy_engine_graph(pid_t pid, long max, atomic_bool *stop,
                        const asmspy_symtab_t *syms, asmspy_graph_sink sink,
                        void *ctx) {
    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    thr_tab_t tab = {0};
    if (seize_threads(pid, PTRACE_O_TRACECLONE, &tab) != 0) {
        detach_threads(&tab);
        return ASMTEST_PTRACE_ETRACE;
    }

    char exebase[64];
    exe_basename(pid, exebase, sizeof exebase);

    graph_t g = {0};
    asmspy_jitmap_t jit; /* name JIT / managed-runtime frames from the perf-map */
    asmspy_jitmap_init(&jit, pid);
    asmspy_jitmap_refresh(&jit); /* pick up methods already compiled at attach */
    long recorded = 0;   /* calls counted so far */
    long published = -1; /* recorded value at the last sink() call */

    while ((max < 0 || recorded < max) && !(stop && atomic_load(stop))) {
        int status;
        pid_t tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            if (errno == EINTR) {
                if (stop && atomic_load(stop))
                    break;
                continue;
            }
            break; /* ECHILD — every tracee is gone */
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thr_del(&tab, tid);
            if (tab.n == 0)
                break;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child)
                thr_get(&tab, (pid_t)child);
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (sig == SIGTRAP && event == 0) { /* our step, or the app's own trap */
            if (sigtrap_is_app(tid)) { /* target's own int3/hw bp — deliver it */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                uint64_t rip = regs.rip;
                thr_t *ts = thr_get(&tab, tid);

                /* read the bytes of the instruction about to retire */
                uint8_t code[16];
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
                ssize_t got = process_vm_readv(pid, &liov, 1, &riov, 1, 0);

                /* consume a pending INDIRECT call: this rip is its callee entry */
                if (ts && ts->pending_call) {
                    recorded +=
                        grecord(&g, syms, &jit, exebase, ts->call_site, rip);
                    ts->pending_call = 0;
                }

                if (got >= 1 && asmtest_disas_available() &&
                    asmtest_disas_is_call(ASMTEST_ARCH_X86_64, code, (size_t)got,
                                          0)) {
                    uint64_t tgt = 0;
                    if (asmtest_disas_call_target(ASMTEST_ARCH_X86_64, code,
                                                  (size_t)got, rip, 0, &tgt)) {
                        /* DIRECT call: target known now — record immediately */
                        recorded += grecord(&g, syms, &jit, exebase, rip, tgt);
                    } else if (ts) {
                        /* INDIRECT call: resolve the callee at the next step */
                        ts->pending_call = 1;
                        ts->call_site = rip;
                    }
                }

                /* throttle live snapshots (single-step is slow; 16 calls/pub) */
                if (sink && recorded - published >= 16) {
                    sink(ctx, g.node, g.nn);
                    published = recorded;
                }
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (event == PTRACE_EVENT_STOP &&
            (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
             sig == SIGTTOU)) {
            thr_get(&tab, tid);
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        int deliver = 0;
        if (event == 0 && sig != SIGTRAP)
            deliver = sig;
        thr_get(&tab, tid);
        ptrace(PTRACE_SINGLESTEP, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(&tab);
    if (sink) /* always hand over a final snapshot */
        sink(ctx, g.node, g.nn);
    free(g.node);
    free(g.edge);
    asmspy_jitmap_free(&jit);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Whole-process live call TREE                                        */
/* ------------------------------------------------------------------ */

/* Emit one "entered a function" line, indented by the caller's live depth. The
 * callee's entry address is passed through so a front-end can disassemble it. */
static void tree_emit(asmspy_tree_sink sink, void *ctx, int multi, pid_t tid,
                      const asmspy_symtab_t *syms, asmspy_jitmap_t *jit,
                      uint64_t callee, int depth) {
    if (!sink)
        return;
    const asmspy_sym_t *s = asmspy_resolve(syms, jit, callee);
    char name[200];
    if (s)
        snprintf(name, sizeof name, "%s [%s]", s->name, s->module);
    else
        snprintf(name, sizeof name, "0x%llx", (unsigned long long)callee);
    int ind = depth * 2;
    if (ind > 60) /* clamp runaway / drifted indentation to a sane width */
        ind = 60;
    char line[320];
    if (multi)
        snprintf(line, sizeof line, "[%d] %*s-> %s", (int)tid, ind, "", name);
    else
        snprintf(line, sizeof line, "%*s-> %s", ind, "", name);
    sink(ctx, line, callee);
}

int asmspy_engine_tree(pid_t pid, long max, atomic_bool *stop,
                       const asmspy_symtab_t *syms, asmspy_tree_sink sink,
                       void *ctx) {
    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    thr_tab_t tab = {0};
    if (seize_threads(pid, PTRACE_O_TRACECLONE, &tab) != 0) {
        detach_threads(&tab);
        return ASMTEST_PTRACE_ETRACE;
    }

    int multi = tab.n > 1;
    asmspy_jitmap_t jit; /* name JIT / managed-runtime frames from the perf-map */
    asmspy_jitmap_init(&jit, pid);
    asmspy_jitmap_refresh(&jit); /* pick up methods already compiled at attach */
    long emitted = 0; /* call lines produced so far */

    while ((max < 0 || emitted < max) && !(stop && atomic_load(stop))) {
        int status;
        pid_t tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            if (errno == EINTR) {
                if (stop && atomic_load(stop))
                    break;
                continue;
            }
            break;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thr_del(&tab, tid);
            if (tab.n == 0)
                break;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child) {
                thr_get(&tab, (pid_t)child);
                multi = 1;
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (sig == SIGTRAP && event == 0) { /* our step, or the app's own trap */
            if (sigtrap_is_app(tid)) { /* target's own int3/hw bp — deliver it */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                uint64_t rip = regs.rip;
                thr_t *ts = thr_get(&tab, tid);

                uint8_t code[16];
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
                ssize_t got = process_vm_readv(pid, &liov, 1, &riov, 1, 0);

                /* a pending INDIRECT call resolves at its callee entry (= rip) */
                if (ts && ts->pending_call) {
                    tree_emit(sink, ctx, multi, tid, syms, &jit, rip, ts->depth);
                    ts->depth++;
                    ts->pending_call = 0;
                    emitted++;
                }

                int isc = 0, isr = 0;
                if (got >= 1 && asmtest_disas_available())
                    asmtest_disas_probe(ASMTEST_ARCH_X86_64, code, (size_t)got, 0,
                                        &isc, &isr);
                if (isc && ts) {
                    uint64_t tgt = 0;
                    if (asmtest_disas_call_target(ASMTEST_ARCH_X86_64, code,
                                                  (size_t)got, rip, 0, &tgt)) {
                        tree_emit(sink, ctx, multi, tid, syms, &jit, tgt,
                                  ts->depth);
                        ts->depth++;
                        emitted++;
                    } else {
                        ts->pending_call = 1; /* indirect: resolve next step */
                        ts->call_site = rip;
                    }
                } else if (isr && ts && ts->depth > 0) {
                    ts->depth--; /* returned: pop a level (clamped at 0) */
                }
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (event == PTRACE_EVENT_STOP &&
            (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
             sig == SIGTTOU)) {
            thr_get(&tab, tid);
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        int deliver = 0;
        if (event == 0 && sig != SIGTRAP)
            deliver = sig;
        thr_get(&tab, tid);
        ptrace(PTRACE_SINGLESTEP, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(&tab);
    asmspy_jitmap_free(&jit);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Process/thread topology (whole tree: procs + threads + children)    */
/* ------------------------------------------------------------------ */

/* Read a task's process id (Tgid), parent pid (PPid) and name (comm) from
 * /proc. Best-effort: fields keep their passed-in defaults on any failure. */
static void read_task_meta(pid_t tid, pid_t *tgid, pid_t *ppid, char *comm,
                           size_t cz) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)tid);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "Tgid:", 5) == 0)
                *tgid = (pid_t)strtol(line + 5, NULL, 10);
            else if (strncmp(line, "PPid:", 5) == 0)
                *ppid = (pid_t)strtol(line + 5, NULL, 10);
        }
        fclose(f);
    }
    if (comm && cz) {
        comm[0] = '\0';
        snprintf(path, sizeof path, "/proc/%d/comm", (int)tid);
        FILE *c = fopen(path, "r");
        if (c) {
            if (fgets(comm, (int)cz, c)) {
                size_t l = strlen(comm);
                if (l && comm[l - 1] == '\n')
                    comm[l - 1] = '\0';
            }
            fclose(c);
        }
    }
}

/* Build one topology snapshot (one asmspy_task_t per tracked task) and hand it to
 * the sink. tgid/ppid/comm are re-read each publish so an exec/rename shows up. */
static void topo_publish(thr_tab_t *tab, asmspy_topo_sink sink, void *ctx,
                         asmspy_task_t **buf, size_t *bufcap) {
    if (!sink || tab->n == 0)
        return;
    if (tab->n > *bufcap) {
        asmspy_task_t *nb = realloc(*buf, tab->n * sizeof *nb);
        if (!nb)
            return; /* keep the last snapshot on OOM */
        *buf = nb;
        *bufcap = tab->n;
    }
    for (size_t i = 0; i < tab->n; i++) {
        asmspy_task_t *t = &(*buf)[i];
        memset(t, 0, sizeof *t);
        t->tid = tab->v[i].tid;
        t->tgid = t->tid; /* default if the /proc read fails */
        read_task_meta(t->tid, &t->tgid, &t->ppid, t->comm, sizeof t->comm);
        t->is_leader = (t->tid == t->tgid);
        if (t->is_leader)
            exe_basename(t->tgid, t->exe, sizeof t->exe);
        t->inv = tab->v[i].inv;
    }
    sink(ctx, *buf, tab->n);
}

static int tgid_tracked(const pid_t *set, size_t n, pid_t g) {
    for (size_t i = 0; i < n; i++)
        if (set[i] == g)
            return 1;
    return 0;
}

/* Seize the target AND its whole EXISTING descendant tree (children, grandchildren
 * — processes reachable by PPid), each with its threads. TRACEFORK only catches
 * forks AFTER attach, so children that predate the attach must be discovered here:
 * rescan /proc, seizing any process whose parent we already track, until stable.
 * Best-effort per descendant (an un-ptraceable child is simply skipped). Returns 0
 * once the root is seized, -1 if even the root cannot be. */
static int seize_process_tree(pid_t pid, long opts, thr_tab_t *tab) {
    if (seize_threads(pid, opts, tab) != 0)
        return -1;
    pid_t *set = malloc(64 * sizeof *set);
    if (!set)
        return 0; /* root is seized; degrade to single-process */
    size_t nset = 0, setcap = 64;
    set[nset++] = pid;
    for (int pass = 0; pass < 64; pass++) {
        DIR *d = opendir("/proc");
        if (!d)
            break;
        int added = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] < '0' || de->d_name[0] > '9')
                continue;
            pid_t p = (pid_t)strtol(de->d_name, NULL, 10);
            if (p <= 0 || tgid_tracked(set, nset, p))
                continue;
            pid_t tg = p, pp = 0;
            read_task_meta(p, &tg, &pp, NULL, 0);
            if (!tgid_tracked(set, nset, pp)) /* parent not (yet) tracked */
                continue;
            if (seize_threads(p, opts, tab) == 0) { /* best-effort */
                if (nset == setcap) {
                    size_t nc = setcap * 2;
                    pid_t *nv = realloc(set, nc * sizeof *nv);
                    if (!nv)
                        break;
                    set = nv;
                    setcap = nc;
                }
                set[nset++] = p;
                added = 1;
            }
        }
        closedir(d);
        if (!added)
            break;
    }
    free(set);
    return 0;
}

int asmspy_engine_procs(pid_t pid, long max, atomic_bool *stop,
                        asmspy_count_t mode, asmspy_topo_sink sink, void *ctx) {
    if (mode == ASMSPY_COUNT_CALLS && !asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    thr_tab_t tab = {0};
    /* Follow every thread (CLONE) AND every child PROCESS (FORK/VFORK), and pick
     * up exec so names/exe stay fresh. Syscall mode also wants TRACESYSGOOD. */
    long opts = PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                PTRACE_O_TRACEEXEC;
    if (mode == ASMSPY_COUNT_SYSCALLS)
        opts |= PTRACE_O_TRACESYSGOOD;
    if (seize_process_tree(pid, opts, &tab) != 0) {
        detach_threads(&tab);
        return ASMTEST_PTRACE_ETRACE;
    }

/* resume a tracee the way this mode drives it: syscall-stops vs single-steps */
#define TOPO_RESUME(t, s)                                                       \
    ptrace(mode == ASMSPY_COUNT_SYSCALLS ? PTRACE_SYSCALL : PTRACE_SINGLESTEP,  \
           (t), NULL, (void *)(long)(s))

    asmspy_task_t *snap = NULL;
    size_t snapcap = 0;
    long counted = 0, published = -1;

    while ((max < 0 || counted < max) && !(stop && atomic_load(stop))) {
        int status;
        pid_t tid = waitpid(-1, &status, __WALL);
        if (tid < 0) {
            if (errno == EINTR) {
                if (stop && atomic_load(stop))
                    break;
                continue;
            }
            break;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            thr_del(&tab, tid);
            if (tab.n == 0)
                break;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        int event = (status >> 16) & 0xff;

        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child)
                thr_get(&tab, (pid_t)child); /* a new thread OR child process */
            TOPO_RESUME(tid, 0);
            continue;
        }

        if (mode == ASMSPY_COUNT_SYSCALLS && sig == (SIGTRAP | 0x80)) {
            thr_t *ts = thr_get(&tab, tid);
            if (ts) { /* count one per syscall (on the exit half of the pair) */
                int entry = syscall_stop_is_entry(tid);
                if (entry < 0)
                    entry = ts->at_entry;
                if (entry) {
                    ts->at_entry = 0;
                } else {
                    ts->at_entry = 1;
                    ts->inv++;
                    counted++;
                }
            }
            TOPO_RESUME(tid, 0);
            if (counted - published >= 32) {
                topo_publish(&tab, sink, ctx, &snap, &snapcap);
                published = counted;
            }
            continue;
        }

        if (mode == ASMSPY_COUNT_CALLS && sig == SIGTRAP && event == 0) {
            if (sigtrap_is_app(tid)) { /* target's own int3/hw bp — deliver it
                                        * (CONT, not the single-stepping RESUME) */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                thr_t *ts = thr_get(&tab, tid);
                uint8_t code[16];
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)regs.rip, sizeof code};
                ssize_t got = process_vm_readv(pid, &liov, 1, &riov, 1, 0);
                if (ts && got >= 1 && asmtest_disas_available() &&
                    asmtest_disas_is_call(ASMTEST_ARCH_X86_64, code, (size_t)got,
                                          0)) {
                    ts->inv++; /* count every CALL this task executes */
                    counted++;
                }
            }
            TOPO_RESUME(tid, 0);
            if (counted - published >= 32) {
                topo_publish(&tab, sink, ctx, &snap, &snapcap);
                published = counted;
            }
            continue;
        }

        if (event == PTRACE_EVENT_STOP &&
            (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
             sig == SIGTTOU)) {
            thr_get(&tab, tid);
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        int deliver = 0;
        if (event == 0 && sig != SIGTRAP && sig != (SIGTRAP | 0x80))
            deliver = sig;
        thr_get(&tab, tid);
        TOPO_RESUME(tid, deliver);
    }

#undef TOPO_RESUME
    if (sink) /* final snapshot BEFORE detach frees the table */
        topo_publish(&tab, sink, ctx, &snap, &snapcap);
    detach_threads(&tab);
    free(snap);
    return 0;
}
