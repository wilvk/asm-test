/*
 * argdecode_victim.c — makes a fixed set of syscalls with KNOWN arguments, so
 * the syscall arg-decoding smoke can assert the rendered text exactly rather
 * than "something plausible appeared" (asmspy-plan Theme E).
 *
 * Every call here is chosen because its rendering is a claim that can be WRONG
 * in a specific way:
 *
 *   openat  O_WRONLY|O_CREAT|O_TRUNC + 0644  — a flag word AND a conditional
 *           arity: mode is only an argument because O_CREAT is set. A second
 *           open (O_RDONLY) must render with NO mode slot at all.
 *   mmap    PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS — 6 args, two flag
 *           words. The old decoder showed three hex slots and stopped.
 *   mprotect PROT_READ — proves a single-bit prot renders, and PROT_NONE has a
 *           name rather than being an empty string.
 *   writev  two iovecs with known text — a VECTOR: the bytes are the datum, the
 *           array's address is not.
 *   nanosleep {0, 2000000} — a struct read out of the target, not a pointer.
 *   tgkill  SIGUSR1 — a signal NUMBER that must render as a name.
 *   rt_sigprocmask SIG_BLOCK, {SIGUSR2} — a `how` enum AND a sigset BITMASK.
 *   getpid  — arity ZERO: the shape the always-3-hex default could not express.
 *   lseek   SEEK_END — a whence enum.
 *
 * The self-pipe/tmp file keep this hermetic: no network, no reliance on what
 * libc happens to do at startup.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#define TMP "/tmp/asmspy_argdecode.txt"

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    printf("ready\n");
    fflush(stdout);

    /* Let the tracer attach before any of the calls it must decode. */
    sleep(1);

    for (;;) {
        /* openat WITH a creating flag: mode is a real argument */
        int fd = open(TMP, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            struct iovec iov[2];
            iov[0].iov_base = (void *)"iovec-one";
            iov[0].iov_len = 9;
            iov[1].iov_base = (void *)"iovec-two";
            iov[1].iov_len = 9;
            writev(fd, iov, 2);
            lseek(fd, 0, SEEK_END);
            close(fd);
        }
        /* openat WITHOUT one: mode must NOT be rendered */
        int rf = open(TMP, O_RDONLY);
        if (rf >= 0)
            close(rf);

        /* two flag words and a 6-arg shape */
        void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED) {
            mprotect(m, 4096, PROT_READ);
            munmap(m, 4096);
        }

        /* a sigset bitmask + a how enum */
        sigset_t s, old;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR2);
        sigprocmask(SIG_BLOCK, &s, &old);
        sigprocmask(SIG_SETMASK, &old, NULL);

        /* a signal number, sent to a thread that ignores it */
        signal(SIGUSR1, SIG_IGN);
        syscall(SYS_tgkill, getpid(), getpid(), SIGUSR1);

        /* arity zero */
        getpid();

        /* a struct read out of the target */
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}
