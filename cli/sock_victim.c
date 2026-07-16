/* sock_victim.c — a victim with real sockets, for the fd->ENDPOINT smoke
 * (asmspy-plan Theme E).
 *
 * readlink("/proc/<pid>/fd/N") on a socket says "socket:[12345]" — an inode,
 * i.e. the one thing a person watching a trace does not care about. asmspy must
 * turn that into the endpoint via /proc/<pid>/net.
 *
 * Sets up, on loopback so nothing leaves the machine and no port is assumed:
 *   - a LISTENing TCP socket (bound to port 0, so the kernel picks a free one)
 *   - an accepted/connected TCP pair, written to in a loop
 *   - an AF_UNIX socket bound to a filesystem path, also written to
 *
 * so the smoke can assert all three shapes: a listener, a connected peer pair,
 * and a named unix path. The TCP pair is both ends of one connection inside this
 * process, which makes the expected endpoints exactly derivable from the port.
 *
 * Opts in via PR_SET_PTRACER_ANY like the other victims.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#define UNIX_PATH "/tmp/asmspy_sock_victim.sock"

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    /* write() to a LISTENING TCP socket returns EPIPE *and raises SIGPIPE*
     * (tcp_sendmsg -> sk_stream_error), which killed this victim outright
     * (exit 141) before this line existed. Real servers ignore SIGPIPE for the
     * same reason; the failing write below is deliberate. */
    signal(SIGPIPE, SIG_IGN);

    /* --- TCP: listener on an ephemeral loopback port, then connect to it --- */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; /* kernel picks a free port — no fixed port to collide */
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 ||
        listen(lfd, 4) != 0) {
        perror("tcp listen");
        return 1;
    }
    socklen_t al = sizeof a;
    getsockname(lfd, (struct sockaddr *)&a, &al);
    unsigned port = ntohs(a.sin_port);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0 || connect(cfd, (struct sockaddr *)&a, sizeof a) != 0) {
        perror("tcp connect");
        return 1;
    }
    int sfd = accept(lfd, NULL, NULL);

    /* --- AF_UNIX bound to a real path --- */
    unlink(UNIX_PATH);
    int ufd = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un u;
    memset(&u, 0, sizeof u);
    u.sun_family = AF_UNIX;
    snprintf(u.sun_path, sizeof u.sun_path, "%s", UNIX_PATH);
    if (ufd < 0 || bind(ufd, (struct sockaddr *)&u, sizeof u) != 0) {
        perror("unix bind");
        return 1;
    }
    /* connect a DGRAM socket to its OWN address so plain write() works on it.
     * sendto() would be the natural call, but asmspy only hand-decodes a small
     * set of syscalls and sendto is not one of them — it would render as raw hex
     * with no fd to enrich, testing nothing. write() is decoded. */
    if (connect(ufd, (struct sockaddr *)&u, sizeof u) != 0) {
        perror("unix connect");
        return 1;
    }

    /* the smoke needs the port to derive the expected endpoint strings */
    fprintf(stderr, "sock_victim pid=%d tcp_port=%u unix=%s\n", (int)getpid(),
            port, UNIX_PATH);
    fflush(stderr);

    for (;;) { /* keep touching every socket so --log has syscalls to decode */
        write(cfd, "c", 1); /* connected TCP, client end */
        if (sfd >= 0)
            write(sfd, "s", 1); /* connected TCP, server end */
        write(ufd, "u", 1);     /* AF_UNIX bound to a path */
        /* Deliberately write to the LISTENING socket. It fails with ENOTCONN,
         * which is fine and is the point: the syscall is still decoded, so the
         * smoke gets a line naming a LISTEN endpoint — otherwise no decoded
         * syscall ever mentions the listener's fd. */
        write(lfd, "l", 1);
        usleep(5 * 1000);
    }
    return 0;
}
