/*
 * fuzz_afl.c — drive an x86-64 guest routine under the emulator with AFL++'s
 * native persistent-mode forkserver, writing the emulator's basic-block coverage
 * into AFL's shared-memory bitmap by hand (Track E external-engine shim; see
 * docs/guides/fuzzing-shim.md).
 *
 * THE CRUX (same as the libFuzzer harness). afl-clang-fast instruments only THIS
 * harness's own code — a small constant background in the map — because the guest
 * runs under Unicorn, invisible to it. So for each executed guest block offset we
 * bump AFL's shared-memory map ourselves (asmtest_afl_map_bump, in the plain-
 * compiled examples/fuzz_afl_map.c — see there for why the map write cannot live
 * in this afl-clang-fast-compiled file): the exact afl-qemu-trace / Unicorn-mode
 * mechanism for binary-only targets.
 *
 * Persistent mode + deferred forkserver: __AFL_INIT() starts the forkserver
 * AFTER emu_open() (the expensive setup runs once); __AFL_LOOP(N) reuses the
 * process across N testcases (~2x-20x); each testcase arrives in shared memory
 * via __AFL_FUZZ_TESTCASE_BUF / _LEN. Run OUTSIDE afl-fuzz, __AFL_LOOP executes
 * once reading stdin, so the harness is a deterministic single-shot unit too.
 *
 * -DFUZZ_CRASH_GUEST selects the crashing guest whose negative path faults; the
 * harness surfaces reaching that block as a real abort() so afl-fuzz records a
 * crash. The default guest is the fault-free CLASSIFY3 baseline.
 */
#include "asmtest_emu.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* read(): __AFL_FUZZ_TESTCASE_LEN expands to a read(0,...) */

__AFL_FUZZ_INIT(); /* file scope: persistent-mode / shared-memory testcase */

/* Write one executed guest block into AFL's map. Defined in the plain-compiled
 * examples/fuzz_afl_map.c so afl-clang-fast's pass does not rewrite the
 * __afl_area_ptr reference (see that file). */
extern void asmtest_afl_map_bump(uint64_t block_off);

#ifdef FUZZ_CRASH_GUEST
/* classify(x): the negative path (block 0x12) loads from [rdi] — unmapped for a
 * negative rdi, so the emulator faults there; reaching the block == the bug. */
static const uint8_t GUEST[] = {
    0x31, 0xc0,                   /* 0x00  xor eax, eax        */
    0x48, 0x85, 0xff,             /* 0x02  test rdi, rdi       */
    0x78, 0x0b,                   /* 0x05  js   0x12 (neg)     */
    0x48, 0x85, 0xff,             /* 0x07  test rdi, rdi       */
    0x74, 0x05,                   /* 0x0a  jz   0x11 (zero)    */
    0xb8, 0x01, 0x00, 0x00, 0x00, /* 0x0c  mov eax, 1  (pos)   */
    0xc3,                         /* 0x11  ret                 */
    0x31, 0xc0,                   /* 0x12  xor eax, eax  (neg) */
    0x48, 0x8b, 0x07,             /* 0x14  mov rax, [rdi] FAULT */
    0xc3,                         /* 0x17  ret                 */
};
#define CRASH_BLOCK 0x12u
#else
/* classify(x) -> {-1,0,+1}, three branch paths, no memory access — the baseline. */
static const uint8_t GUEST[] = {
    0x31, 0xc0,                   /* 0x00  xor eax, eax        */
    0x48, 0x85, 0xff,             /* 0x02  test rdi, rdi       */
    0x78, 0x0b,                   /* 0x05  js   0x12 (neg)     */
    0x48, 0x85, 0xff,             /* 0x07  test rdi, rdi       */
    0x74, 0x05,                   /* 0x0a  jz   0x11 (zero)    */
    0xb8, 0x01, 0x00, 0x00, 0x00, /* 0x0c  mov eax, 1  (pos)   */
    0xc3,                         /* 0x11  ret                 */
    0xb8, 0xff, 0xff, 0xff, 0xff, /* 0x12  mov eax, -1 (neg)   */
    0xc3,                         /* 0x17  ret                 */
};
#endif

static uint64_t g_blocks[sizeof GUEST];

int main(void) {
    emu_t *e = emu_open();
    __AFL_INIT(); /* deferred forkserver, after the expensive emu_open() */
    unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
    while (__AFL_LOOP(10000)) {
        int len = __AFL_FUZZ_TESTCASE_LEN;
        long arg = 0;
        memcpy(&arg, buf, (size_t)len < sizeof arg ? (size_t)len : sizeof arg);
        size_t n = emu_cover_hits(e, GUEST, sizeof GUEST, &arg, 1, 0, g_blocks,
                                  sizeof GUEST);
        for (size_t i = 0; i < n; i++) {
            uint64_t off = g_blocks[i];
            asmtest_afl_map_bump(off); /* node coverage into AFL's shm bitmap */
#ifdef CRASH_BLOCK
            if (off == CRASH_BLOCK)
                abort(); /* the buggy negative path ran — a real crash */
#endif
        }
    }
    return 0;
}
