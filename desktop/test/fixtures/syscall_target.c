/* serve_record_target.c — the target for test_serve_record and for the 61 T7c
 * crossings fixture. `work` is called every iteration so a tracer that ATTACHES
 * mid-run still sees entries to it, and the syscalls it makes fall into more
 * than one SyscallClass family — openat/write/close are File, getpid is
 * Process, clock_nanosleep is Time — so the crossing class channel has
 * something to distinguish rather than one colour repeated. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

__attribute__((noinline)) int work(int i) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
        write(fd, "x", 1);
        close(fd);
    }
    (void)getpid();
    return i + 1;
}

int main(void) {
    int n = 0;
    int i;
    for (i = 0; i < 3000; i++) {
        struct timespec ts = {0, 5000000};
        n = work(n);
        nanosleep(&ts, 0);
    }
    return n & 1;
}
