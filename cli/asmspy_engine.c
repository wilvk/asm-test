/*
 * asmspy_engine.c — the two out-of-process tracer engines behind asmspy.
 *
 *   asmspy_engine_syscalls() — PTRACE_SYSCALL stream, decoding data (a strace)
 *   asmspy_engine_region()   — race-to-entry + trace_attached_ex sampler
 *                              (asm + calls), on whichever thread arrives first
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
#include "asmtest_codeimage.h" /* asmtest_codeimage_t — the attach_jit versioned-decode param (unused today, NULL) */
#include "asmtest_ibs.h" /* the out-of-band statistical sampler (asmspy_engine_sample) */

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
    case 0:
        m = "ok";
        break;
    case ASMSPY_REGION_NEVER_RAN:
        m = "region never observed executing (multi-threaded target? --trace "
            "follows only the main thread)";
        break;
    case ASMSPY_DATAFLOW_UNAVAIL:
        m = "data-flow value producer unavailable (off Linux x86-64 / built "
            "without Capstone)";
        break;
    case ASMSPY_ETRACEE_I386:
        /* Must fit the callers' char[128] or it truncates mid-sentence
         * (measured). The detail lives in asmspy.h; this is the operator's
         * one-liner: what is wrong, and what to do instead. */
        m = "32-bit (i386) tracee — unsupported (asmspy is x86-64-only and "
            "would MISDECODE it). Trace a 64-bit process";
        break;
    case ASMSPY_WATCH_UNAVAIL:
        m = "hardware data watchpoint unavailable (off x86-64, or "
            "debug-register "
            "arming refused: permission / seccomp / qemu)";
        break;
    case ASMTEST_PTRACE_EINVAL:
        m = "invalid argument";
        break;
    case ASMTEST_PTRACE_EUNAVAIL:
        m = "tracer unavailable on this host";
        break;
    case ASMTEST_PTRACE_ENOSYS:
        m = "backend not built in";
        break;
    case ASMTEST_PTRACE_ENOENT:
        m = "region/symbol not found";
        break;
    case ASMTEST_PTRACE_ETRACE:
        m = "ptrace/attach failure (permission? ptrace_scope? on a managed "
            "target, possibly a W^X JIT page refusing the entry breakpoint)";
        break;
    default:
        m = "attach failed";
        break;
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
    case SYS_symlink:   /* (target, linkpath) — target is the datum */
    case SYS_symlinkat: /* (target, newdirfd, linkpath) — likewise */
    case SYS_creat:
    case SYS_mknod:
    case SYS_utimes:
    case SYS_chroot:
    case SYS_acct:
    case SYS_mount: /* (source, target, ...) */
    case SYS_umount2:
    case SYS_swapon:
    case SYS_swapoff:
        return PATH_RDI;
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
    default:
        return PATH_NONE;
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
                           pid_t pid, long nr, const struct user_regs_struct *e,
                           long ret) {
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
                    (unsigned long long)e->rsi, (unsigned long long)e->rdx,
                    ret);
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
    pid_t tgid;   /* thread-group (process) this task belongs to. 0 = not yet
                   * known. Only meaningful once --follow is on: a cloned THREAD
                   * shares its parent's mm/fd table, but a FORKED CHILD shares
                   * neither, so every read keyed off the original leader `pid`
                   * is wrong for it. */
    int at_entry; /* fallback toggle: next stop is an ENTRY */
    long ent_nr;  /* syscall nr captured at entry, -1 if none */
    struct user_regs_struct entry; /* register snapshot at that entry */
    int pending_call; /* graph engine: an INDIRECT call awaits its callee entry */
    uint64_t
        call_site; /* graph engine: call-site addr of that pending call     */
    int depth;     /* tree engine: live call depth (push on call, pop on ret) */
    int focus_depth; /* tree engine: real depth at which this thread entered the
                      * --focus subtree, ASMSPY_TF_NO_FOCUS when outside one.
                      * Per-thread because thread A being inside the focused
                      * function says nothing about thread B.                 */
    unsigned long long
        inv;     /* procs engine: per-task invocation count (syscalls/calls) */
    int armed;   /* watch engine: its debug-register watchpoint is installed  */
    int stopped; /* region engine: we consumed its stop and left it stopped.
                  * Must be tracked, not probed: PTRACE_INTERRUPT on a thread
                  * already in a ptrace-stop yields no new event, so a blind
                  * interrupt-then-waitpid at detach would block forever. */
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

/* TEST-ONLY fault injection for the thr_get OOM path (asmspy-plan Theme C).
 *
 * The bug this guards — an untabled task resumed anyway, escaping the two-phase
 * detach and dying later of a SIGTRAP that looks like the target's own — is
 * reachable only through a mid-trace allocation failure, which cannot be
 * provoked from outside and does not announce itself when it happens. Without a
 * lever, the fix could only ever be argued, not demonstrated.
 *
 * ASMSPY_TEST_THR_OOM=<n> allows `n` tasks to be tabled and fails every NEW one
 * after that (sticky, so the untabled task stays untabled on every later stop —
 * which is what a real OOM would do). Unset = disabled, the normal path.
 * Returns 1 if this insertion must fail. */
static int thr_table_full_for_test(const thr_tab_t *t) {
    static int cap = -2; /* -2 = unread, -1 = disabled */
    if (cap == -2) {
        const char *e = getenv("ASMSPY_TEST_THR_OOM");
        cap = (e && *e) ? atoi(e) : -1;
    }
    return cap >= 0 && t->n >= (size_t)cap;
}

/* Find `tid`, adding a fresh entry if absent. NULL only on allocation failure. */
static thr_t *thr_get(thr_tab_t *t, pid_t tid) {
    int i = thr_find(t, tid);
    if (i >= 0)
        return &t->v[i];
    if (thr_table_full_for_test(t))
        return NULL; /* injected OOM (ASMSPY_TEST_THR_OOM) */
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
    e->tgid = 0; /* resolved lazily (thr_tgid) or set by the clone/fork event */
    e->at_entry = 1;
    e->ent_nr = -1;
    memset(&e->entry, 0, sizeof e->entry);
    e->pending_call = 0;
    e->call_site = 0;
    e->depth = 0;
    e->focus_depth = ASMSPY_TF_NO_FOCUS;
    e->inv = 0;
    e->armed = 0;
    e->stopped = 0;
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

/* SEIZE + INTERRUPT a SINGLE thread `tid` (the --tid filter): trace exactly this
 * one task and leave the rest of its process running untraced (no TRACECLONE
 * following). Returns 0, or -1 if it cannot be seized (gone / not permitted). */
static int seize_one(pid_t tid, long opts, thr_tab_t *tab) {
    if (ptrace(PTRACE_SEIZE, tid, NULL, (void *)opts) != 0)
        return -1;
    ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
    if (!thr_get(tab, tid)) {
        ptrace(PTRACE_DETACH, tid, NULL,
               NULL); /* OOM: don't strand it seized */
        return -1;
    }
    return 0;
}

/* Attach for a single-step engine: one thread when `only_tid` is set, else the
 * whole process (all threads, following clones). Returns 0 or -1.
 *
 * PTRACE_O_TRACEEXEC is set on BOTH paths — a --tid-pinned thread that execs
 * replaces the image just as thoroughly as the leader doing it (and by then it
 * IS the leader; execve from any thread makes the caller the thread-group
 * leader). Without the option the exec-stop arrives as a bare SIGTRAP that the
 * step loop cannot tell from its own, so the engine would resume stepping brand
 * new code while still naming it from the OLD image's symbol table. */
static int seize_for_engine(pid_t pid, pid_t only_tid, int follow,
                            thr_tab_t *tab) {
    long opts = PTRACE_O_TRACEEXEC;
    if (only_tid) /* --tid: exactly this task; follow neither clones nor forks */
        return seize_one(only_tid, opts, tab);
    opts |= PTRACE_O_TRACECLONE;
    if (follow) /* --follow: child PROCESSES too (strace -f) */
        opts |= PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK;
    return seize_threads(pid, opts, tab);
}

/* Defined further down with the views that own them; needed here by the
 * per-process resolution helpers below. */
static void exe_basename(pid_t pid, char *out, size_t cap);
static void read_task_meta(pid_t tid, pid_t *tgid, pid_t *ppid, char *comm,
                           size_t cz);

/* The thread-group `tid` belongs to. Recorded when we see the clone/fork event
 * that created it (free, exact); looked up from /proc once and cached otherwise
 * (a task seized at attach, or one we met before its event). Falls back to
 * `dflt` — the original leader — which is right for every non-follow trace,
 * where by construction there is only one thread group. */
static pid_t thr_tgid(thr_tab_t *t, pid_t tid, pid_t dflt) {
    int i = thr_find(t, tid);
    if (i < 0)
        return dflt;
    if (t->v[i].tgid == 0) {
        pid_t g = 0, pp = 0;
        char comm[24];
        read_task_meta(tid, &g, &pp, comm, sizeof comm);
        t->v[i].tgid = g > 0 ? g : dflt;
    }
    return t->v[i].tgid;
}

/* ------------------------------------------------------------------ */
/* Per-PROCESS symbol resolution: execve + followed children           */
/*                                                                      */
/* Two separate things invalidate "one symtab for the whole trace":     */
/*                                                                      */
/*  1. execve() replaces an image wholesale — same pid, entirely new     */
/*     text, new load bias, new symbols. Every later resolution against  */
/*     the old table returns a CONFIDENTLY WRONG name. For a tracer that */
/*     is worse than no name: an address that resolves to "main" because */
/*     the old binary happened to have main there reads as fact. (Same   */
/*     reasoning that makes Theme A's separate-debug keys verified       */
/*     rather than trusted.) PTRACE_O_TRACEEXEC stops the tracee at      */
/*     exactly the right moment: the new image is mapped, but not one of */
/*     its instructions has retired yet.                                 */
/*                                                                      */
/*  2. --follow (PTRACE_O_TRACEFORK) brings in child PROCESSES. A cloned */
/*     THREAD shares its parent's address space and fd table, so one     */
/*     table and one `pid` to read through serve the whole thread group. */
/*     A FORKED CHILD shares NEITHER: new address space, new fd table,   */
/*     its own /proc/<pid>/maps. So everything the engines key off the   */
/*     leader `pid` — process_vm_readv, symbol resolution, fd->path, the */
/*     exe basename — is wrong for a followed child unless it is re-keyed */
/*     by that child's own thread-group id. Following forks is therefore  */
/*     not "one more ptrace option": it is a per-tgid re-keying.          */
/*                                                                      */
/* Both reduce to the same thing — resolve against the table for THIS    */
/* task's process, reloading it when that process's image changes.       */
/* ------------------------------------------------------------------ */

typedef struct {
    pid_t tgid;
    const asmspy_symtab_t *syms; /* what to resolve against for this process  */
    asmspy_symtab_t own;         /* our loaded table, when we own one         */
    int have_own;                /* `own` is loaded -> free it                */
    asmspy_jitmap_t jit;         /* this process's perf-map / jitdump         */
    char exebase[64];            /* its exe basename (the [int]/[EXT] split)  */
} psym_t;

typedef struct {
    psym_t *v;
    size_t n, cap;
    pid_t root;                   /* the pid the caller attached to        */
    const asmspy_symtab_t *rsyms; /* the caller's table, for `root` only   */
} psym_tab_t;

static void psym_init(psym_tab_t *pt, pid_t root,
                      const asmspy_symtab_t *rsyms) {
    pt->v = NULL;
    pt->n = pt->cap = 0;
    pt->root = root;
    pt->rsyms = rsyms;
}

/* Load (or re-load) one process's symbol table, JIT map and exe basename.
 * Best-effort throughout: a failure leaves an empty table, so the view falls
 * back to RAW ADDRESSES. That is the honest outcome — an unresolved "0x…" says
 * "I don't know", a stale name says something false. */
static void psym_load(psym_t *p, pid_t tgid, int reload) {
    asmspy_symtab_t fresh;
    asmspy_symtab_load(tgid, &fresh);
    if (p->have_own)
        asmspy_symtab_free(&p->own);
    p->own = fresh;
    p->have_own = 1;
    p->syms = &p->own;
    if (reload) {
        /* The perf-map / jitdump described the OLD image too. Re-init rather
         * than refresh: a refresh merges, and a stale JIT method surviving an
         * exec is the same confidently-wrong name in a different table. */
        asmspy_jitmap_free(&p->jit);
    }
    asmspy_jitmap_init(&p->jit, tgid);
    asmspy_jitmap_refresh(&p->jit);
    exe_basename(tgid, p->exebase, sizeof p->exebase);
}

/* The resolution context for `tgid`, created on first sight. The process the
 * caller attached to starts with the caller's pre-loaded table (no reload —
 * that table was read for exactly this image); any other process is one we
 * followed into, so we load its own. NULL only on allocation failure. */
static psym_t *psym_get(psym_tab_t *pt, pid_t tgid) {
    for (size_t i = 0; i < pt->n; i++)
        if (pt->v[i].tgid == tgid)
            return &pt->v[i];
    if (pt->n == pt->cap) {
        size_t nc = pt->cap ? pt->cap * 2 : 8;
        psym_t *nv = realloc(pt->v, nc * sizeof *nv);
        if (!nv)
            return NULL;
        pt->v = nv;
        pt->cap = nc;
    }
    psym_t *p = &pt->v[pt->n++];
    memset(p, 0, sizeof *p);
    p->tgid = tgid;
    if (tgid == pt->root) {
        p->syms = pt->rsyms; /* the caller's table — borrowed, never freed */
        p->have_own = 0;
        asmspy_jitmap_init(&p->jit, tgid);
        asmspy_jitmap_refresh(&p->jit); /* methods already compiled at attach */
        exe_basename(tgid, p->exebase, sizeof p->exebase);
    } else {
        psym_load(p, tgid, 0); /* a followed child: its own image entirely */
    }
    return p;
}

/* Re-resolve `tgid` after an execve replaced its image. */
static void psym_exec(psym_tab_t *pt, pid_t tgid) {
    psym_t *p = psym_get(pt, tgid);
    if (p)
        psym_load(p, tgid, 1);
}

static void psym_free(psym_tab_t *pt) {
    for (size_t i = 0; i < pt->n; i++) {
        if (pt->v[i].have_own)
            asmspy_symtab_free(&pt->v[i].own);
        asmspy_jitmap_free(&pt->v[i].jit);
    }
    free(pt->v);
    pt->v = NULL;
    pt->n = pt->cap = 0;
}

/* x86 EFLAGS Trap Flag: set by PTRACE_SINGLESTEP to fire a #DB after one insn. */
#define ASMSPY_EFLAGS_TF 0x100UL

/* Clear the Trap Flag on a stopped tracee. A single-step engine leaves each thread
 * resumed with PTRACE_SINGLESTEP (TF set); a thread that resumes still step-armed
 * takes a #DB -> SIGTRAP with no tracer to absorb it, which TERMINATES the tracee.
 * We write eflags back with TF clear UNCONDITIONALLY — not gated on the read-back TF
 * bit — because when PTRACE_SINGLESTEP armed the step the kernel sets an INTERNAL
 * forced TF and MASKS it out of PTRACE_GETREGS (eflags reads TF=0); a gated write
 * would skip exactly the still-armed threads. (This cancels a step not yet taken;
 * it does NOT undo a step already COMPLETED but not yet reported — see
 * drain_pending_step for that.) */
static void clear_trap_flag(pid_t tid) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0)
        return;
    regs.eflags &= ~ASMSPY_EFLAGS_TF;
    ptrace(PTRACE_SETREGS, tid, NULL, &regs);
}

/* True if `rip` points at a syscall-entry instruction (x86-64 `syscall`, or the
 * legacy `sysenter` / `int 0x80`). We must never single-step ACROSS one at teardown:
 * the step would only complete when the — possibly indefinitely blocking — syscall
 * returns. Fails safe (returns 1) if the bytes can't be read, so an unreadable rip
 * is treated as "don't step". */
static int at_syscall_insn(pid_t pid, uint64_t rip) {
    unsigned char b[2] = {0};
    struct iovec l = {b, 2}, r = {(void *)(uintptr_t)rip, 2};
    if (process_vm_readv(pid, &l, 1, &r, 1, 0) != 2)
        return 1;
    return (b[0] == 0x0f &&
            (b[1] == 0x05 || b[1] == 0x34)) || /* syscall/sysenter */
           (b[0] == 0xcd && b[1] == 0x80);     /* int 0x80 */
}

/* Drain a single-step trap the kernel has already QUEUED on a stopped thread but not
 * yet reported. When a step completes across a syscall, the resulting #DB is deferred
 * until the syscall returns; if the thread was parked in a blocking call (a JIT's
 * worker futex-waiting) the engine moved on and the trap is still pending at teardown.
 * Detach with it unconsumed and, when the call later returns, the #DB fires with no
 * tracer -> a fatal SIGTRAP that kills the whole process SECONDS after we left. So we
 * single-step once more: the queued trap is reported first (the thread does not
 * advance) and we swallow it, leaving the thread genuinely quiescent. A thread poised
 * ON a syscall instruction is skipped — stepping it would enter (and maybe block in)
 * the call — and clear_trap_flag alone disarms that not-yet-taken step. Because we
 * only ever step a NON-syscall instruction, the follow-up wait cannot hang. */
static void drain_pending_step(pid_t pid, pid_t tid) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0)
        return;
    if (at_syscall_insn(pid, regs.rip))
        return;
    if (ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL) != 0)
        return;
    int st;
    while (waitpid(tid, &st, __WALL) < 0 && errno == EINTR)
        ;
    /* Whatever it reported — the queued #DB, or the one guarded step — is consumed. */
}

/* Stop tracing and let the target run on normally. A SINGLE-STEP engine leaves the
 * target in a state that, released carelessly, kills a JIT / managed runtime
 * (V8/Node, JVM, …) with a fatal SIGTRAP seconds later, so when `single_stepped` is
 * set we clean up in two phases:
 *
 *   Phase 1 — stop + clear armed steps. INTERRUPT every thread, drain it to a
 *   ptrace-stop, and clear_trap_flag() so none resumes with a step still armed
 *   (that #DB would be fatal with no tracer to absorb it).
 *
 *   Phase 2 — drain deferred steps. A step that completed across a syscall leaves
 *   its #DB QUEUED until the syscall returns; a worker parked in a blocking call
 *   (futex) carries that pending trap right through detach, and it fires — fatally —
 *   when the call later returns. drain_pending_step() swallows it while we are still
 *   the tracer. (MEASURED on a live Node/V8 process: without this, ~1 in 2-6 detaches
 *   killed it with SIGTRAP a second or two after we left; the caught trap was a step
 *   #DB, si_code=TRAP_BRKPT, at the instruction just past a `syscall`.)
 *
 * A PTRACE_SYSCALL engine (the syscall log, procs in syscall-count mode) never arms a
 * step, so `single_stepped` is 0 and both phases are skipped — running them would
 * INJECT single-step state (an extra PTRACE_SINGLESTEP per thread) into a target that
 * had none, which is itself the fatal condition on the next re-attach. Then
 * PTRACE_DETACH each thread and free the table. */
static void detach_threads(pid_t pid, thr_tab_t *tab, int single_stepped) {
    for (size_t i = 0; i < tab->n;
         i++) { /* phase 1: interrupt + drain to a stop */
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
                if (single_stepped)
                    clear_trap_flag(
                        tid); /* drop any not-yet-taken armed step */
                break;        /* leave it STOPPED; drain + release below */
            }
        }
    }

    if (single_stepped)
        for (size_t i = 0; i < tab->n;
             i++) { /* phase 2: drain any QUEUED step #DB */
            /* Read the guard instruction through the task's OWN process: under
             * --follow the table spans several address spaces, and checking a
             * followed child's rip against the ORIGINAL leader's memory would
             * decide "don't step" (or worse, "do step") from unrelated bytes. */
            pid_t g = tab->v[i].tgid ? tab->v[i].tgid : pid;
            drain_pending_step(g, tab->v[i].tid);
            clear_trap_flag(
                tab->v[i].tid); /* the drain step re-armed TF; drop it */
        }

    for (size_t i = 0; i < tab->n; i++)
        ptrace(PTRACE_DETACH, tab->v[i].tid, NULL, NULL);

    free(tab->v);
    tab->v = NULL;
    tab->n = tab->cap = 0;
}

/* Give up on a tracee we could not TABLE (thr_get returned NULL — allocation
 * failure). It must NOT simply be resumed. detach_threads walks the TABLE, so an
 * untabled task is never detached; and in a single-step engine it was resumed
 * trap-flag-armed, so when we eventually go away it takes a #DB with no tracer
 * left to absorb it and DIES — seconds later, seemingly unrelated to the tool.
 * That is the fatal-SIGTRAP class the crash-safe two-phase detach exists to
 * prevent, and an untracked clone child is exactly how one escapes it.
 *
 * So disarm and detach it here instead. We lose sight of that task, which is the
 * honest outcome of running out of memory — the alternative is killing the very
 * process we are supposed to be watching out of band. This is the policy the
 * seize path already applies on OOM (seize_threads/seize_one detach rather than
 * strand a thread seized); the resume paths simply never did.
 *
 * `stepped` mirrors detach_threads: only a single-step engine can have armed TF,
 * and a PTRACE_SYSCALL engine must not have step state written into it. */
static void release_untracked(pid_t tid, int stepped) {
    if (stepped)
        clear_trap_flag(tid); /* drop a step armed before we lost the table */
    ptrace(PTRACE_DETACH, tid, NULL, NULL);
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

int asmspy_engine_syscalls(pid_t pid, int follow, long max, atomic_bool *stop,
                           asmspy_syscall_sink sink, void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    arm_quit_wake();

    thr_tab_t tab = {0};
    long opts = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE;
    if (follow) /* --follow: child PROCESSES too (strace -f) */
        opts |= PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK;
    if (seize_threads(pid, opts, &tab) != 0) {
        detach_threads(pid, &tab, 0); /* frees the (empty) table */
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
                thr_t *ct =
                    thr_get(&tab, (pid_t)child); /* fine if we saw it first */
                if (ct) /* CLONE joins this group; FORK/VFORK starts its own */
                    ct->tgid = (event == PTRACE_EVENT_CLONE)
                                   ? thr_tgid(&tab, tid, pid)
                                   : (pid_t)child;
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
                } else if (ts->ent_nr >=
                           0) { /* skip an exit we saw no entry for */
                    char line[1024], sdata[512], out[1088];
                    /* Decode via this task's OWN thread-group leader (shared mm
                     * + fd table). Threads of one process all share the target's,
                     * but a FOLLOWED CHILD has its own address space AND its own
                     * fd table — decoding it through the original leader would
                     * read the parent's strings and resolve the parent's fds,
                     * silently reporting the wrong path for the right syscall.
                     * The per-thread label is added below, not in the decoder. */
                    format_syscall(line, sizeof line, sdata, sizeof sdata,
                                   thr_tgid(&tab, tid, pid), ts->ent_nr,
                                   &ts->entry, (long)regs.rax);
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
        if (event == PTRACE_EVENT_STOP && (sig == SIGSTOP || sig == SIGTSTP ||
                                           sig == SIGTTIN || sig == SIGTTOU)) {
            if (!thr_get(&tab, tid)) { /* OOM: don't strand it traced */
                release_untracked(tid, 0);
                continue;
            }
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
        if (!thr_get(&tab, tid)) { /* OOM: detach, never resume untracked */
            release_untracked(tid, 0);
            continue;
        }
        ptrace(PTRACE_SYSCALL, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(pid, &tab, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Region-sample engine                                                 */
/*                                                                      */
/* Worker-thread targeting (asmspy-plan Theme B). This engine used to    */
/* PTRACE_ATTACH the LEADER and run_to() it, so a routine that runs on a */
/* worker thread — as managed methods almost always do — was never       */
/* observed and the engine returned ASMSPY_REGION_NEVER_RAN. It now      */
/* SEIZEs every thread, plants ONE shared int3 at the region entry, and  */
/* races them all: whichever thread arrives first is the one sampled.    */
/* This mirrors the landed, oracle-validated race in the data-flow tier  */
/* (src/dataflow_ptrace.c: dfp_seize_all + dfp_run_to_multi, Increment   */
/* 4) rather than inventing a second design; it is reimplemented here    */
/* over asmspy's own thr_tab, matching the precedent that an asmspy      */
/* engine stays in cli/ and leaves src/ptrace_backend.c untouched (as    */
/* asmspy_engine_watch does), because those producers' helpers are       */
/* static to a tier with no public header.                               */
/*                                                                      */
/* Two deliberate departures from the data-flow tier's one-shot capture: */
/*                                                                      */
/*  - PTRACE_O_TRACECLONE IS set, and the siblings stay SEIZED for the   */
/*    engine's lifetime. The data-flow tier captures ONE invocation and  */
/*    detaches, so it must not inherit future clone-stops; this engine   */
/*    re-races every round for `max` samples, so a thread spawned mid-run */
/*    should be able to win the race too. Siblings still run FREE while  */
/*    one thread is stepped: a ptrace stop is per-thread, so CONTinued    */
/*    siblings are only stopped if they themselves reach the entry.       */
/*                                                                      */
/*  - `only_tid` CANNOT narrow the SEIZE. The entry breakpoint is a      */
/*    shared int3 in the process's text, so an UNSEIZED thread reaching  */
/*    it would take a SIGTRAP with no tracer and DIE. only_tid is        */
/*    therefore a race FILTER (a non-target arrival is stepped over the  */
/*    bp and released, keeping it armed for the pinned tid), never a     */
/*    seize_one() — which is why this engine does not use               */
/*    seize_for_engine() the way the pure single-step engines do.        */
/* ------------------------------------------------------------------ */

/* Plant the shared entry int3 at `base` via any STOPPED thread of the target (one
 * address space). PTRACE_POKETEXT patches an r-x text page the way a debugger does.
 * The original word lands in *orig for the later restore. 0 on success, -1 on failure
 * (notably a W^X-enforced JIT page, which refuses POKETEXT with EIO — this engine has
 * no DR0 fallback yet, the same gap the data-flow tier carries, so such a target
 * self-skips cleanly rather than being traced wrong). */
static int rgn_plant_bp(pid_t tid, uint64_t base, long *orig) {
    errno = 0;
    long o = ptrace(PTRACE_PEEKTEXT, tid, (void *)(uintptr_t)base, NULL);
    if (o == -1 && errno != 0)
        return -1;
    long trap = (o & ~0xffL) | 0xccL;
    if (ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
               (void *)(uintptr_t)trap) != 0)
        return -1;
    *orig = o;
    return 0;
}

static void rgn_remove_bp(pid_t tid, uint64_t base, long orig) {
    ptrace(PTRACE_POKETEXT, tid, (void *)(uintptr_t)base,
           (void *)(uintptr_t)orig);
}

/* THE SAFETY NET. A thread trap-stopped just past the shared int3 has rip == base+1,
 * which is the MIDDLE of the region's first real instruction once the original byte is
 * restored — resuming it there executes garbage and would crash a process we do not own.
 * So every path that observes a stopped thread rewinds it here before letting it run,
 * and because every thread stays seized for the engine's lifetime, no such stop can be
 * lost: it can only be delivered later, and is rewound whenever it surfaces. Returns 1
 * if it rewound, 0 otherwise. */
static int rgn_rewind_from_bp(pid_t tid, uint64_t base) {
    struct user_regs_struct regs;
    if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) != 0)
        return 0;
    if (regs.rip != base + 1)
        return 0;
    regs.rip = base;
    return ptrace(PTRACE_SETREGS, tid, NULL, &regs) == 0;
}

/* DR0..DR3 + DR6/DR7 via struct user's u_debugreg[] — the same door the data-watchpoint
 * engine below opens, and the one src/ptrace_backend.c uses for EXECUTION breakpoints. */
#define RGN_DR_OFFSET(n)                                                       \
    (offsetof(struct user, u_debugreg) + (size_t)(n) * sizeof(long))

/* Arm a slot-0 EXECUTION breakpoint at `addr` on ONE stopped thread: DR0 = addr,
 * DR7 = 0x1 (L0 enable; R/W0 = 00 execute; LEN0 = 00) — the encoding ptrace_backend.c's
 * set_hw_bp already ships. Debug registers are PER-THREAD, which is the whole point
 * here: no other thread can trap on it. Returns 0, or -1 where PTRACE_POKEUSER is
 * refused (qemu-user exposes no slots; seccomp/permission can also refuse). */
static int rgn_hw_bp_arm(pid_t tid, uint64_t addr) {
    if (ptrace(PTRACE_POKEUSER, tid, (void *)RGN_DR_OFFSET(0),
               (void *)(uintptr_t)addr) != 0)
        return -1;
    if (ptrace(PTRACE_POKEUSER, tid, (void *)RGN_DR_OFFSET(7), (void *)0x1UL) !=
        0)
        return -1;
    return 0;
}

/* Disable all four slots (DR7 = 0) and clear the address + status. Idempotent. */
static void rgn_hw_bp_disarm(pid_t tid) {
    ptrace(PTRACE_POKEUSER, tid, (void *)RGN_DR_OFFSET(7), (void *)0UL);
    ptrace(PTRACE_POKEUSER, tid, (void *)RGN_DR_OFFSET(0), (void *)0UL);
    ptrace(PTRACE_POKEUSER, tid, (void *)RGN_DR_OFFSET(6), (void *)0UL);
}

/* Resume `tid` (forwarding `sig`, 0 for none) and record that it is no longer stopped.
 * Every resume in the race goes through here so the stopped-bookkeeping detach relies
 * on cannot drift out of step with reality. */
static void rgn_cont(thr_tab_t *tab, pid_t tid, int sig) {
    if (ptrace(PTRACE_CONT, tid, NULL, (void *)(uintptr_t)sig) == 0) {
        int i = thr_find(tab, tid);
        if (i >= 0)
            tab->v[i].stopped = 0;
    }
}

static void rgn_mark_stopped(thr_tab_t *tab, pid_t tid) {
    int i = thr_find(tab, tid);
    if (i >= 0)
        tab->v[i].stopped = 1;
}

/* Bring `tid` to a ptrace-stop so PTRACE_POKETEXT / PTRACE_POKEUSER will be accepted —
 * BOTH are refused on a running tracee, silently leaving the trap we planted armed.
 * Returns 0 if stopped on return, -1 if it vanished (then it is removed from `tab`). */
static int rgn_ensure_stopped(thr_tab_t *tab, pid_t tid) {
    int i = thr_find(tab, tid);
    if (i < 0)
        return -1;
    if (tab->v[i].stopped)
        return 0;
    ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
    for (;;) {
        int st = 0;
        pid_t r = waitpid(tid, &st, __WALL);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            thr_del(tab, tid);
            return -1;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            thr_del(tab, tid);
            return -1;
        }
        if (WIFSTOPPED(st)) {
            rgn_mark_stopped(tab, tid);
            return 0;
        }
    }
}

/* Remove whichever entry trap this round armed, from ANY exit path — including the
 * ones where every thread is running free (the idle timeout, a user quit).
 *
 * This is the single most safety-critical function in the engine. A trap left behind
 * on detach does not fail loudly: the target runs on and then, on its NEXT arrival at
 * the region, takes a SIGTRAP with no tracer attached and DIES (exit 133). The victim
 * outlives the tool that killed it by whole seconds, so the damage does not look like
 * ours. Both primitives are refused on a RUNNING thread, so each must be stopped first
 * — which is exactly the step it is tempting to skip, because skipping it still passes
 * a survives-immediately-after-detach check. */
static void rgn_disarm_entry(thr_tab_t *tab, int hw, pid_t only_tid,
                             uint64_t base, long orig) {
    if (hw) {
        if (rgn_ensure_stopped(tab, only_tid) == 0)
            rgn_hw_bp_disarm(only_tid);
        return;
    }
    /* The int3 is shared, so any thread we can stop can restore the byte — they share
     * one address space. Walk until one sticks; entries vanish as threads exit, so
     * only advance when the current one survived. */
    for (size_t i = 0; i < tab->n;) {
        pid_t tid = tab->v[i].tid;
        if (rgn_ensure_stopped(tab, tid) == 0) {
            rgn_remove_bp(tid, base, orig);
            return;
        }
        /* vanished: thr_del swapped a new tid into slot i — retry the same slot */
    }
}

/* If no thread reaches the entry within this much FREE-RUNNING time, call the region
 * idle rather than block a UI thread (and a headless --trace) forever. The pre-Theme-B
 * engine bounded this implicitly and crudely — a fast-failing run_to retried 21 times,
 * ~1s total — so an explicit, more generous bound is a strict improvement. Polled
 * (WNOHANG + a short nap) rather than signal-driven, to avoid interacting with the
 * SIGALRM/ITIMER watchdog the descent tracer arms inside trace_attached_ex. */
#define RGN_ENTRY_WAIT_MS 3000
#define RGN_POLL_US       200

/* Race the target's threads to the region entry, arming whichever primitive fits:
 *
 *   only_tid == 0 — a SHARED int3 at `base` planted through `planter` (any stopped
 *     tid; one address space). Every thread races and the FIRST to arrive is sampled:
 *     the Theme-B fix, and it needs no debug-register budget, so it works wherever
 *     ptrace does.
 *
 *   only_tid != 0 — a PER-THREAD hardware execution breakpoint on that tid alone.
 *     A shared int3 would be wrong here, not merely slower: every other thread would
 *     trap on it too and have to be single-stepped back over, which on a hot region
 *     is an unbounded storm that perturbs threads the user did not ask about (and,
 *     measured, does not converge). The hardware breakpoint is per-thread, so the
 *     non-target threads never trap at all, and it patches NO code — the target's
 *     bytes are untouched.
 *
 * On success *entering is the arriving thread, left trap-stopped exactly AT base with
 * the breakpoint disarmed and the other threads running free — the precondition
 * asmtest_ptrace_trace_attached_ex expects. Returns 0 on success, 1 if the user asked
 * to stop, 2 if the region stayed idle for RGN_ENTRY_WAIT_MS, -1 on failure/target
 * gone. Every non-zero return leaves the breakpoint removed. */
static int rgn_race_to_entry(thr_tab_t *tab, pid_t planter, uint64_t base,
                             pid_t only_tid, atomic_bool *stop,
                             pid_t *entering) {
    const int hw = (only_tid != 0);
    long orig = 0;

    if (hw) {
        if (rgn_hw_bp_arm(only_tid, base) != 0)
            return -1;
    } else if (rgn_plant_bp(planter, base, &orig) != 0) {
        return -1;
    }

    /* Release every thread we are holding into the race. A thread we already let run
     * (a previous round's sibling) is not stopped, so its CONT would just ESRCH —
     * skip it and keep the bookkeeping honest. */
    for (size_t i = 0; i < tab->n; i++) {
        if (!tab->v[i].stopped)
            continue;
        if (ptrace(PTRACE_CONT, tab->v[i].tid, NULL, NULL) == 0)
            tab->v[i].stopped = 0;
    }

    unsigned long idle_us = 0;
    for (;;) {
        if (stop && atomic_load(stop)) {
            rgn_disarm_entry(tab, hw, only_tid, base, orig);
            return 1;
        }
        int st = 0;
        pid_t w = waitpid(-1, &st, __WALL | WNOHANG);
        if (w == 0) { /* nothing ready: the target is running free */
            if (idle_us >= (unsigned long)RGN_ENTRY_WAIT_MS * 1000UL) {
                rgn_disarm_entry(tab, hw, only_tid, base, orig);
                return 2;
            }
            struct timespec nap = {0, RGN_POLL_US * 1000};
            nanosleep(&nap, NULL);
            idle_us += RGN_POLL_US;
            continue;
        }
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1; /* ECHILD: target gone, the bp died with it */
        }
        idle_us = 0; /* something happened: the target is alive and moving */

        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            thr_del(tab, w);
            if (tab->n == 0)
                return -1; /* target gone */
            if (hw && w == only_tid) {
                /* The pinned thread died: nothing can arrive. Report idle rather
                 * than poll out the full window. */
                return 2;
            }
            continue;
        }
        if (!WIFSTOPPED(st))
            continue;
        rgn_mark_stopped(
            tab, w); /* we consumed its stop; it is ours until resumed */

        int sig = WSTOPSIG(st);
        int event = (st >> 16) & 0xff;

        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, w, NULL, &child) == 0 && child)
                /* Table it so it can win a LATER round — a thread spawned mid-run is
                 * a legitimate region entrant, and it inherits no debug registers, so
                 * the pinned (hw) case correctly ignores it. Do NOT resume it here:
                 * its own attach-stop has not necessarily arrived yet. It surfaces in
                 * this same loop as a group-stop and is released there. */
                thr_get(tab, (pid_t)child);
            rgn_cont(tab, w, 0);
            continue;
        }

        if (sig == SIGTRAP && event == 0) {
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, w, NULL, &regs) != 0) {
                rgn_cont(tab, w, 0);
                continue;
            }
            /* OURS? Test this BEFORE sigtrap_is_app: a hardware breakpoint reports
             * si_code == TRAP_HWBKPT, which that helper (correctly, for its own
             * callers) reads as "the target's own". Ours is identified by the PC.
             * A hardware execution breakpoint is a FAULT — it stops AT base, before
             * the instruction runs — so, unlike the int3, there is nothing to rewind. */
            int mine = hw ? (w == only_tid && regs.rip == base)
                          : (regs.rip == base + 1);
            if (mine) {
                if (!thr_get(tab, w)) { /* OOM: don't strand it trap-stopped */
                    if (!hw)
                        rgn_rewind_from_bp(w, base);
                    rgn_cont(tab, w, 0);
                    continue;
                }
                /* `w` is stopped (we just consumed its trap), so it can restore the
                 * byte / clear its own debug registers directly. */
                if (hw)
                    rgn_hw_bp_disarm(w);
                else
                    rgn_remove_bp(w, base, orig);
                if (!hw && !rgn_rewind_from_bp(w, base))
                    return -1;
                *entering = w; /* stays marked stopped: the caller steps it */
                return 0;
            }
            /* Not our breakpoint. The target's OWN int3/hardware bp is delivered so
             * its signal machinery runs as it would untraced; anything else (a stray
             * ptrace-synthesized trap) is simply resumed. */
            if (sigtrap_is_app(w)) {
                deliver_app_sigtrap(w);
                int i = thr_find(tab, w);
                if (i >= 0)
                    tab->v[i].stopped = 0; /* deliver_app_sigtrap resumes it */
            } else {
                rgn_cont(tab, w, 0);
            }
            continue;
        }

        /* A group-stop (SIGSTOP family under SEIZE) resumes with 0; any other signal
         * is forwarded so the target handles its own. */
        int fwd = (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN ||
                   sig == SIGTTOU)
                      ? 0
                      : sig;
        rgn_cont(tab, w, fwd);
    }
}

/* Detach every tracked thread, leaving the target alive and running. Mirrors
 * detach_threads' interrupt-drain-detach shape but adds the two teardown hazards this
 * engine owns, which is why it does not simply call detach_threads():
 *
 *  1. The base+1 REWIND. A sibling that hit the shared int3 concurrently with its
 *     removal is still queued at base+1; detaching it there resumes it in the MIDDLE of
 *     the region's first instruction. Every stop we see here is rewound first.
 *  2. The debug-register DISARM. PTRACE_DETACH does NOT clear DR0/DR7 — a thread left
 *     armed would trap at `base` with no tracer attached, take a SIGTRAP nothing
 *     handles, and DIE. It is cheap and idempotent, so every thread is disarmed
 *     regardless of which primitive this run actually used.
 *
 * Both are the same failure in different clothes: leaving a live process holding a trap
 * we planted. The house rule is that a foreign target is never killed, and detach is
 * exactly where that rule is won or lost. */
static void rgn_detach_all(thr_tab_t *tab, uint64_t base) {
    for (size_t i = 0; i < tab->n; i++) {
        pid_t tid = tab->v[i].tid;
        /* Only interrupt a thread we believe is RUNNING. Interrupting one that is
         * already ptrace-stopped produces no new event, so the wait below would
         * block forever — the bookkeeping exists precisely to avoid that. */
        if (!tab->v[i].stopped) {
            ptrace(PTRACE_INTERRUPT, tid, NULL, NULL);
            for (;;) {
                int st = 0;
                pid_t r = waitpid(tid, &st, __WALL);
                if (r < 0) {
                    if (errno == EINTR)
                        continue;
                    break; /* ECHILD — already gone */
                }
                if (WIFEXITED(st) || WIFSIGNALED(st))
                    goto gone; /* nothing left to clean up or detach */
                if (WIFSTOPPED(st))
                    break;
            }
        }
        rgn_rewind_from_bp(tid, base);
        rgn_hw_bp_disarm(tid);
        ptrace(PTRACE_DETACH, tid, NULL, NULL);
    gone:;
    }
    free(tab->v);
    tab->v = NULL;
    tab->n = tab->cap = 0;
}

int asmspy_engine_region(pid_t pid, pid_t only_tid, uint64_t base, size_t len,
                         long max, atomic_bool *stop, asmspy_region_sink sink,
                         void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;
    if (len == 0 || len > (64u << 20))
        return ASMTEST_PTRACE_EINVAL;

    arm_quit_wake();

    /* SEIZE the WHOLE process, never seize_one(): see the only_tid note above. */
    thr_tab_t tab = {0};
    if (seize_threads(pid, PTRACE_O_TRACECLONE, &tab) != 0) {
        rgn_detach_all(&tab, base);
        return ASMTEST_PTRACE_ETRACE;
    }

    /* Drain each SEIZE/INTERRUPT stop so every thread is ptrace-stopped — the
     * precondition for planting the shared int3 and for the release CONT. Drop any
     * thread that vanished mid-seize. */
    for (size_t i = 0; i < tab.n;) {
        pid_t tid = tab.v[i].tid;
        int st = 0;
        pid_t w;
        do {
            w = waitpid(tid, &st, __WALL);
        } while (w < 0 && errno == EINTR);
        if (w != tid || WIFEXITED(st) || WIFSIGNALED(st)) {
            thr_del(&tab, tid);
            continue;
        }
        tab.v[i].stopped = 1;
        i++;
    }
    if (tab.n == 0) {
        rgn_detach_all(&tab, base);
        return ASMTEST_PTRACE_ETRACE;
    }

    /* region bytes for the disassembler (read once; static code) */
    uint8_t *code = malloc(len);
    if (code && rd(pid, base, code, len) != 0) {
        free(code);
        code = NULL;
    }

    /* Any stopped thread can POKETEXT the shared bp; after a sample the entrant is
     * left stopped at the region exit, so it plants the next round. */
    pid_t planter = tab.v[0].tid;
    unsigned sample = 0;
    int idle = 0; /* consecutive rounds that produced NO sample */
    while ((max < 0 || (long)sample < max) && !(stop && atomic_load(stop))) {
        pid_t entrant = 0;
        int rc =
            rgn_race_to_entry(&tab, planter, base, only_tid, stop, &entrant);
        /* rc: 0 = entrant caught; 1 = user quit; 2 = region idle for the whole
         * wait window; -1 = target gone / entry unarmable. Only 0 continues. A 2
         * ends sampling rather than retrying: the round already waited longer than
         * the pre-Theme-B engine's entire give-up budget, so retrying would only
         * delay an honest "it isn't running" (and, with samples already taken, the
         * region has simply gone quiet). */
        if (rc != 0)
            break;

        asmtest_trace_t *tr = asmtest_trace_new(8192, 512);
        asmtest_descent_t *dsc =
            asmtest_descent_new(ASMTEST_DESCENT_RECORD_EDGES);
        long result = 0;
        /* Step the ENTERING thread, not the leader — the whole point of the race. */
        int trc = asmtest_ptrace_trace_attached_ex(
            entrant, (void *)(uintptr_t)base, len, &result, tr, dsc);
        int produced = (trc == ASMTEST_PTRACE_OK && tr &&
                        asmtest_emu_trace_insns_total(tr) >= 1);
        if (produced) {
            sample++;
            idle = 0;
            if (sink)
                sink(ctx, sample, result, tr, dsc, code, len, base);
        }
        asmtest_trace_free(tr);
        asmtest_descent_free(dsc);

        /* The entrant is left stopped past the region: it plants the next round. */
        planter = entrant;

        if (!produced) {
            /* The race reached the entry but the trace failed/was empty; count it
             * toward the idle bailout so a bad region can't spin the loop forever. */
            if (stop && atomic_load(stop))
                break;
            if (trc == ASMTEST_PTRACE_ENOENT)
                break;
            if (++idle > 20)
                break;
        }
    }

    free(code);
    rgn_detach_all(&tab, base);
    /* Detached cleanly but never saw the region run (and the user didn't quit).
     * Since the race now covers every thread, this no longer means "it ran on a
     * worker" — it means the region genuinely did not execute in the sample window. */
    if (sample == 0 && !(stop && atomic_load(stop)))
        return ASMSPY_REGION_NEVER_RAN;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Scoped data-flow value capture (Increments 6 + 5)                   */
/*                                                                     */
/* Wraps the landed JIT-aware scoped-ptrace L0 value producer          */
/* (src/dataflow_ptrace.c: asmtest_dataflow_ptrace_attach_jit) — SEIZE  */
/* EVERY thread of a live pid, race whichever one first enters          */
/* [base,base+len) (so a routine running on a worker thread, as         */
/* managed methods almost always do, is reached — not just the          */
/* leader), single-step it capturing each step's operand VALUES with    */
/* the signal split (the target's OWN trap is detected and DELIVERED,   */
/* never swallowed or single-stepped through), DETACH so the target     */
/* survives — then builds the pure L1 last-writer def-use graph over    */
/* the L0 trace and hands both to the sink. The producer ships NO       */
/* public header (a value-trace producer is a tier, not the shared      */
/* asmtest_valtrace.h sink API), so — exactly as its test suite         */
/* examples/test_dataflow_ptrace.c does — its entry point + return      */
/* codes are re-declared here. `img`/`when` (versioned decode / method   */
/* attribution) are not wired in yet — a target patched/re-JIT'd MID-   */
/* capture decodes the live snapshot, same as the native tier; a         */
/* genuinely W^X-enforced JIT page refusing the POKETEXT entry           */
/* breakpoint (no hardware-breakpoint fallback here yet, unlike the      */
/* offset-only tracer) self-skips cleanly via DF_PTRACE_ETRACE rather    */
/* than crashing anything — both are known, separately-tracked gaps,     */
/* not silent partial captures.                                         */
/* ------------------------------------------------------------------ */

/* The scoped ptrace producers' return codes — declared here, NOT in a public
 * header, kept in step with src/dataflow_ptrace.c and its test suite. */
#define DF_PTRACE_OK     0    /* clean, complete scoped trace                 */
#define DF_PTRACE_FAULT  1    /* region faulted; a partial trace is filled    */
#define DF_PTRACE_EINVAL (-1) /* bad arguments                                */
#define DF_PTRACE_ENOSYS (-3) /* off Linux x86-64 / no Capstone: self-skip    */
#define DF_PTRACE_ETRACE (-4) /* SEIZE/ptrace/wait failure (seccomp/yama)     */

int asmtest_dataflow_ptrace_attach_jit(pid_t pid, pid_t only_tid, uint64_t base,
                                       size_t code_len,
                                       asmtest_codeimage_t *img, uint64_t when,
                                       uint64_t max_insns, long *result,
                                       int *survived, asmtest_valtrace_t *vt);

int asmspy_engine_dataflow(pid_t pid, pid_t only_tid, uint64_t base, size_t len,
                           long max, atomic_bool *stop,
                           asmspy_dataflow_sink sink, void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    if (len == 0 || len > (64u << 20))
        return ASMTEST_PTRACE_EINVAL;
    if (stop && atomic_load(stop))
        return 0;

    /* Region bytes for the sink's disassembler (best-effort, static native code,
     * read out of band before we attach). The producer reads its OWN copy from the
     * target for the operand enumerator; this snapshot is purely render-side. */
    uint8_t *code = malloc(len);
    if (code && rd(pid, base, code, len) != 0) {
        free(code);
        code = NULL;
    }

    /* Caller-owned L0 buffers. Bound the step buffer by `max` when given, else a
     * generous default; recs/wide scale with steps x operands-per-step. Overflow is
     * honest (vt->truncated), so a bigger region simply truncates rather than lying.
     * Cap the allocation so a huge --max can't ask for gigabytes up front. */
    size_t steps_cap = (max > 0) ? (size_t)max : 16384;
    if (steps_cap < 64)
        steps_cap = 64;
    if (steps_cap > 65536)
        steps_cap =
            65536; /* bound the one-shot allocation (~33 MB worst case) */
    asmtest_valtrace_t *vt =
        asmtest_valtrace_new(steps_cap, steps_cap * 8, steps_cap * 16);
    if (!vt) {
        free(code);
        return ASMTEST_PTRACE_EUNAVAIL; /* OOM: nothing captured */
    }

    long result = 0;
    int survived = 0; /* not yet surfaced to the caller; see asmspy.h note */
    /* SEIZE every thread of `pid` and step whichever one first enters the region —
     * so a routine that runs on a worker thread (as managed methods almost always
     * do) is reached, not just the leader. `only_tid` (non-0, the --tid convention)
     * pins exactly that thread instead of racing all of them. */
    uint64_t max_insns = (max > 0) ? (uint64_t)max : 0;
    int prc = asmtest_dataflow_ptrace_attach_jit(
        pid, only_tid, base, len, NULL, 0, max_insns, &result, &survived, vt);

    int rc;
    switch (prc) {
    case DF_PTRACE_OK:
    case DF_PTRACE_FAULT: {
        /* Captured fully, or a partial prefix on a fault (vt->truncated tells the
         * story). Build the last-writer def-use graph and surface both. */
        asmtest_defuse_t *g = asmtest_defuse_build(vt);
        if (sink)
            sink(ctx, result, vt, g, code, len, base);
        asmtest_defuse_free(g);
        rc = 0;
        break;
    }
    case DF_PTRACE_ENOSYS:
        rc =
            ASMSPY_DATAFLOW_UNAVAIL; /* off-platform / no Capstone: clean skip */
        break;
    case DF_PTRACE_EINVAL:
        rc = ASMTEST_PTRACE_EINVAL;
        break;
    case DF_PTRACE_ETRACE:
    default:
        rc =
            ASMTEST_PTRACE_ETRACE; /* SEIZE/permission failure (yama/seccomp) */
        break;
    }

    asmtest_valtrace_free(vt);
    free(code);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Whole-process live instruction stream                               */
/* ------------------------------------------------------------------ */

int asmspy_engine_stream(pid_t pid, pid_t only_tid, int follow, long max,
                         atomic_bool *stop, const asmspy_symtab_t *syms,
                         asmspy_stream_sink sink, void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    /* Whole-process single-stepping (all threads, interleaved), or — when
     * only_tid is set — just that one thread while the rest run at full speed.
     * No TRACESYSGOOD — every stop here is a step. */
    thr_tab_t tab = {0};
    if (seize_for_engine(pid, only_tid, follow, &tab) != 0) {
        detach_threads(pid, &tab, 1);
        return ASMTEST_PTRACE_ETRACE;
    }

    int multi = tab.n > 1;
    /* Per-process resolution: re-read on execve, and give a followed child its
     * own table (it shares neither the address space nor the symbols). */
    psym_tab_t pt;
    psym_init(&pt, pid, syms);
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
                thr_t *ct = thr_get(&tab, (pid_t)child);
                if (ct)
                    /* A CLONE joins this task's thread group; a FORK/VFORK is a
                     * new process whose tgid is the child pid itself. Taken
                     * from the event: free and exact. */
                    ct->tgid = (event == PTRACE_EVENT_CLONE)
                                   ? thr_tgid(&tab, tid, pid)
                                   : (pid_t)child;
                multi = 1;
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        /* execve() replaced the image: RE-RESOLVE before stepping a single
         * instruction of the new text, or every line below names new code from
         * the old binary's symbol table. Post-exec the caller IS the leader, so
         * its tid names the thread group whose image just changed. */
        if (event == PTRACE_EVENT_EXEC) {
            thr_t *ts = thr_get(&tab, tid);
            if (ts)
                ts->tgid = tid;
            psym_exec(&pt, tid);
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (sig == SIGTRAP &&
            event == 0) { /* our step, or the app's own trap */
            if (sigtrap_is_app(
                    tid)) { /* target's own int3/hw bp — deliver it */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                uint64_t rip = regs.rip;
                /* This task's OWN process: with --follow the tracee set spans
                 * several address spaces, and reading/naming through the
                 * original leader would silently describe the wrong one. */
                pid_t tgid = thr_tgid(&tab, tid, pid);
                psym_t *ps = psym_get(&pt, tgid);

                /* disassemble the instruction about to retire */
                uint8_t code[16];
                char dis[160] = "";
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
                /* read via the thread-group LEADER: all threads share the
                 * address space, and process_vm_readv on a non-leader tid can be
                 * refused, which would blank the disassembly */
                ssize_t got = process_vm_readv(tgid, &liov, 1, &riov, 1, 0);
                if (got >= 1 && asmtest_disas_available())
                    asmtest_disas(ASMTEST_ARCH_X86_64, code, (size_t)got, rip,
                                  0, dis, sizeof dis);

                /* resolve the function this address lands in (ELF, else JIT) */
                char loc[96];
                const asmspy_sym_t *s =
                    ps ? asmspy_resolve(ps->syms, &ps->jit, rip) : NULL;
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
        if (event == PTRACE_EVENT_STOP && (sig == SIGSTOP || sig == SIGTSTP ||
                                           sig == SIGTTIN || sig == SIGTTOU)) {
            if (!thr_get(&tab, tid)) { /* OOM: don't strand it traced */
                release_untracked(tid, 1);
                continue;
            }
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
        if (!thr_get(&tab, tid)) { /* OOM: detach, never resume untracked */
            release_untracked(tid, 1);
            continue;
        }
        ptrace(PTRACE_SINGLESTEP, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(pid, &tab, 1);
    psym_free(&pt);
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
                      asmspy_jitmap_t *jit, const char *exebase,
                      uint64_t addr) {
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
        e->external = !is_jit && (is_plt || (exebase && exebase[0] &&
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
        if (g->edge[i].caller == (size_t)ci &&
            g->edge[i].callee == (size_t)ki) {
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

/* Hand the current graph to `sink`, converting the internal index-based edges to
 * the public address-keyed asmspy_gedge_t (so the sink may sort/filter nodes
 * without invalidating edges). `ae`/`aecap` is a caller-owned scratch buffer,
 * grown as needed and reused across snapshots. On OOM the sink still gets the
 * nodes (edges NULL/0). */
static void graph_emit(asmspy_graph_sink sink, void *ctx, const graph_t *g,
                       asmspy_gedge_t **ae, size_t *aecap) {
    if (!sink)
        return;
    if (g->ne > *aecap) {
        asmspy_gedge_t *nv = realloc(*ae, g->ne * sizeof **ae);
        if (!nv) {
            sink(ctx, g->node, g->nn, NULL, 0);
            return;
        }
        *ae = nv;
        *aecap = g->ne;
    }
    for (size_t i = 0; i < g->ne; i++) {
        (*ae)[i].caller_addr = g->node[g->edge[i].caller].addr;
        (*ae)[i].callee_addr = g->node[g->edge[i].callee].addr;
        (*ae)[i].count = g->edge[i].count;
    }
    sink(ctx, g->node, g->nn, *ae, g->ne);
}

int asmspy_engine_graph(pid_t pid, pid_t only_tid, int follow, long max,
                        atomic_bool *stop, const asmspy_symtab_t *syms,
                        asmspy_graph_sink sink, void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    thr_tab_t tab = {0};
    if (seize_for_engine(pid, only_tid, follow, &tab) != 0) {
        detach_threads(pid, &tab, 1);
        return ASMTEST_PTRACE_ETRACE;
    }

    graph_t g = {0};
    asmspy_gedge_t *aedges =
        NULL; /* scratch: address-keyed edges for the sink */
    size_t aecap = 0;
    /* Per-process resolution: symtab, JIT map and exe basename are all
     * per-image, so an execve or a followed child needs its own. */
    psym_tab_t pt;
    psym_init(&pt, pid, syms);
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
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child) {
                thr_t *ct = thr_get(&tab, (pid_t)child);
                if (ct) /* CLONE joins this thread group; FORK starts its own */
                    ct->tgid = (event == PTRACE_EVENT_CLONE)
                                   ? thr_tgid(&tab, tid, pid)
                                   : (pid_t)child;
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        /* execve() replaced the image: re-resolve BOTH the symbol table and the
         * exe basename before a single instruction of the new text retires. The
         * basename drives the [int]/[EXT] split, so a stale one silently
         * re-labels the new binary's own functions as external. */
        if (event == PTRACE_EVENT_EXEC) {
            thr_t *ts = thr_get(&tab, tid);
            if (ts)
                ts->tgid = tid; /* post-exec the caller IS the leader */
            psym_exec(&pt, tid);
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (sig == SIGTRAP &&
            event == 0) { /* our step, or the app's own trap */
            if (sigtrap_is_app(
                    tid)) { /* target's own int3/hw bp — deliver it */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                uint64_t rip = regs.rip;
                thr_t *ts = thr_get(&tab, tid);
                /* this task's own process (see the stream engine's note) */
                pid_t tgid = thr_tgid(&tab, tid, pid);
                psym_t *ps = psym_get(&pt, tgid);
                if (!ps) { /* OOM: keep stepping rather than mis-name */
                    ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
                    continue;
                }

                /* read the bytes of the instruction about to retire */
                uint8_t code[16];
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
                ssize_t got = process_vm_readv(tgid, &liov, 1, &riov, 1, 0);

                /* consume a pending INDIRECT call: this rip is its callee entry */
                if (ts && ts->pending_call) {
                    recorded += grecord(&g, ps->syms, &ps->jit, ps->exebase,
                                        ts->call_site, rip);
                    ts->pending_call = 0;
                }

                if (got >= 1 && asmtest_disas_available() &&
                    asmtest_disas_is_call(ASMTEST_ARCH_X86_64, code,
                                          (size_t)got, 0)) {
                    uint64_t tgt = 0;
                    if (asmtest_disas_call_target(ASMTEST_ARCH_X86_64, code,
                                                  (size_t)got, rip, 0, &tgt)) {
                        /* DIRECT call: target known now — record immediately */
                        recorded += grecord(&g, ps->syms, &ps->jit, ps->exebase,
                                            rip, tgt);
                    } else if (ts) {
                        /* INDIRECT call: resolve the callee at the next step */
                        ts->pending_call = 1;
                        ts->call_site = rip;
                    }
                }

                /* throttle live snapshots (single-step is slow; 16 calls/pub) */
                if (sink && recorded - published >= 16) {
                    graph_emit(sink, ctx, &g, &aedges, &aecap);
                    published = recorded;
                }
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (event == PTRACE_EVENT_STOP && (sig == SIGSTOP || sig == SIGTSTP ||
                                           sig == SIGTTIN || sig == SIGTTOU)) {
            if (!thr_get(&tab, tid)) { /* OOM: don't strand it traced */
                release_untracked(tid, 1);
                continue;
            }
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        int deliver = 0;
        if (event == 0 && sig != SIGTRAP)
            deliver = sig;
        if (!thr_get(&tab, tid)) { /* OOM: detach, never resume untracked */
            release_untracked(tid, 1);
            continue;
        }
        ptrace(PTRACE_SINGLESTEP, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(pid, &tab, 1);
    if (sink) /* always hand over a final snapshot */
        graph_emit(sink, ctx, &g, &aedges, &aecap);
    free(g.node);
    free(g.edge);
    free(aedges);
    psym_free(&pt);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Whole-process live call TREE                                        */
/* ------------------------------------------------------------------ */

/* Emit one "entered a function" line, indented by the caller's live depth, plus
 * the structured entry (tid/depth/addr/name/module) so a front-end can
 * disassemble the callee or export the tree without re-parsing the text.
 *
 * `filt`/`focus_depth` apply the view's output filter (depth cap / symbol focus
 * / module — asmspy_treefilter.h). Returns 1 if a line was emitted, 0 if the
 * filter suppressed it: the CALLER still pushes the depth either way, because
 * the shadow counter tracks the target's real control flow, not the filtered
 * view of it. Resolution happens here (before the filter) because focus and
 * module both match on the RESOLVED name/module, not the raw address. */
static int tree_emit(asmspy_tree_sink sink, void *ctx, int multi, pid_t tid,
                     const asmspy_symtab_t *syms, asmspy_jitmap_t *jit,
                     uint64_t callee, int depth,
                     const asmspy_tree_filter_t *filt, int *focus_depth) {
    if (!sink)
        return 0;
    const asmspy_sym_t *s = asmspy_resolve(syms, jit, callee);
    char raw[24];
    const char *nm, *mod;
    if (s) {
        nm = s->name;
        mod = s->module;
    } else {
        snprintf(raw, sizeof raw, "0x%llx", (unsigned long long)callee);
        nm = raw;
        mod = "?";
    }

    /* Match on what the view SHOWS: an unresolved frame is "0x…"/"?", so
     * --focus=0x7f… can still pin a raw JIT address and --module='?' can single
     * out the unresolved frames, while --module=libc correctly skips them. */
    int eff = depth;
    if (!asmspy_tree_filter_call(filt, nm, mod, depth, focus_depth, &eff))
        return 0;

    char name[200];
    if (s)
        snprintf(name, sizeof name, "%s [%s]", nm, mod);
    else
        snprintf(name, sizeof name, "%s", raw);
    int ind = eff * 2;
    if (ind > 60) /* clamp runaway / drifted indentation to a sane width */
        ind = 60;
    char line[320];
    if (multi)
        snprintf(line, sizeof line, "[%d] %*s-> %s", (int)tid, ind, "", name);
    else
        snprintf(line, sizeof line, "%*s-> %s", ind, "", name);
    asmspy_tree_call_t call = {tid, eff, callee, nm, mod};
    sink(ctx, line, &call);
    return 1;
}

int asmspy_engine_tree(pid_t pid, pid_t only_tid, int follow, long max,
                       atomic_bool *stop, const asmspy_symtab_t *syms,
                       const asmspy_tree_filter_t *filter,
                       asmspy_tree_sink sink, void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    if (!asmtest_ptrace_available())
        return ASMTEST_PTRACE_EUNAVAIL;

    arm_quit_wake();

    thr_tab_t tab = {0};
    if (seize_for_engine(pid, only_tid, follow, &tab) != 0) {
        detach_threads(pid, &tab, 1);
        return ASMTEST_PTRACE_ETRACE;
    }

    int multi = tab.n > 1;
    /* Per-process resolution (execve + followed children) */
    psym_tab_t pt;
    psym_init(&pt, pid, syms);
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
                thr_t *ct = thr_get(&tab, (pid_t)child);
                if (ct) { /* CLONE joins this group; FORK starts its own */
                    ct->tgid = (event == PTRACE_EVENT_CLONE)
                                   ? thr_tgid(&tab, tid, pid)
                                   : (pid_t)child;
                    /* A forked child resumes INSIDE fork() in a copy of the
                     * parent's stack, so it inherits the parent's live call
                     * depth — start it there rather than at 0, or its first
                     * returns would underflow to a clamped 0 and its whole tree
                     * would render flat at the top level. */
                    if (event != PTRACE_EVENT_CLONE) {
                        int pi = thr_find(&tab, tid);
                        if (pi >= 0)
                            ct->depth = tab.v[pi].depth;
                    }
                }
                multi = 1;
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        /* execve() replaced the image: re-resolve before naming any of the new
         * text from the old binary's table. The per-thread call depths are also
         * meaningless across an exec (the old stack is gone), so reset them. */
        if (event == PTRACE_EVENT_EXEC) {
            thr_t *ts = thr_get(&tab, tid);
            if (ts) {
                ts->tgid = tid; /* post-exec the caller IS the leader */
                ts->depth = 0;
                ts->pending_call = 0;
                ts->focus_depth = ASMSPY_TF_NO_FOCUS;
            }
            psym_exec(&pt, tid);
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (sig == SIGTRAP &&
            event == 0) { /* our step, or the app's own trap */
            if (sigtrap_is_app(
                    tid)) { /* target's own int3/hw bp — deliver it */
                deliver_app_sigtrap(tid);
                continue;
            }
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0) {
                uint64_t rip = regs.rip;
                thr_t *ts = thr_get(&tab, tid);
                /* this task's own process (see the stream engine's note) */
                pid_t tgid = thr_tgid(&tab, tid, pid);
                psym_t *ps = psym_get(&pt, tgid);
                if (!ps) { /* OOM: keep stepping rather than mis-name */
                    ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
                    continue;
                }

                uint8_t code[16];
                struct iovec liov = {code, sizeof code};
                struct iovec riov = {(void *)(uintptr_t)rip, sizeof code};
                ssize_t got = process_vm_readv(tgid, &liov, 1, &riov, 1, 0);

                /* a pending INDIRECT call resolves at its callee entry (= rip).
                 * `emitted` counts SURVIVING lines, but the depth push is
                 * unconditional: the shadow counter tracks the target's real
                 * control flow, never the filtered view of it. */
                if (ts && ts->pending_call) {
                    emitted +=
                        tree_emit(sink, ctx, multi, tid, ps->syms, &ps->jit,
                                  rip, ts->depth, filter, &ts->focus_depth);
                    ts->depth++;
                    ts->pending_call = 0;
                }

                int isc = 0, isr = 0;
                if (got >= 1 && asmtest_disas_available())
                    asmtest_disas_probe(ASMTEST_ARCH_X86_64, code, (size_t)got,
                                        0, &isc, &isr);
                if (isc && ts) {
                    uint64_t tgt = 0;
                    if (asmtest_disas_call_target(ASMTEST_ARCH_X86_64, code,
                                                  (size_t)got, rip, 0, &tgt)) {
                        emitted +=
                            tree_emit(sink, ctx, multi, tid, ps->syms, &ps->jit,
                                      tgt, ts->depth, filter, &ts->focus_depth);
                        ts->depth++;
                    } else {
                        ts->pending_call = 1; /* indirect: resolve next step */
                        ts->call_site = rip;
                    }
                } else if (isr && ts) {
                    if (ts->depth > 0)
                        ts->depth--; /* returned: pop a level (clamped at 0) */
                    /* left the --focus subtree? (unconditional, so a drifted
                     * counter resting at 0 still releases a depth-0 focus) */
                    asmspy_tree_filter_ret(filter, ts->depth, &ts->focus_depth);
                }
            }
            ptrace(PTRACE_SINGLESTEP, tid, NULL, NULL);
            continue;
        }

        if (event == PTRACE_EVENT_STOP && (sig == SIGSTOP || sig == SIGTSTP ||
                                           sig == SIGTTIN || sig == SIGTTOU)) {
            if (!thr_get(&tab, tid)) { /* OOM: don't strand it traced */
                release_untracked(tid, 1);
                continue;
            }
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        int deliver = 0;
        if (event == 0 && sig != SIGTRAP)
            deliver = sig;
        if (!thr_get(&tab, tid)) { /* OOM: detach, never resume untracked */
            release_untracked(tid, 1);
            continue;
        }
        ptrace(PTRACE_SINGLESTEP, tid, NULL, (void *)(long)deliver);
    }

    detach_threads(pid, &tab, 1);
    psym_free(&pt);
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
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

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
        detach_threads(pid, &tab, (mode == ASMSPY_COUNT_CALLS));
        return ASMTEST_PTRACE_ETRACE;
    }

/* resume a tracee the way this mode drives it: syscall-stops vs single-steps */
#define TOPO_RESUME(t, s)                                                      \
    ptrace(mode == ASMSPY_COUNT_SYSCALLS ? PTRACE_SYSCALL : PTRACE_SINGLESTEP, \
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
                    asmtest_disas_is_call(ASMTEST_ARCH_X86_64, code,
                                          (size_t)got, 0)) {
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

        if (event == PTRACE_EVENT_STOP && (sig == SIGSTOP || sig == SIGTSTP ||
                                           sig == SIGTTIN || sig == SIGTTOU)) {
            if (!thr_get(&tab, tid)) { /* OOM: don't strand it traced */
                release_untracked(tid, (mode == ASMSPY_COUNT_CALLS));
                continue;
            }
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        int deliver = 0;
        if (event == 0 && sig != SIGTRAP && sig != (SIGTRAP | 0x80))
            deliver = sig;
        if (!thr_get(&tab, tid)) { /* OOM: detach, never resume untracked */
            release_untracked(tid, (mode == ASMSPY_COUNT_CALLS));
            continue;
        }
        TOPO_RESUME(tid, deliver);
    }

#undef TOPO_RESUME
    if (sink) /* final snapshot BEFORE detach frees the table */
        topo_publish(&tab, sink, ctx, &snap, &snapcap);
    detach_threads(pid, &tab, (mode == ASMSPY_COUNT_CALLS));
    free(snap);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Statistical hot-edge sampler (AMD IBS-Op, out of band)              */
/*                                                                     */
/* Unlike every other engine above, this one NEVER attaches ptrace and */
/* never single-steps: asmtest_ibs_survey_process samples retired taken */
/* branches through the perf ring while the target runs full speed, so */
/* it is the safe rich view on a JIT / managed runtime. It only maps    */
/* the sampled addresses to names — the heavy lifting is in the IBS     */
/* backend (src/ibs_backend.c).                                         */
/* ------------------------------------------------------------------ */

/* Resolve one sampled address to "func+0xNN [module]" (ELF symtab, then JIT
 * perf-map), or "0x…" if nothing names it. Mirrors gnode_get's naming so the
 * sample view and the call-graph view label the same address identically. */
static void sample_name(const asmspy_symtab_t *syms, asmspy_jitmap_t *jit,
                        uint64_t addr, char *out, size_t cap) {
    const asmspy_sym_t *s = asmspy_resolve(syms, jit, addr);
    if (!s) {
        snprintf(out, cap, "0x%llx", (unsigned long long)addr);
        return;
    }
    uint64_t off = addr >= s->addr ? addr - s->addr : 0;
    if (off)
        snprintf(out, cap, "%s+0x%llx [%s]", s->name, (unsigned long long)off,
                 s->module);
    else
        snprintf(out, cap, "%s [%s]", s->name, s->module);
}

int asmspy_engine_sample(pid_t pid, unsigned ms, atomic_bool *stop,
                         const asmspy_symtab_t *syms, asmspy_jitmap_t *jit,
                         asmspy_sample_sink sink, void *ctx) {
    if (!asmtest_ibs_available())
        return ASMSPY_SAMPLE_UNAVAIL;
    if (ms == 0)
        ms = 300;

    asmspy_sample_edge_t *res = NULL; /* reused resolved-edge scratch */
    size_t rescap = 0;
    int hard_fail = 0, any = 0;

    for (;;) {
        if (stop && atomic_load(stop))
            break;

        asmtest_ibs_survey_t sv;
        int rc = asmtest_ibs_survey_process(pid, ms, NULL, &sv);
        if (rc == ASMTEST_IBS_EUNAVAIL) {
            hard_fail = 1;
            break;
        }
        if (rc !=
            ASMTEST_IBS_OK) { /* transient (e.g. target vanished); retry/stop */
            asmtest_ibs_survey_free(&sv);
            if (!stop)
                break;
            continue;
        }
        any = 1;

        if (sv.n >
            rescap) { /* grow the resolved-edge buffer to fit this window */
            asmspy_sample_edge_t *nv = realloc(res, sv.n * sizeof *nv);
            if (nv) {
                res = nv;
                rescap = sv.n;
            }
        }
        size_t n =
            sv.n <= rescap ? sv.n : rescap; /* on OOM: as many as we can */
        for (size_t i = 0; i < n; i++) {
            const asmtest_ibs_edge_t *e = &sv.edges[i];
            res[i].from_addr = e->from;
            res[i].to_addr = e->to;
            res[i].count = e->count;
            res[i].mispred = e->mispred;
            res[i].is_return = e->is_return;
            /* resolve endpoints — copy the name out before the next resolve, whose
             * return pointer may be invalidated by a refresh-on-miss */
            sample_name(syms, jit, e->from, res[i].from, sizeof res[i].from);
            sample_name(syms, jit, e->to, res[i].to, sizeof res[i].to);
        }
        if (sink)
            sink(ctx, res, n, sv.samples, sv.branch_samples, sv.lost,
                 sv.throttled);
        asmtest_ibs_survey_free(&sv);

        if (!stop) /* headless: exactly one window */
            break;
    }

    free(res);
    if (hard_fail && !any)
        return ASMSPY_SAMPLE_UNAVAIL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hardware DATA-watchpoint engine                                     */
/*                                                                     */
/* Arms an x86 debug register (DR0, slot 0) in WRITE / READ-WRITE mode  */
/* on a chosen address in EVERY thread of the target, PTRACE_CONTs it,  */
/* and at each #DB reports who touched the field + the value. NO single- */
/* step, NO code patch — the target runs at native speed between hits.  */
/* The DR7/DR6 encoding is the one examples/watchpoint_spike.c proved    */
/* (F3 spike, GO); ptrace_backend.c is NOT touched — this reimplements   */
/* the DATA-mode DR7 fields (the backend ships only DR7=0x1 EXECUTE bps).*/
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)

/* DR0..DR3 + DR6/DR7 are reached through struct user's u_debugreg[] via
 * PTRACE_POKEUSER/PEEKUSER — the SAME door src/ptrace_backend.c opens for
 * EXECUTION breakpoints (where it writes DR7 = 0x1). A data watchpoint differs
 * only in DR7's per-slot R/W + LEN fields. asmspy uses slot 0 (it watches one
 * field). */
#define WATCH_DR_OFFSET(n)                                                     \
    (offsetof(struct user, u_debugreg) + (size_t)(n) * sizeof(long))

/* DR7 slot-0 word: L0 enable | R/W0 | LEN0. R/W0 = 01 (write, self-labelling) or
 * 11 (read+write); LEN0 encodes 1/2/4/8 bytes as 00/01/11/10 — NOT monotonic:
 * 8 bytes is 10b, 4 bytes is 11b (validated by the spike). */
static unsigned long watch_dr7_word(int rw_rdwr, int len) {
    unsigned long rw = rw_rdwr ? 0x3UL : 0x1UL;
    unsigned long lenf;
    switch (len) {
    case 1:
        lenf = 0x0UL;
        break;
    case 2:
        lenf = 0x1UL;
        break;
    case 4:
        lenf = 0x3UL;
        break;
    default:
        lenf = 0x2UL; /* 8 bytes */
        break;
    }
    return 1UL | (rw << 16) | (lenf << 18); /* L0 | R/W0 | LEN0 */
}

/* Arm the slot-0 data watchpoint (DR0 = addr, DR7 = word) on one stopped thread.
 * Returns 0, or -1 if PTRACE_POKEUSER is refused (permission / seccomp / no real
 * debug registers). */
static int watch_arm(pid_t tid, uint64_t addr, unsigned long dr7) {
    errno = 0;
    if (ptrace(PTRACE_POKEUSER, tid, (void *)WATCH_DR_OFFSET(0),
               (void *)(uintptr_t)addr) != 0)
        return -1;
    errno = 0;
    if (ptrace(PTRACE_POKEUSER, tid, (void *)WATCH_DR_OFFSET(7), (void *)dr7) !=
        0)
        return -1;
    return 0;
}

/* Disarm every debug slot on one stopped thread (DR7 = 0 disables all four) and
 * clear DR0 + the DR6 status — so after detach the thread never takes a stray #DB
 * (which, with no tracer to absorb it, would be a fatal SIGTRAP). */
static void watch_disarm(pid_t tid) {
    ptrace(PTRACE_POKEUSER, tid, (void *)WATCH_DR_OFFSET(7), (void *)0UL);
    ptrace(PTRACE_POKEUSER, tid, (void *)WATCH_DR_OFFSET(0), (void *)0UL);
    ptrace(PTRACE_POKEUSER, tid, (void *)WATCH_DR_OFFSET(6), (void *)0UL);
}

/* DR6's low nibble (B0..B3) says which slot tripped; nonzero => a data watchpoint
 * we armed fired (as opposed to an app int3 / other SIGTRAP). */
static unsigned long watch_dr6(pid_t tid) {
    errno = 0;
    long v = ptrace(PTRACE_PEEKUSER, tid, (void *)WATCH_DR_OFFSET(6), NULL);
    return (v == -1 && errno != 0) ? 0UL : (unsigned long)v;
}

/* The #DB is delivered AFTER the accessing instruction retires, so RIP is the
 * NEXT instruction and the access instruction is the one ENDING at RIP. Read a
 * 15-byte window (max x86 insn length) before RIP, disassemble forward, and find
 * the instruction whose end == RIP; classify its memory operand as write (in the
 * write-set) or read (in the read-set). Returns 1 write, 0 read, -1 unknown, and
 * sets *access_pc to the access instruction's address when found. Uses the same
 * Capstone operand enumerator asmspy's --dataflow path links (dataflow_operands). */
static int watch_decode_dir(pid_t pid, uint64_t rip, uint64_t *access_pc) {
    if (!asmtest_operands_available() || rip < 15)
        return -1;
    enum { WIN = 15 };
    uint8_t buf[WIN];
    uint64_t start = rip - WIN;
    struct iovec l = {buf, WIN};
    struct iovec r = {(void *)(uintptr_t)start, WIN};
    if (process_vm_readv(pid, &l, 1, &r, 1, 0) != (ssize_t)WIN)
        return -1;
    for (size_t off = 0; off < WIN; off++) {
        at_val_rec_t reads[24], writes[24];
        size_t nr = 24, nw = 24;
        size_t ilen = asmtest_operands(ASMTEST_ARCH_X86_64, buf, WIN,
                                       (uint64_t)off, reads, &nr, writes, &nw);
        if (ilen == 0 || start + off + ilen != rip)
            continue; /* not the instruction ending exactly at RIP */
        int wrote_mem = 0, read_mem = 0;
        for (size_t i = 0; i < nw; i++)
            if (writes[i].kind != AT_LOC_REG)
                wrote_mem = 1;
        for (size_t i = 0; i < nr; i++)
            if (reads[i].kind != AT_LOC_REG)
                read_mem = 1;
        if (access_pc)
            *access_pc = start + off;
        return wrote_mem ? 1 : (read_mem ? 0 : -1);
    }
    return -1;
}

/* Interrupt every thread, DISARM its debug registers, then DETACH — the teardown
 * that lets the watched target SURVIVE (a thread released with a slot still armed
 * would take a #DB with no tracer -> a fatal SIGTRAP). The watch engine never
 * single-steps, so detach_threads' single-step drain is not needed; disarming the
 * debug registers is this engine's equivalent crash-safety step. */
static void watch_teardown(thr_tab_t *tab) {
    for (size_t i = 0; i < tab->n; i++) { /* phase 1: interrupt + disarm */
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
                break;
            if (WIFSTOPPED(st)) {
                watch_disarm(tid);
                break; /* leave it stopped; released below */
            }
        }
    }
    for (size_t i = 0; i < tab->n; i++) /* phase 2: release */
        ptrace(PTRACE_DETACH, tab->v[i].tid, NULL, NULL);
    free(tab->v);
    tab->v = NULL;
    tab->n = tab->cap = 0;
}

int asmspy_engine_watch(pid_t pid, uint64_t addr, int rw, int len, long max,
                        atomic_bool *stop, const asmspy_symtab_t *syms,
                        asmspy_watch_sink sink, void *ctx) {
    /* Refuse a 32-bit tracee BEFORE attaching: every register read and syscall
     * decode below assumes the x86-64 ABI, so on an i386 task this engine does
     * not fail — it reports confident nonsense. (asmspy.h: ASMSPY_ETRACEE_I386) */
    if (asmspy_elf_class(pid) == 32)
        return ASMSPY_ETRACEE_I386;

    if (len != 1 && len != 2 && len != 4 && len != 8)
        return ASMTEST_PTRACE_EINVAL;
    if (addr &
        (uint64_t)(len - 1)) /* x86 requires a length-aligned watch addr */
        return ASMTEST_PTRACE_EINVAL;

    arm_quit_wake();

    /* SEIZE every thread (debug registers are per-thread, so we arm them all) and
     * follow threads spawned later via PTRACE_O_TRACECLONE. */
    thr_tab_t tab = {0};
    if (seize_threads(pid, PTRACE_O_TRACECLONE, &tab) != 0) {
        detach_threads(pid, &tab, 0); /* frees the (empty) table */
        return ASMTEST_PTRACE_ETRACE;
    }

    unsigned long dr7 = watch_dr7_word(rw, len);
    asmspy_jitmap_t
        jit; /* name JIT / managed-runtime frames from the perf-map */
    asmspy_jitmap_init(&jit, pid);
    asmspy_jitmap_refresh(&jit);

    int any_armed = 0;   /* at least one thread's DR was successfully armed  */
    int arm_refused = 0; /* the FIRST arm failed -> host refuses (skip)      */
    unsigned long hits = 0;

    while ((max < 0 || (long)hits < max) && !(stop && atomic_load(stop))) {
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

        /* A newly-spawned thread: table it (armed on its own first stop, below),
         * then resume the parent. Debug registers are per-thread and NOT inherited
         * across clone, so every new thread must be armed explicitly. */
        if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
            event == PTRACE_EVENT_VFORK) {
            unsigned long child = 0;
            if (ptrace(PTRACE_GETEVENTMSG, tid, NULL, &child) == 0 && child)
                thr_get(&tab, (pid_t)child);
            ptrace(PTRACE_CONT, tid, NULL, NULL);
            continue;
        }

        thr_t *ts = thr_get(&tab, tid);

        /* Our data watchpoint fired? DR6's low nibble names the slot. A data #DB
         * reports SIGTRAP with si_code TRAP_HWBKPT, so we key on DR6 (which is
         * OURS) — NOT sigtrap_is_app, which treats every hw breakpoint as the
         * app's. */
        if (sig == SIGTRAP && ts && ts->armed) {
            if (watch_dr6(tid) & 0xfUL) {
                struct user_regs_struct regs;
                uint64_t rip = 0;
                if (ptrace(PTRACE_GETREGS, tid, NULL, &regs) == 0)
                    rip = (uint64_t)regs.rip;

                /* the value: the watched bytes AFTER the access (read via the
                 * leader `pid` — process_vm_readv on a non-leader tid can be
                 * refused). */
                uint64_t value = 0;
                int value_ok =
                    (rip != 0) && rd(pid, addr, &value, (size_t)len) == 0;

                /* who: the access instruction (one back from RIP) resolved to
                 * function+offset. A write-only watch is self-labelling (every
                 * hit is a store); for --rw decode the faulting instruction. */
                uint64_t pc = rip, access_pc = 0;
                int dir = watch_decode_dir(pid, rip, &access_pc);
                if (access_pc)
                    pc = access_pc;
                const asmspy_sym_t *s = asmspy_resolve(syms, &jit, pc);

                hits++;
                if (sink) {
                    asmspy_watch_hit_t h;
                    h.hit_no = hits;
                    h.tid = tid;
                    h.pc = pc;
                    h.addr = addr;
                    h.is_write = rw ? dir : 1;
                    h.value_ok = value_ok;
                    h.value_len = (unsigned)len;
                    h.value = value;
                    h.func = s ? s->name : NULL;
                    h.module = s ? s->module : NULL;
                    h.off = s ? (pc - s->addr) : 0;
                    sink(ctx, &h);
                }

                /* clear DR6 (best-effort) so the next hit is unambiguous, then
                 * resume — DR7 stays armed. */
                ptrace(PTRACE_POKEUSER, tid, (void *)WATCH_DR_OFFSET(6),
                       (void *)0UL);
                ptrace(PTRACE_CONT, tid, NULL, (void *)0);
                continue;
            }
            /* a SIGTRAP that is NOT our watchpoint (the target's own int3 / hw bp):
             * deliver it so the target handles its own breakpoint, else swallow. */
            if (sigtrap_is_app(tid))
                deliver_app_sigtrap(tid);
            else
                ptrace(PTRACE_CONT, tid, NULL, (void *)0);
            continue;
        }

        /* First stop for this thread (the SEIZE/INTERRUPT stop, or a clone child's
         * initial stop): ARM its debug registers, then CONT. */
        if (ts && !ts->armed) {
            if (watch_arm(tid, addr, dr7) == 0) {
                any_armed = 1;
            } else if (!any_armed) {
                /* the host refuses debug-register arming (qemu zero slots /
                 * seccomp / permission) — a clean, whole-op skip. */
                arm_refused = 1;
                ptrace(PTRACE_CONT, tid, NULL, (void *)0);
                break;
            }
            ts->armed = 1;
            ptrace(PTRACE_CONT, tid, NULL, (void *)0);
            continue;
        }

        /* A job-control group-stop (^Z / SIGSTOP / tty) under SEIZE: PTRACE_LISTEN
         * so the target stays suspended (honoring ^Z) instead of being resumed. */
        if (event == PTRACE_EVENT_STOP && (sig == SIGSTOP || sig == SIGTSTP ||
                                           sig == SIGTTIN || sig == SIGTTOU)) {
            ptrace(PTRACE_LISTEN, tid, NULL, NULL);
            continue;
        }

        /* Otherwise a real signal-delivery-stop: forward the signal and resume. */
        int deliver = (event == 0 && sig != SIGTRAP) ? sig : 0;
        ptrace(PTRACE_CONT, tid, NULL, (void *)(long)deliver);
    }

    watch_teardown(&tab);
    asmspy_jitmap_free(&jit);
    (void)any_armed;
    return arm_refused ? ASMSPY_WATCH_UNAVAIL : 0;
}

#else /* !__x86_64__ — no DR0..DR3/DR7 reachable via PTRACE_POKEUSER */

int asmspy_engine_watch(pid_t pid, uint64_t addr, int rw, int len, long max,
                        atomic_bool *stop, const asmspy_symtab_t *syms,
                        asmspy_watch_sink sink, void *ctx) {
    (void)pid;
    (void)addr;
    (void)rw;
    (void)len;
    (void)max;
    (void)stop;
    (void)syms;
    (void)sink;
    (void)ctx;
    return ASMSPY_WATCH_UNAVAIL; /* x86 debug registers only */
}

#endif /* __x86_64__ */
