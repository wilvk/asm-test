/*
 * xstate_hi16.h — where the UPPER SIXTEEN vector registers (zmm16..31, and so
 * ymm16-31 / xmm16-31) live inside an XSAVE image, and whether they are there at
 * all.
 *
 * Split out of src/dataflow_ptrace.c so the DECISION is a pure function of four
 * measured inputs, testable with combinations this machine cannot produce (see
 * cli/test_hi16.c). The producer and the test call the same code.
 *
 * THE ONE THING THIS HEADER EXISTS TO GET RIGHT. `CPUID(0xd,0).EAX` bit 7 reports
 * which state components XCR0 *can* enable — it is a statement about the part,
 * and it is unchanged by whether XCR0 actually enabled anything. Gating on it is
 * the classic mistake the ISA rule exists to prevent ("check OSXSAVE + XGETBV,
 * never the CPUID feature bit alone"), and here it is not academic:
 *
 *   An AVX-512 part booted with `clearcpuid=avx512f` — Linux drops
 *   XFEATURE_MASK_AVX512 from XCR0, but raw CPUID is untouched, so
 *   CPUID(0xd,7).EBX still reports 1408. On a PKU-capable CPU, PKRU (component 9,
 *   offset 2432) keeps the ptrace image 2440 bytes long, so a length check still
 *   passes and offset 1408 is a KERNEL-ZEROED HOLE. A reader gated on the CPUID
 *   bit returns success with 32 zero bytes, and the producer records
 *   value_valid:true, bytes:"00..00" — a fabricated register value for a live
 *   process, which is the one outcome this producer must never produce.
 *
 *   A hypervisor exposing leaf 0xD while masking XCR0 gets there the same way.
 *
 * So the gate below is XCR0, read via XGETBV, plus the ISA feature itself. It is
 * a HARDWARE/OS gate (CLAUDE.md): there is nothing installable that would put
 * ymm16-31 on a part that lacks them or a kernel that declined to enable them.
 */
#ifndef ASMTEST_XSTATE_HI16_H
#define ASMTEST_XSTATE_HI16_H

#include <stddef.h>
#include <stdint.h>

/* Bytes per register in the Hi16_ZMM component (7). It saves the FULL 512-bit
 * zmm16..31, so ymm16 is the first 32 bytes of a slot and xmm16 the first 16.
 * Measured (Ryzen 9 9950X, CPUID(0xd,7)): size 1024 = 16 x 64, offset 1408. */
#define ASMTEST_HI16_STRIDE 64

/* The AVX-512 state group in XCR0: opmask (5), ZMM_Hi256 (6), Hi16_ZMM (7). All
 * three, not bit 7 alone — Linux enables and disables them as a set, and this is
 * exactly the state test __builtin_cpu_supports("avx512f") performs (libgcc's
 * __cpu_indicator_init requires XSTATE_OPMASK|ZMM|HI_ZMM before it will report
 * avx512f). Matching it is deliberate: cli/evex_victim.c's own guard is that
 * builtin, and a guard that disagreed with the producer would let the smoke pass
 * a host the producer declines, or vice versa. */
#define ASMTEST_XCR0_AVX512                                                    \
    (((uint64_t)1 << 5) | ((uint64_t)1 << 6) | ((uint64_t)1 << 7))

/* The decision. Returns the byte offset of zmm16's slot in a standard-format
 * XSAVE image, or 0 for "this machine has no readable upper sixteen" — on which
 * every caller must decline rather than invent bytes.
 *
 *   isa_avx512f  the ISA feature is present AND usable (pass
 *                __builtin_cpu_supports("avx512f"); it folds in the XGETBV test).
 *   xcr0         XGETBV(0), the mask the OS actually enabled. NOT CPUID(0xd,0).EAX.
 *   comp_size    CPUID(0xd,7).EAX — the component's size in bytes.
 *   comp_off     CPUID(0xd,7).EBX — its standard-format offset. Not architecturally
 *                fixed the way component 2's 576 is; it must be read, never assumed.
 *
 * comp_size is checked against the full 16 x 64 because a part reporting a
 * SMALLER Hi16_ZMM lays its upper sixteen out in some way this code does not
 * model, and reading it on the 64-byte-stride assumption would be a silent
 * misread — the exact failure mode with no loud symptom. */
static inline size_t asmtest_hi16_slot_offset(int isa_avx512f, uint64_t xcr0,
                                              unsigned comp_size,
                                              unsigned comp_off) {
    if (!isa_avx512f)
        return 0;
    if ((xcr0 & ASMTEST_XCR0_AVX512) != ASMTEST_XCR0_AVX512)
        return 0; /* enumerated by the part, never enabled by the OS */
    if (comp_off == 0 || comp_size < 16u * ASMTEST_HI16_STRIDE)
        return 0;
    return (size_t)comp_off;
}

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>

/* Gather the four inputs on THIS machine and decide. Kept beside the decision so
 * the producer and cli/test_hi16.c's live-consistency check run identical code —
 * a test that re-implemented the gather could agree with itself while disagreeing
 * with what ships. */
static inline size_t asmtest_hi16_probe(void) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    uint64_t xcr0 = 0;
    unsigned size = 0, off = 0;
    /* XGETBV #UDs unless the OS set CR4.OSXSAVE, which it advertises in
     * CPUID(1).ECX bit 27 — so that bit is a precondition for READING xcr0, not
     * a substitute for reading it. */
    if (__get_cpuid(1, &a, &b, &c, &d) && (c & (1u << 27))) {
        unsigned lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        xcr0 = ((uint64_t)hi << 32) | lo;
    }
    if (__get_cpuid_count(0xd, 7, &a, &b, &c, &d)) {
        size = a;
        off = b;
    }
    return asmtest_hi16_slot_offset(__builtin_cpu_supports("avx512f") ? 1 : 0,
                                    xcr0, size, off);
}
#endif /* x86 */

#endif /* ASMTEST_XSTATE_HI16_H */
