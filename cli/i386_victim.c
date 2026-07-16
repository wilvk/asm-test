/* i386_victim.c — a 32-bit tracee for the EI_CLASS refusal smoke
 * (asmspy-plan Theme F3). Built with -m32 (see mk/cli.mk).
 *
 * asmspy's engines are x86-64-only, and on an i386 task that is not a build
 * error but a SILENTLY WRONG ANSWER: the kernel reports this process's syscall
 * number in orig_rax against the i386 table, which asmspy would decode against
 * the x86-64 one. The numbers overlap and disagree — i386 4 is write, x86-64 4
 * is stat — so a --log here would name every syscall wrong and look perfectly
 * plausible doing it.
 *
 * So this victim just does obvious, nameable syscalls in a loop (write to
 * /dev/null, nanosleep) and exists to be REFUSED. If the refusal ever regresses,
 * the smoke's mutation evidence shows what comes out instead.
 *
 * Opts in via PR_SET_PTRACER_ANY like the other victims, so a refusal can never
 * be mistaken for a permission failure.
 */
#include <fcntl.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

__attribute__((noinline)) long i386_fn(long x) { return x * 29 + 11; }

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "i386_victim pid=%d (%zu-bit pointers)\n", (int)getpid(),
            sizeof(void *) * 8);
    fflush(stderr);
    int fd = open("/dev/null", O_WRONLY);
    volatile long sink = 0;
    for (long i = 0;; i++) {
        sink += i386_fn(i);
        if (fd >= 0)
            write(fd, "x", 1); /* i386 SYS_write(4) == x86-64 SYS_stat(4) */
        usleep(20 * 1000);
    }
    return 0;
}
