/*
 * fuzz_libfuzzer.c — drive an x86-64 guest routine under the emulator with
 * libFuzzer, feeding the emulator's basic-block coverage into libFuzzer's
 * feedback channel (Track E external-engine shim; see
 * docs/guides/fuzzing-shim.md and docs/internal/implementations/libfuzzer-afl-shim.md).
 *
 * THE CRUX. The guest is raw machine code executed under Unicorn, so
 * `clang -fsanitize=fuzzer` never sees it — its automatic SanitizerCoverage
 * instruments only this harness loop, not the guest. So we register an EXTERNAL
 * 8-bit counter array via __sanitizer_cov_8bit_counters_init and write the
 * guest's executed block offsets into it BY HAND (through the tested
 * emu_cover_hits seam), exactly as afl-qemu-trace / Unicorn-mode / FRIDA-mode do
 * for binary-only targets, and exactly as Jazzer feeds a non-C runtime's
 * coverage. libFuzzer then consumes our array as if it were compiler-generated.
 *
 * `-fsanitize=fuzzer` links libFuzzer's own main() and its SanitizerCoverage
 * runtime; the guest bytes are DATA (Unicorn runs them), so no compiler counter
 * exists for them — hence the external array. Every recorded block offset is
 * < code_len (offsets are measured from routine entry), so the offset indexes
 * the counter map directly with no hash.
 *
 * Guest variants, selected at compile time:
 *   default             CLASSIFY3, a fault-free classify(x)->{-1,0,+1}; the
 *                       no-crash baseline (-runs=... exits 0, cov: climbs).
 *   -DFUZZ_CRASH_GUEST  the same shape but the negative path dereferences [rdi],
 *                       which for a negative rdi is an unmapped address: a bug
 *                       reachable ONLY on the negative path. The emulator
 *                       SANDBOXES that fault (it never crashes this process), so
 *                       we surface it through the coverage the seam reports —
 *                       reaching the negative block means the buggy path ran —
 *                       and raise a real SIGABRT so the engine saves a crash-*
 *                       artifact. The coverage feedback is what steers the engine
 *                       to a negative input within budget.
 *
 * -DFUZZ_AFL_DRIVER reuses this SAME harness under AFL++ (Path A): libAFLDriver.a
 * (afl-clang-fast -fsanitize=fuzzer) supplies main() and bridges our
 * LLVMFuzzerTestOneInput into AFL's forkserver loop. AFL++ ships NO
 * SanitizerCoverage runtime, so the guest coverage is fed into AFL's shared-
 * memory map (asmtest_afl_map_bump, examples/fuzz_afl_map.c) instead of sancov
 * counters — same guest, same emu_cover_hits path, only the coverage SINK
 * differs. (Re-verified against the installed AFL++: it provides no
 * __sanitizer_cov_* symbols, so a verbatim sancov reuse would not link.)
 */
#include "asmtest_emu.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef FUZZ_AFL_DRIVER
/* Feed AFL's shared-memory map (defined in the plain-compiled fuzz_afl_map.c so
 * afl-clang-fast's pass does not rewrite the __afl_area_ptr reference). */
extern void asmtest_afl_map_bump(uint64_t block_off);
#else
/* SanitizerCoverage entry points: register an external 8-bit counter array so
 * libFuzzer treats our emulator coverage exactly like compiler-generated
 * counters. Declared by us because the guest is NOT clang-instrumented. The
 * runtime resolves these purely by symbol name, so uint8_t* matches its char*.
 *
 * clang-18's libFuzzer cross-checks that every registered 8-bit-counter region
 * has a matching PC-table region of EQUAL length — registering counters alone
 * aborts at startup with "The size of coverage PC tables does not match ...".
 * (The doc's research note that the PC table is "symbolization only, not
 * required for the feedback loop" predates this check; re-verified against the
 * installed libFuzzer, which requires it.) So we also register a synthetic PC
 * table: one { PC, flags } entry per counter, PC a distinct synthetic address
 * so a crash symbolizes to the guest block index. */
extern void __sanitizer_cov_8bit_counters_init(uint8_t *start, uint8_t *stop);
extern void __sanitizer_cov_pcs_init(const uintptr_t *pcs_beg,
                                     const uintptr_t *pcs_end);

typedef struct {
    uintptr_t pc;    /* synthetic guest-offset program counter */
    uintptr_t flags; /* bit 0 = function entry (libFuzzer convention) */
} pc_table_entry_t;
#endif

#ifdef FUZZ_CRASH_GUEST
/* classify(x): negative path (js target, block 0x12) is buggy — it loads from
 * [rdi], unmapped for a negative rdi, so the emulator faults there. A benign
 * `xor eax,eax` leads the block so the block entry is recorded before the fault.
 *   0x00 xor eax,eax / 0x02 test rdi,rdi / 0x05 js 0x12
 *   0x07 test rdi,rdi / 0x0a jz 0x11 / 0x0c mov eax,1 / 0x11 ret
 *   0x12 xor eax,eax / 0x14 mov rax,[rdi]  (FAULTS) / 0x17 ret               */
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
#define CRASH_BLOCK 0x12u /* reaching the negative block == the bug fired */
#else
/* classify(x) -> {-1,0,+1}, three branch paths, no memory access — the fault-
 * free baseline. Raw x86-64 bytes, host-independent under Unicorn. */
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

/* block offsets are < code_len, so one slot per byte over-covers the block set */
#define NCOUNTERS (sizeof GUEST)
static uint64_t g_blocks[NCOUNTERS]; /* deduped offsets fit in code_len slots */
static emu_t *g_emu;
#ifndef FUZZ_AFL_DRIVER
static uint8_t g_counters[NCOUNTERS];
static pc_table_entry_t g_pcs[NCOUNTERS]; /* one PC per counter, same length */
#endif

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    g_emu = emu_open();
#ifndef FUZZ_AFL_DRIVER
    for (size_t i = 0; i < NCOUNTERS; i++) {
        g_pcs[i].pc = (uintptr_t)0x400000u + i; /* distinct synthetic PCs */
        g_pcs[i].flags = (i == 0) ? 1u : 0u;    /* first = function entry */
    }
    __sanitizer_cov_8bit_counters_init(g_counters, g_counters + NCOUNTERS);
    __sanitizer_cov_pcs_init((const uintptr_t *)g_pcs,
                             (const uintptr_t *)(g_pcs + NCOUNTERS));
#endif
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
#ifndef FUZZ_AFL_DRIVER
    memset(g_counters, 0, sizeof g_counters); /* per-input coverage */
#endif
    long arg = 0;
    memcpy(&arg, Data, Size < sizeof arg ? Size : sizeof arg);

    size_t n = emu_cover_hits(g_emu, GUEST, sizeof GUEST, &arg, 1,
                              /*max_insns default*/ 0, g_blocks, NCOUNTERS);
    for (size_t i = 0; i < n; i++) {
        uint64_t off = g_blocks[i];
#ifdef FUZZ_AFL_DRIVER
        asmtest_afl_map_bump(off); /* guest node coverage into AFL's shm map */
#else
        if (off < NCOUNTERS)
            g_counters[off]++; /* offset indexes the external map directly */
#endif
#ifdef CRASH_BLOCK
        if (off == CRASH_BLOCK)
            abort(); /* the buggy negative path ran — a real crash */
#endif
    }
    return 0;
}
