/* syscall_victim.c — a separate process (asm-test does NOT start it) that does
 * real filesystem + stdout I/O in a loop, so the out-of-band syscall logger
 * (examples/syscall_log.c) has actual DATA to capture: openat paths, write
 * buffers, and read buffers crossing the kernel boundary — plus a bare path
 * probe (access) whose only interesting argument IS the path.
 *
 * Opts into tracing by any same-uid process via PR_SET_PTRACER_ANY, so the demo
 * runs under a plain `docker run` even at Yama ptrace_scope=1 (see
 * examples/attach_victim.c for the permission-model note).
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    fprintf(stderr, "victim pid=%d\n", (int)getpid());
    fflush(stderr);

    const char *path = "/tmp/asmtest_syscall_demo.txt";
    for (int n = 0;; n++) {
        char msg[64];
        int len = snprintf(msg, sizeof msg, "tick %d from pid %d\n", n,
                           (int)getpid());

        access(path, F_OK); /* path-only syscall; glibc may route it via *at */

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, msg, (size_t)len); /* outgoing data */
            close(fd);
        }
        fd = open(path, O_RDONLY);
        if (fd >= 0) {
            char in[64];
            ssize_t got = read(fd, in, sizeof in); /* incoming data */
            (void)got;
            close(fd);
        }
        usleep(300 * 1000); /* ~3 Hz */
    }
    return 0;
}
