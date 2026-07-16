/*
 * dataflow_victim.c — the shared LIVE VICTIM fixture for the language bindings'
 * live-attach data-flow lanes (F7, live-attach-dataflow-followup-plan.md).
 *
 * Every `make dataflow-<lang>-test` lane needs the same thing to attach TO: a
 * process it did NOT trace-fork, running independently, whose target routine sits
 * at an address the tracer can learn. Ten hand-rolled victims (one per binding)
 * would be ten chances to drift; this is ONE, spawned by all ten wrappers, so the
 * lanes differ only in the FFI under test — which is the thing being tested.
 *
 * It mirrors the victim `examples/test_dataflow_ptrace.c` forks for the native
 * suite's attach_pid case (same `df_chain` bytes, same tight call loop, same
 * survival counter), with the one difference a foreign binding forces: the C
 * suite inherits the executable mapping through fork(), so the parent already
 * knows `base`, whereas this victim is exec'd and must PUBLISH its base — hence
 * the stdout handshake below. The producer reads the region bytes back out of the
 * target with process_vm_readv, so it needs nothing else from us.
 *
 * Protocol (a binding wrapper's side of it is ~5 lines in any language):
 *   argv: dataflow_victim <counter_path> <a> <b>
 *   stdout, one line, then flushed:   base=0x<hex> len=<decimal> pid=<decimal>
 *   then: loop forever calling region(a, b) and bumping a counter in the
 *         8-byte little-endian file at <counter_path>.
 * The caller attaches at `base`, expects the region to return a+b, then reads the
 * counter file twice to prove the victim SURVIVED the detach. Passing a/b in
 * makes the expected result a property of THIS run, not a constant a stubbed
 * wrapper could hardcode.
 *
 * The victim reports its OWN pid because not every binding's spawn primitive can
 * tell the caller: Lua's io.popen returns a stream and no pid at all, and the
 * popen family runs the command under `sh -c`, so a caller-side pid can silently
 * be the SHELL's rather than the victim's — and attaching to the wrong pid is a
 * failure that looks like "the region was never entered". getpid() here is the one
 * answer that is right in every language.
 *
 * PR_SET_PTRACER_ANY: the victim OPTS IN to being traced, so the lanes work under
 * Yama ptrace_scope=1 (the common host default, and non-namespaced — a container
 * inherits it) without the caller needing to be an ancestor. Same move the
 * hwtrace attach-demo victim makes. It is advisory and drops away where Yama is
 * absent; the caller still needs ptrace permission (a plain child does).
 */
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

/* The SAME fixture the native suite attaches to (examples/test_dataflow_ptrace.c
 * df_chain): six instructions computing rax = rdi + rsi through a store/load pair,
 * so the capture has a real memory def-use edge (step 1 -> step 2) in it and not
 * just register moves. Kept byte-for-byte identical so a binding's live capture is
 * comparable to the native suite's on step count, result and slice shape. */
static const uint8_t df_chain[] = {
    0x48, 0x89, 0xf8,             /* 0x00 mov rax, rdi        */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax    */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x08 mov rcx, [rsp-8]    */
    0x48, 0x8d, 0x14, 0x31,       /* 0x0d lea rdx, [rcx+rsi]  */
    0x48, 0x89, 0xd0,             /* 0x11 mov rax, rdx        */
    0xc3,                         /* 0x14 ret                 */
};

typedef long (*fn2_t)(long, long);

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s <counter_path> <a> <b>\n", argv[0]);
        return 2;
    }
    const char *counter_path = argv[1];
    const long a = strtol(argv[2], NULL, 0);
    const long b = strtol(argv[3], NULL, 0);

    /* Opt in to being traced (see the header comment). Advisory: ignore failure. */
    (void)prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    const size_t len = sizeof df_chain;
    void *ex = mmap(NULL, len, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ex == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memcpy(ex, df_chain, len);
    /* W^X: drop write before execute, exactly as the native suite's victim does. */
    if (mprotect(ex, len, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        return 1;
    }

    /* The survival counter: an 8-byte file the caller reads twice AFTER the
     * detach. A shared FILE (not anonymous shm) because the reader is a foreign
     * process in another language — every binding can read 8 bytes. */
    int fd = open(counter_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (ftruncate(fd, (off_t)sizeof(uint64_t)) != 0) {
        perror("ftruncate");
        return 1;
    }
    uint64_t *counter = mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
    if (counter == MAP_FAILED) {
        perror("mmap counter");
        return 1;
    }
    *counter = 0;

    /* Publish the base the caller attaches at, our pid, and FLUSH — the caller
     * blocks on this line, so a buffered newline would deadlock the lane. */
    printf("base=0x%llx len=%zu pid=%d\n", (unsigned long long)(uintptr_t)ex, len,
           (int)getpid());
    fflush(stdout);

    /* Independent hot loop: call the region, bump the counter, forever. The sleep
     * keeps a spinning victim from burning a core for the whole lane while still
     * entering the region far more often than the tracer needs (the tracer plants
     * one int3 at `base` and waits for the next arrival). */
    struct timespec ts = {0, 2 * 1000 * 1000}; /* 2 ms */
    for (;;) {
        volatile long r = ((fn2_t)ex)(a, b);
        (void)r;
        (*counter)++;
        nanosleep(&ts, NULL);
    }
    return 0;
}
