/* jitdump_victim.c — a JIT stand-in that publishes its code via the BINARY
 * perf jitdump format (jit-<pid>.dump), NOT the text perf-map.
 *
 * The sibling jit_victim proves the text /tmp/perf-<pid>.map tier; this victim
 * proves the jitdump tier end to end: it mmaps an anonymous executable page,
 * writes a small hot loop into it, describes that region in a jitdump file
 * (header + one JIT_CODE_LOAD record) created in the directory given as
 * argv[1] (default /tmp), and then — like real emitters (perf's jvmti agent,
 * LLVM, Julia) — mmaps the file's header page so the filename is visible in
 * /proc/<pid>/maps, which is exactly how asmspy (and perf) discover it. No
 * perf-map is written, so a resolved name can only have come from the jitdump
 * reader. Opts in via PR_SET_PTRACER_ANY like the other victims. x86-64, LE.
 */
#define _GNU_SOURCE
#include <fcntl.h>
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

/* long f(int n): a=0; do a++; while (a<n); return a;  (System V: n in edi)
 *   31 c0            xor  eax, eax
 *   83 c0 01   L:    add  eax, 1
 *   39 f8            cmp  eax, edi
 *   72 f9            jb   L          ; a < n (unsigned)
 *   c3               ret
 */
static const unsigned char HOT_CODE[] = {0x31, 0xc0, 0x83, 0xc0, 0x01,
                                         0x39, 0xf8, 0x72, 0xf9, 0xc3};

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

int main(int argc, char **argv) {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    void *code = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memcpy(code, HOT_CODE, sizeof HOT_CODE);
    __builtin___clear_cache((char *)code, (char *)code + sizeof HOT_CODE);

    /* Write the jitdump: 40-byte header, then one JIT_CODE_LOAD (id 0) naming
     * the anonymous region "dump_hot_loop", code bytes appended per the spec. */
    const char *dir = argc > 1 ? argv[1] : "/tmp";
    const char name[] = "dump_hot_loop";
    char path[512];
    snprintf(path, sizeof path, "%s/jit-%d.dump", dir, (int)getpid());
    FILE *d = fopen(path, "w+");
    if (!d) {
        perror(path);
        return 1;
    }
    put32(d, 0x4A695444); /* magic 'JiTD' (little-endian writer) */
    put32(d, 1);          /* version                             */
    put32(d, 40);         /* header total_size                   */
    put32(d, 62);         /* elf_mach = EM_X86_64                */
    put32(d, 0);          /* pad1                                */
    put32(d, (uint32_t)getpid());
    put64(d, 1); /* timestamp */
    put64(d, 0); /* flags     */
    put32(d, 0); /* record id = JIT_CODE_LOAD */
    put32(d, (uint32_t)(16 + 40 + sizeof name + sizeof HOT_CODE));
    put64(d, 2); /* record timestamp */
    put32(d, (uint32_t)getpid());
    put32(d, (uint32_t)getpid());        /* tid (single-threaded: == pid) */
    put64(d, (uint64_t)(uintptr_t)code); /* vma       */
    put64(d, (uint64_t)(uintptr_t)code); /* code_addr */
    put64(d, sizeof HOT_CODE);           /* code_size */
    put64(d, 1);                         /* code_index */
    fwrite(name, sizeof name, 1, d);     /* NUL included */
    fwrite(HOT_CODE, sizeof HOT_CODE, 1, d);
    fflush(d);

    /* The discovery marker: map the file's header page (like real emitters do)
     * so the tracer finds the filename in /proc/<pid>/maps. */
    if (mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fileno(d), 0) == MAP_FAILED) {
        perror("mmap jitdump marker");
        return 1;
    }

    fprintf(stderr, "jitdump_victim pid=%d dump=%s\n", (int)getpid(), path);
    fflush(stderr);

    long (*hot)(int) = (long (*)(int))code;
    volatile long sink = 0;
    for (;;)
        sink += hot(1000); /* stay resident in the anonymous JIT region */
    return 0;
}
