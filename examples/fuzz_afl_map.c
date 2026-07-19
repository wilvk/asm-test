/*
 * fuzz_afl_map.c — write one executed guest block into AFL++'s shared-memory
 * coverage bitmap. Shared by both AFL harnesses (examples/fuzz_afl.c native
 * forkserver, and examples/fuzz_libfuzzer.c under aflpp_driver).
 *
 * WHY A SEPARATE TU. This must be compiled by a PLAIN compiler ($(CC)), NOT by
 * afl-clang-fast: the afl-clang-fast instrumentation pass rewrites a harness's
 * own `__afl_area_ptr` reference to a module-local `__afl_area_ptr.2` copy that
 * is only defined inside instrumented functions, so a direct map write from
 * afl-clang-fast-compiled code fails to link ("undefined reference to
 * __afl_area_ptr.2"). Kept here, uninstrumented, the extern resolves cleanly to
 * afl-compiler-rt's real global, which the forkserver repoints at the shm map.
 *
 * NODE (per-block) coverage: derive a location from the block address and mask
 * to the map (lcamtuf's binary-only / emulator hash), matching the libFuzzer
 * 8-bit-counter model. AFL's native EDGE coverage would need the ordered block
 * sequence, which the deduped trace does not carry (out of scope — see the doc).
 */
#include <stdint.h>

extern uint8_t *__afl_area_ptr; /* afl-compiler-rt's shm bitmap pointer */
#define MAP_SIZE 65536          /* AFL++ default (1 << 16) */

void asmtest_afl_map_bump(uint64_t block_off) {
    uint32_t loc =
        (uint32_t)(((block_off >> 4) ^ (block_off << 8)) & (MAP_SIZE - 1));
    __afl_area_ptr[loc]++;
}
