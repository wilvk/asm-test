/*
 * taint_multirange_fixture.h — the shared fixture for the DynamoRIO taint tier's
 * multi-range / method-range-scoping slice (dynamorio-taint-tier-plan.md, Increment 6).
 * Included by BOTH examples/taint_multirange.c (the launched workload) and
 * examples/taint_multirange_validator.c (the out-of-process oracle) so they agree on the
 * fixture bytes without a "KEEP IN SYNC" hand-copy.
 *
 * The fixture is ONE contiguous code blob but is instrumented as TWO DISJOINT ranges with
 * an un-instrumented GAP between them — the whole point of Increment 6 (scope the expensive
 * per-operand shadow work to registered method ranges, not the entire runtime). The taint
 * is carried ACROSS the gap through STACK MEMORY: the tainted value is stored to [rsp-8] in
 * range A and reloaded in range B, so the PROCESS-GLOBAL shadow must persist the tag while
 * control flows through the un-instrumented gap and re-enters a scoped range — the plan's
 * boundary-policy requirement made concrete. The gap does only scratch-register (rdx)
 * arithmetic that never touches [rsp-8] or the carried value, so carrying the tag through
 * UNCHANGED is exact; the emulator forward slice over the whole blob then matches the
 * client's captured taint restricted to the two ranges.
 *
 * Offsets are blob-relative and are also the client's at_vstep_t.off values (the client's
 * offset origin is the lowest registered range base = the blob base), so they line up with
 * the emulator oracle's blob-relative offsets for the out-of-process diff.
 */
#ifndef TAINT_MULTIRANGE_FIXTURE_H
#define TAINT_MULTIRANGE_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

static const uint8_t taint_multirange_code[] = {
    /* ---- RANGE A [0x00,0x08): seed load + stack spill ---- */
    0x48, 0x8b, 0x07,             /* 0x00 mov rax, [rdi]   (SEED origin)  */
    0x48, 0x89, 0x44, 0x24, 0xf8, /* 0x03 mov [rsp-8], rax (spill: taint) */
    /* ---- GAP [0x08,0x18): NOT instrumented. Writes only rdx; never
     *      touches [rsp-8]/rax/rcx/rsp, so the stack tag carried across
     *      the gap is preserved unchanged. ---- */
    0x48, 0x89, 0xf2,             /* 0x08 mov rdx, rsi                    */
    0x48, 0x01, 0xf2,             /* 0x0b add rdx, rsi                    */
    0x48, 0x31, 0xd2,             /* 0x0e xor rdx, rdx                    */
    0x48, 0x01, 0xfa,             /* 0x11 add rdx, rdi                    */
    0x48, 0x31, 0xd2,             /* 0x14 xor rdx, rdx                    */
    0x90,                         /* 0x17 nop                            */
    /* ---- RANGE B [0x18,0x26): reload, derive, sink ---- */
    0x48, 0x8b, 0x4c, 0x24, 0xf8, /* 0x18 mov rcx, [rsp-8] (reload taint) */
    0x48, 0x01, 0xf1,             /* 0x1d add rcx, rsi     (rcx+flags)    */
    0x74, 0x03,                   /* 0x20 jz 0x25          (SINK: ZF)     */
    0x48, 0x89, 0xc8,             /* 0x22 mov rax, rcx                    */
    0xc3,                         /* 0x25 ret                            */
};

/* The two instrumented ranges (base offsets + lengths within the blob). The client
 * registers these via two region-marker calls; the gap [0x08, 0x18) is deliberately left
 * unregistered. */
#define TMR_RANGE_A_OFF 0x00
#define TMR_RANGE_A_LEN 0x08
#define TMR_RANGE_B_OFF 0x18
#define TMR_RANGE_B_LEN 0x0e

#define TMR_SINK_OFF      0x20 /* the jz whose tainted ZF is the sink   */
#define TMR_EXPECT_RESULT 12   /* [rdi]=7, rsi=5 -> rcx = 7+5 = 12      */
#define TMR_EXPECT_STEPS  7    /* range A (2) + range B (5); gap excluded */

#endif /* TAINT_MULTIRANGE_FIXTURE_H */
