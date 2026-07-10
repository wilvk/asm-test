/* syscall_log.c — the DATA-logging sibling of examples/attach_trace.c.
 *
 * Where attach_trace records which CODE ran (control flow), this records what a
 * process SENDS and RECEIVES through syscalls — the buffers and paths crossing the
 * kernel boundary — out of band, using the SAME ptrace attach seam. It is a
 * minimal strace: PTRACE_SYSCALL to stop on every syscall entry/exit, PTRACE_GETREGS
 * for the number + arguments, and process_vm_readv to copy the pointed-to DATA.
 *
 * Note it links only libc — NOT the hwtrace tier. Data logging needs the ptrace
 * seam, not asm-test's instruction decoder; the attach demo just showed the seam is
 * reusable. x86-64 Linux (the syscall-arg registers are the SysV syscall ABI; an
 * AArch64 port reads x8=nr / x0..x5=args via PTRACE_GETREGSET/NT_PRSTATUS).
 *
 * Usage:  syscall_log <pid> [max_syscalls]     (default 24, then DETACH)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#define DUMP_CAP 48 /* bytes of a buffer/path to show */

/* Print up to `n` bytes at target address `addr` as a C-escaped quoted string. */
static void dump_data(pid_t pid, uint64_t addr, size_t n) {
    size_t want = n > DUMP_CAP ? DUMP_CAP : n;
    unsigned char buf[DUMP_CAP];
    struct iovec local = {buf, want};
    struct iovec remote = {(void *)(uintptr_t)addr, want};
    ssize_t got = want ? process_vm_readv(pid, &local, 1, &remote, 1, 0) : 0;
    if (got < 0)
        got = 0;
    putchar('"');
    for (ssize_t i = 0; i < got; i++) {
        unsigned char c = buf[i];
        if (c == '\n')
            fputs("\\n", stdout);
        else if (c == '\t')
            fputs("\\t", stdout);
        else if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c >= 0x20 && c < 0x7f)
            putchar((int)c);
        else
            printf("\\x%02x", c);
    }
    putchar('"');
    if (n > (size_t)got)
        fputs("...", stdout); /* truncated */
}

/* Read a NUL-terminated string (a path) at `addr`, quoted. */
static void dump_cstr(pid_t pid, uint64_t addr) {
    char buf[DUMP_CAP + 1];
    struct iovec local = {buf, DUMP_CAP};
    struct iovec remote = {(void *)(uintptr_t)addr, DUMP_CAP};
    ssize_t got = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (got < 0)
        got = 0;
    buf[got] = '\0';
    printf("\"%.*s\"", (int)got, buf);
}

static void print_dirfd(long long raw) {
    int dirfd = (int)raw; /* dirfd is a 32-bit int; -100 arrives zero-extended */
    if (dirfd == AT_FDCWD)
        fputs("AT_FDCWD", stdout);
    else
        printf("%d", dirfd);
}

/* Name a few syscalls the demo's fallback path hits, else "#<nr>". */
static const char *syscall_name(long nr) {
    switch (nr) {
    case SYS_getpid:
        return "getpid";
    case SYS_nanosleep:
        return "nanosleep";
    case SYS_clock_nanosleep:
        return "clock_nanosleep";
    case SYS_restart_syscall:
        return "restart_syscall";
    case SYS_lseek:
        return "lseek";
    default:
        return NULL;
    }
}

/* Decode the syscall at ENTRY: name + args, and the OUTGOING data (write/openat). */
static void print_entry(pid_t pid, const struct user_regs_struct *r) {
    long nr = (long)r->orig_rax;
    switch (nr) {
    case SYS_write:
        printf("write(fd=%lld, ", r->rdi);
        dump_data(pid, r->rsi, (size_t)r->rdx);
        printf(", %lld)", r->rdx);
        break;
    case SYS_read:
        printf("read(fd=%lld, %lld)", r->rdi, r->rdx);
        break;
    case SYS_openat:
        fputs("openat(", stdout);
        print_dirfd((long)r->rdi);
        fputs(", ", stdout);
        dump_cstr(pid, r->rsi);
        printf(", flags=0x%llx)", r->rdx);
        break;
    case SYS_close:
        printf("close(fd=%lld)", r->rdi);
        break;
    default: {
        const char *nm = syscall_name(nr);
        if (nm)
            printf("%s(0x%llx, 0x%llx, 0x%llx)", nm, r->rdi, r->rsi, r->rdx);
        else
            printf("syscall#%ld(0x%llx, 0x%llx, 0x%llx)", nr, r->rdi, r->rsi,
                   r->rdx);
        break;
    }
    }
}

/* Decode the EXIT: return value, and the INCOMING data (read fills its buffer). */
static void print_exit(pid_t pid, long nr, uint64_t read_buf,
                       const struct user_regs_struct *r) {
    long ret = (long)r->rax;
    if (nr == SYS_read && ret > 0) {
        fputs(" = ", stdout);
        dump_data(pid, read_buf, (size_t)ret);
        printf(" [%ld bytes]\n", ret);
    } else {
        printf(" = %ld\n", ret);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <pid> [max_syscalls]\n", argv[0]);
        return 2;
    }
    pid_t pid = (pid_t)strtol(argv[1], NULL, 0);
    long max_syscalls = argc > 2 ? strtol(argv[2], NULL, 0) : 24;
    setvbuf(stdout, NULL, _IOLBF, 0); /* flush each line in chronological order */

    /* ATTACH — same seam as the control-flow demo. */
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0) {
        perror("PTRACE_ATTACH (needs same-uid + ptrace_scope=0, CAP_SYS_PTRACE, "
               "or the target's PR_SET_PTRACER opt-in)");
        return 1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    /* Mark syscall-stops distinctly (WSTOPSIG == SIGTRAP|0x80). */
    ptrace(PTRACE_SETOPTIONS, pid, NULL, (void *)PTRACE_O_TRACESYSGOOD);

    fprintf(stderr, "logging up to %ld syscalls of pid %d (out of band)\n\n",
            max_syscalls, (int)pid);

    int at_entry = 1;   /* the next syscall-stop is a syscall ENTRY */
    long ent_nr = -1;   /* syscall number captured at entry           */
    uint64_t rbuf = 0;  /* read()'s buffer ptr, carried entry->exit    */
    int deliver = 0;    /* signal to re-inject on the next resume      */
    long done = 0;

    while (done < max_syscalls) {
        if (ptrace(PTRACE_SYSCALL, pid, NULL, (void *)(long)deliver) != 0)
            break;
        deliver = 0;
        if (waitpid(pid, &status, 0) < 0)
            break;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            fprintf(stderr, "\n(target exited)\n");
            break;
        }
        if (!WIFSTOPPED(status))
            continue;

        int sig = WSTOPSIG(status);
        if (sig == (SIGTRAP | 0x80)) { /* a syscall-stop */
            struct user_regs_struct regs;
            if (ptrace(PTRACE_GETREGS, pid, NULL, &regs) != 0)
                break;
            if (at_entry) {
                print_entry(pid, &regs);
                ent_nr = (long)regs.orig_rax;
                rbuf = regs.rsi; /* read(fd, buf=rsi, count) */
                at_entry = 0;
            } else {
                print_exit(pid, ent_nr, rbuf, &regs);
                at_entry = 1;
                done++;
            }
        } else if (sig != SIGTRAP) {
            deliver = sig; /* forward a real signal to the target */
        }
    }

    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    fprintf(stderr, "\ndetached; logged %ld syscalls\n", done);
    return 0;
}
