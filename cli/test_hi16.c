/*
 * test_hi16.c — pins src/xstate_hi16.h's decision about the UPPER SIXTEEN vector
 * registers, which the live ptrace producer reads for ymm16-31 / xmm16-31.
 *
 * WHY THIS IS A UNIT TEST AND NOT A CAPTURE. The dangerous input is one this
 * machine cannot be put into without a reboot: a part that ENUMERATES the
 * Hi16_ZMM component while the OS never ENABLED it (`clearcpuid=avx512f`, or a
 * hypervisor masking XCR0). There, CPUID(0xd,7).EBX still reports 1408, a
 * PKU-capable CPU still yields a 2440-byte ptrace image so a length check still
 * passes, and offset 1408 is a kernel-zeroed hole. A reader gated on
 * CPUID(0xd,0).EAX bit 7 — which reports what XCR0 *can* enable, not what it did
 * — returns 32 zero bytes as if measured, and the producer writes
 * value_valid:true, bytes:"00..00" for a live process. Fabricating a register
 * value is the one thing this producer must never do, and no capture on this
 * host can reach that state, so the decision is tested directly instead.
 *
 * The live case at the bottom is the other half: it asserts that the shipped
 * probe agrees with __builtin_cpu_supports("avx512f") on THIS machine, whichever
 * way that goes. cli/evex_victim.c's hardware guard is that builtin, and
 * cli/cli_smoke.sh's ymm16-31 section claims guard and producer answer the same
 * predicate — this is that claim, asserted rather than commented.
 */
#include <stdio.h>

#include "xstate_hi16.h"

static int failures;

/* Measured on the host this was developed against (Ryzen 9 9950X): component 7
 * at offset 1408, size 1024 = 16 x 64, with XCR0 = 0x2e7 (x87|SSE|AVX|opmask|
 * ZMM_Hi256|Hi16_ZMM|PKRU). Every case below perturbs exactly one of those. */
#define REAL_XCR0 0x2e7u
#define REAL_OFF  1408u
#define REAL_SIZE 1024u

static void expect(const char *label, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL %s\n  want %zu\n  got  %zu\n", label, want, got);
        failures++;
    }
}

int main(void) {
    /* The healthy machine: everything present and enabled. Without this the
     * decline cases below would all pass on a function that always returns 0. */
    expect("enabled host -> the enumerated offset",
           asmtest_hi16_slot_offset(1, REAL_XCR0, REAL_SIZE, REAL_OFF),
           REAL_OFF);

    /* THE REVIEWER'S CASE. Hi16_ZMM enumerated at 1408, XCR0 bit 7 clear: the
     * component exists on the part and does not exist in the image. Must decline
     * — returning 1408 here is what turns a kernel-zeroed hole into a recorded
     * register value. */
    expect("component enumerated, XCR0 bit 7 (Hi16_ZMM) clear -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0 & ~(1u << 7), REAL_SIZE,
                                    REAL_OFF),
           0);

    /* clearcpuid=avx512f: Linux drops the whole AVX-512 state group from XCR0.
     * Raw CPUID is untouched, so the offset still reads 1408. */
    expect("clearcpuid=avx512f (XCR0 bits 5/6/7 clear) -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0 & ~0xE0u, REAL_SIZE, REAL_OFF),
           0);

    /* Partial enablement must not squeak through: the group is checked as a
     * group, matching what __builtin_cpu_supports("avx512f") requires. */
    expect("XCR0 opmask (5) clear, 6/7 set -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0 & ~(1u << 5), REAL_SIZE,
                                    REAL_OFF),
           0);
    expect("XCR0 ZMM_Hi256 (6) clear, 5/7 set -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0 & ~(1u << 6), REAL_SIZE,
                                    REAL_OFF),
           0);

    /* A hypervisor masking the ISA bit while leaving the xstate layout visible. */
    expect("ISA avx512f absent, XCR0 fully enabled -> decline",
           asmtest_hi16_slot_offset(0, REAL_XCR0, REAL_SIZE, REAL_OFF), 0);

    /* No such component: CPUID(0xd,7) reports zeroes on a pre-AVX-512 part. */
    expect("no component 7 at all -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0, 0, 0), 0);

    /* Enumerated but SMALLER than 16 x 64: the part lays its upper sixteen out
     * in a way this code does not model, and reading it on the 64-byte-stride
     * assumption would be a silent misread with no loud symptom. */
    expect("component smaller than 16 x 64 -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0, 16u * 32u, REAL_OFF), 0);

    /* Offset 0 is not a real slot (the legacy FXSAVE area lives there). */
    expect("offset 0 -> decline",
           asmtest_hi16_slot_offset(1, REAL_XCR0, REAL_SIZE, 0), 0);

    /* An offset the developer host never produced: the decision must PASS IT
     * THROUGH, not launder it into a hardcoded 1408. */
    expect("a different enumerated offset is honoured, not overridden",
           asmtest_hi16_slot_offset(1, REAL_XCR0, REAL_SIZE, 2048u), 2048u);

    /* LIVE: the shipped probe on this machine must agree with the predicate
     * cli/evex_victim.c guards on. Either both say yes (an enabled AVX-512 host,
     * where the smoke's value assertions run for real) or both say no (where it
     * self-skips) — a DISAGREEMENT is the bug, in whichever direction. */
    {
        const int builtin_says = __builtin_cpu_supports("avx512f") ? 1 : 0;
        const size_t probe_says = asmtest_hi16_probe() != 0 ? 1 : 0;
        if ((size_t)builtin_says != probe_says) {
            fprintf(
                stderr,
                "FAIL live parity: __builtin_cpu_supports(\"avx512f\")=%d "
                "but asmtest_hi16_probe()=%zu -- evex_victim's guard and the "
                "producer disagree about this host\n",
                builtin_says, asmtest_hi16_probe());
            failures++;
        }
        printf("test_hi16: this host: avx512f=%d hi16_offset=%zu\n",
               builtin_says, asmtest_hi16_probe());
    }

    if (failures) {
        fprintf(stderr, "test_hi16: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_hi16: PASS\n");
    return 0;
}
