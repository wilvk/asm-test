/*
 * amd_backend.c — AMD branch-record decode backend for the hardware-trace tier.
 * See docs/plans/amd-lbr-trace-plan.md, asmtest_hwtrace.h, docs/native-tracing.md.
 *
 * AMD has no Intel-PT-style continuous trace ring; the closest native trace is
 * reconstruction from the shallow (16-entry) branch-record stack — Zen 3 BRS or
 * Zen 4 LbrExtV2 — captured via perf PERF_SAMPLE_BRANCH_STACK as an ordered array
 * of {from, to, flags} taken-branch records (hwtrace.c does the capture). This TU
 * turns that array into the same asmtest_trace_t offset stream the Intel PT
 * backend produces, by REPLAYING asm-test's own registered code bytes between the
 * branch waypoints.
 *
 * Unlike libipt, this carries no instruction decoder of its own: it reuses the
 * project's existing Capstone layer through asmtest_disas() (src/disasm.c) for the
 * instruction-length walk. asmtest_amd_decoder_present() is therefore true only
 * when Capstone is linked (asmtest_disas_available()) on a Linux x86-64 build;
 * otherwise the tier self-skips, exactly like the libipt/OpenCSD gating.
 *
 * The capture needs an AMD Zen 3 (BRS) / Zen 4 / Zen 5 (LbrExtV2) host with perf
 * branch-stack permitted; it self-skips on Zen 2 (no branch facility), non-AMD, VMs,
 * and CI. Live capture + decode is verified on a Zen 5 host (Ryzen 9 9950X,
 * amd_lbr_v2 — see examples/test_hwtrace.c test_amd_live and `make docker-hwtrace-amd`);
 * THIS reconstruction is additionally validated host-independently with synthetic
 * perf_branch_entry[] inputs (test_amd_reconstruction), cross-checked against the
 * DynamoRIO/PT block+instruction partition.
 */
#include "asmtest_trace.h"

#include <stddef.h>
#include <stdint.h>

#define ASMTEST_HW_OK 0
#define ASMTEST_HW_ENOSYS (-5)
#define ASMTEST_HW_EDECODE (-8)

#if defined(__linux__) && defined(__x86_64__)
#include <linux/perf_event.h>

int asmtest_amd_decoder_present(void) {
    /* Reconstruction needs the Capstone length-decoder (via asmtest_disas). */
    return asmtest_disas_available() ? 1 : 0;
}

/* AMD's 16-deep branch stack. A full stack (nbr >= AMD_LBR_DEPTH) means the
 * routine took at least that many branches and earlier ones were lost — the
 * window overflowed, so the reconstruction cannot be complete. */
#define AMD_LBR_DEPTH 16

int asmtest_amd_decode(const struct perf_branch_entry *br, size_t nbr,
                       const void *base, size_t len, asmtest_trace_t *trace) {
    if (br == NULL || nbr == 0 || base == NULL || len == 0 || trace == NULL)
        return ASMTEST_HW_EDECODE;
    if (!asmtest_disas_available())
        return ASMTEST_HW_ENOSYS;

    const uint64_t base_ip = (uint64_t)(uintptr_t)base;
    const uint64_t end_ip = base_ip + len;
    uint64_t ip = base_ip;

    /* Block start at the region entry (matches PT/DR normalization). */
    trace_append_block(trace, 0);

    /* perf delivers the branch stack newest-first; replay oldest-first. */
    for (size_t k = nbr; k-- > 0;) {
        const struct perf_branch_entry *e = &br[k];
        if (e->abort)
            continue; /* transactional abort: path rolled back, not executed */
        uint64_t from = e->from, to = e->to;

        /* Decode the straight-line run from the current in-region IP up to and
         * including the branch instruction at `from`. Skip when IP or the branch
         * is outside the region (e.g. executing inside a callee). */
        if (ip >= base_ip && ip < end_ip && from >= ip && from < end_ip) {
            uint64_t o = ip - base_ip;
            const uint64_t from_off = from - base_ip;
            for (;;) {
                size_t l = asmtest_disas(ASMTEST_ARCH_X86_64, (const uint8_t *)base,
                                         len, base_ip, o, NULL, 0);
                if (l == 0) { /* undecodable: cannot trust the rest */
                    trace->truncated = true;
                    return ASMTEST_HW_OK;
                }
                trace_append_insn(trace, o);
                if (o == from_off)
                    break; /* recorded the branch instruction itself */
                o += l;
                if (o > from_off) { /* walked past the branch: decode desync */
                    trace->truncated = true;
                    return ASMTEST_HW_OK;
                }
            }
        }

        /* Follow the branch. A target inside the region begins a new block (and
         * resumes decoding there); a target outside (call/ret leaving) just moves
         * IP — a later record whose `to` re-enters the region resumes decoding. */
        ip = to;
        if (to >= base_ip && to < end_ip)
            trace_append_block(trace, to - base_ip);
    }

    /* Window overflow: a full stack means earlier branches were dropped, so the
     * reconstruction is incomplete — flag it (the caller falls back to the
     * DynamoRIO tier, which has no depth ceiling). Never emit partial as complete. */
    if (nbr >= AMD_LBR_DEPTH)
        trace->truncated = true;
    return ASMTEST_HW_OK;
}

#else /* not Linux x86-64 */

int asmtest_amd_decoder_present(void) { return 0; }

/* struct perf_branch_entry is Linux-only; use void* so the prototype in
 * hwtrace.c's dispatch still links on other platforms. */
int asmtest_amd_decode(const void *br, size_t nbr, const void *base, size_t len,
                       asmtest_trace_t *trace) {
    (void)br;
    (void)nbr;
    (void)base;
    (void)len;
    (void)trace;
    return ASMTEST_HW_ENOSYS;
}

#endif
