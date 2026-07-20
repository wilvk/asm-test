/* jit_victim.c — a minimal JIT stand-in for asmspy's perf-map smoke.
 *
 * Real managed runtimes (Node/V8, .NET, OpenJDK) execute code they compile at
 * runtime from anonymous executable mappings the ELF symtab cannot name, and
 * publish those names in /tmp/perf-<pid>.map. This victim does the same with one
 * hand-written function: it mmaps an executable page, writes a small hot loop
 * into it, registers that region in the perf map as "jit_hot_loop", and calls it
 * forever. asmspy must name the anonymous region's instructions
 * "jit_hot_loop [jit]" instead of a bare "0x..". Opts in via PR_SET_PTRACER_ANY
 * like the other example victims so attach works in a plain container. x86-64.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#ifndef PR_SET_PTRACER
#define PR_SET_PTRACER 0x59616d61
#endif
#ifndef PR_SET_PTRACER_ANY
#define PR_SET_PTRACER_ANY ((unsigned long)-1)
#endif

#if defined(__aarch64__)
/* long f(int n): a=0; do a++; while (a<n); return a;  (AAPCS64: n in w0)
 *   52800001         mov  w1, #0
 *   11000421   L:    add  w1, w1, #1
 *   6b00003f         cmp  w1, w0
 *   54ffffc3         b.lo L          ; a < n (unsigned)
 *   2a0103e0         mov  w0, w1
 *   d65f03c0         ret
 */
static const unsigned char HOT_CODE[] = {
    0x01, 0x00, 0x80, 0x52, 0x21, 0x04, 0x00, 0x11, 0x3f, 0x00, 0x00, 0x6b,
    0xc3, 0xff, 0xff, 0x54, 0xe0, 0x03, 0x01, 0x2a, 0xc0, 0x03, 0x5f, 0xd6};
#else
/* long f(int n): a=0; do a++; while (a<n); return a;  (System V: n in edi)
 *   31 c0            xor  eax, eax
 *   83 c0 01   L:    add  eax, 1
 *   39 f8            cmp  eax, edi
 *   72 f9            jb   L          ; a < n (unsigned)
 *   c3               ret
 */
static const unsigned char HOT_CODE[] = {0x31, 0xc0, 0x83, 0xc0, 0x01,
                                         0x39, 0xf8, 0x72, 0xf9, 0xc3};
#endif

int main(void) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memcpy(code, HOT_CODE, sizeof HOT_CODE);
    __builtin___clear_cache((char *)code, (char *)code + sizeof HOT_CODE);

    /* Publish the region in the perf map exactly as a JIT would. */
    char path[64];
    snprintf(path, sizeof path, "/tmp/perf-%d.map", (int)getpid());
    FILE *m = fopen(path, "w");
    if (m) {
        fprintf(m, "%llx %llx jit_hot_loop\n",
                (unsigned long long)(uintptr_t)code,
                (unsigned long long)sizeof HOT_CODE);
        fclose(m);
    }

    fprintf(stderr, "jit_victim pid=%d\n", (int)getpid());
    fflush(stderr);

    long (*hot)(int) = (long (*)(int))code;
    volatile long sink = 0;
    for (;;)
        sink += hot(1000); /* stay resident in the anonymous JIT region */
    return 0;
}
