/*
 * cs_backend.c — ARM CoreSight decode backend for the hardware-trace tier
 * (OpenCSD). See asmtest_hwtrace.h and docs/native-tracing.md.
 *
 * CoreSight ETM/ETE waypoint trace is captured (perf cs_etm AUX, by hwtrace.c) and
 * deformatted + decoded by OpenCSD, which emits OCSD_GEN_TRC_ELEM_INSTR_RANGE
 * elements — each a contiguous run of executed instructions [start, end) ending at a
 * waypoint (branch). Those ranges are reconstructed against asm-test's registered
 * code bytes into the same instruction-offset stream the Intel PT and AMD LBR
 * backends produce, normalized into Unicorn/DynamoRIO-equivalent basic blocks.
 *
 * The reconstruction is split into two halves, mirroring the AMD backend:
 *
 *   - asmtest_cs_reconstruct() — the DECODER-INDEPENDENT core: ordered instruction
 *     ranges -> insns[]/blocks[]. It is arch-parameterized and needs only the
 *     Capstone length-decoder, so it is host-validated WITHOUT a CoreSight board by
 *     feeding it synthetic ranges (the same way examples/test_hwtrace.c validates
 *     the AMD reconstruction from a synthetic branch stack). This half is DONE and
 *     tested.
 *
 *   - asmtest_cs_decode() — the LIVE OpenCSD path: build an ocsd_create_dcd_tree for
 *     the formatted cs_etm AUX stream, attach an ETMv4/ETE instruction decoder with
 *     a memory accessor serving [base, base+len), and translate each
 *     OCSD_GEN_TRC_ELEM_INSTR_RANGE element into an asmtest_cs_range_t fed to the
 *     core. This half needs the OpenCSD library AND a real AArch64 CoreSight board to
 *     write and validate, so — per the project's "no untested hardware code" rule —
 *     it is NOT implemented here: asmtest_cs_decoder_present() returns 0 so
 *     asmtest_hwtrace_available(CORESIGHT) self-skips on every host until the tree is
 *     completed on a board. The reconstruction core it will feed is already proven.
 *
 * Gated on -DASMTEST_HAVE_OPENCSD (pkg-config libopencsd); the reconstruction core
 * compiles regardless (Capstone only).
 */
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#define ASMTEST_HW_OK 0
#define ASMTEST_HW_ENOSYS (-5)
#define ASMTEST_HW_EDECODE (-8)

/* One executed instruction range, as an ETM/ETE INSTR_RANGE element resolves to:
 * the byte offsets [start_off, end_off) of the run from the region entry, and
 * whether its last instruction is a branch waypoint (so the next range's first
 * instruction begins a new block). Offsets, not absolute addresses, so the core is
 * position-independent. KEEP IN SYNC with the declaration in
 * examples/test_hwtrace.c. */
typedef struct {
    uint64_t start_off;
    uint64_t end_off;   /* one past the last instruction in the range */
    int ends_in_branch; /* range terminates at a branch waypoint      */
} asmtest_cs_range_t;

/* Decoder-independent reconstruction: replay the ordered instruction ranges through
 * the Capstone length-decoder to recover each executed offset, normalizing blocks at
 * range boundaries — single-entry/ends-at-branch, byte-identical to pt_backend.c. A
 * block starts at the region entry and at the first instruction of any range that
 * follows a branch waypoint; instructions within a range fall through. `arch` selects
 * the Capstone mode (ASMTEST_ARCH_ARM64 for a real CoreSight trace; the synthetic
 * host test passes ASMTEST_ARCH_X86_64 so its offsets are directly comparable to the
 * PT/AMD/single-step backends over the shared fixture). Sets trace->truncated and
 * stops on an undecodable instruction, never emitting a partial trace as complete. */
int asmtest_cs_reconstruct(asmtest_arch_t arch, const asmtest_cs_range_t *ranges,
                           size_t nranges, const void *base, size_t len,
                           asmtest_trace_t *trace) {
    if (ranges == NULL || base == NULL || len == 0 || trace == NULL)
        return ASMTEST_HW_EDECODE;

    const uint8_t *code = (const uint8_t *)base;
    uint64_t base_ip = (uint64_t)(uintptr_t)base;
    int prev_was_branch = 1; /* the first executed instruction starts a block */

    for (size_t r = 0; r < nranges; r++) {
        uint64_t off = ranges[r].start_off;
        uint64_t end = ranges[r].end_off;
        if (end > len)
            end = len;
        while (off < end) {
            trace_append_insn(trace, off);
            if (prev_was_branch)
                trace_append_block(trace, off);
            /* Within a range instructions fall through; only a range that ends at a
             * branch waypoint opens a new block for the NEXT range. */
            prev_was_branch = 0;

            size_t l = asmtest_disas(arch, code, len, base_ip, off, NULL, 0);
            if (l == 0) {
                trace->truncated = true;
                return ASMTEST_HW_OK;
            }
            off += l;
        }
        if (ranges[r].ends_in_branch)
            prev_was_branch = 1;
    }
    return ASMTEST_HW_OK;
}

#ifdef ASMTEST_HAVE_OPENCSD

/* The live OpenCSD decode tree (ocsd_create_dcd_tree + ETMv4 decoder + memory
 * accessor + element callback feeding asmtest_cs_reconstruct) is written and
 * validated on a real AArch64 CoreSight board; until then this reports the decoder
 * absent so the tier self-skips even where libopencsd is installed (e.g. an AArch64
 * host without ETM access). Implementing it does not change the reconstruction core
 * above, which is already host-validated. */
int asmtest_cs_decoder_present(void) { return 0; }

int asmtest_cs_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace) {
    (void)aux;
    (void)aux_len;
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

#else /* !ASMTEST_HAVE_OPENCSD */

int asmtest_cs_decoder_present(void) { return 0; }

int asmtest_cs_decode(const uint8_t *aux, size_t aux_len, const void *base,
                      size_t len, asmtest_trace_t *trace) {
    (void)aux;
    (void)aux_len;
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

#endif /* ASMTEST_HAVE_OPENCSD */
