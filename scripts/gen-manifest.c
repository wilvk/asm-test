/*
 * gen-manifest.c — emit the asm-test "binding ABI" layout manifest as JSON.
 *
 * Track 0 (multi-language bindings substrate). Language bindings must mirror the
 * C structs the trampoline and emulator fill — regs_t, the per-guest emu
 * register files, and the result structs. Hand-transcribing field offsets is the
 * single highest-risk correctness hazard (a wrong offset silently validates
 * garbage), so instead of publishing offsets in prose, this program is compiled
 * against the real headers and prints sizeof/offsetof for the active host arch.
 * A binding's code generator consumes the JSON; the _Static_assert guards in the
 * headers keep the structs themselves from drifting.
 *
 * Build/run via `make manifest` (writes asmtest_abi.json). Needs no Unicorn:
 * asmtest_emu.h declares the guest structs without pulling in <unicorn.h>.
 */
#include <stddef.h>
#include <stdio.h>

#include "asmtest.h"
#include "asmtest_emu.h"

/* offsetof/sizeof of a field in one go. */
#define FOFF(st, f) offsetof(st, f), sizeof(((st *)0)->f)

static int field_first; /* comma bookkeeping within a "fields" array */

static void field(const char *name, size_t off, size_t size) {
    printf("%s\n        {\"name\": \"%s\", \"offset\": %zu, \"size\": %zu}",
           field_first ? "" : ",", name, off, size);
    field_first = 0;
}
#define FIELD(st, f) field(#f, FOFF(st, f))

static int struct_first = 1; /* comma bookkeeping within "structs" */

static void begin_struct(const char *name, size_t size, size_t align) {
    printf("%s\n    {\n      \"name\": \"%s\", \"size\": %zu, \"align\": %zu,\n"
           "      \"fields\": [",
           struct_first ? "" : ",", name, size, align);
    struct_first = 0;
    field_first = 1;
}
#define BEGIN(st) begin_struct(#st, sizeof(st), _Alignof(st))

static void end_struct(void) { printf("\n      ]\n    }"); }

int main(void) {
#if defined(__x86_64__)
    const char *arch = "x86_64";
#elif defined(__aarch64__)
    const char *arch = "aarch64";
#else
    const char *arch = "unknown";
#endif

#if defined(ASMTEST_ABI_WIN64)
    const char *abi = "win64";
#else
    const char *abi = "sysv";
#endif

    printf("{\n  \"asmtest_abi\": {\n");
    printf("    \"version\": \"%s\",\n", ASMTEST_VERSION);
    printf("    \"version_num\": %d,\n", ASMTEST_VERSION_NUM);
    printf("    \"host_arch\": \"%s\",\n", arch);
    printf("    \"abi\": \"%s\",\n", abi);
    printf("    \"pointer_bits\": %zu,\n", sizeof(void *) * 8);
    printf("    \"structs\": [");

    /* --- Native capture tier (host arch) --- */
    BEGIN(vec128_t);
    FIELD(vec128_t, u8);
    FIELD(vec128_t, u64);
    FIELD(vec128_t, f64);
    end_struct();

    BEGIN(regs_t);
    FIELD(regs_t, ret);
#if defined(ASMTEST_ABI_WIN64)
    FIELD(regs_t, rdi); /* Win64 adds rdi/rsi to the callee-saved set */
    FIELD(regs_t, rsi);
#endif
    FIELD(regs_t, flags);
    FIELD(regs_t, fret);
    FIELD(regs_t, vec);
    end_struct();

    /* --- Emulator tier (all guests are declared on every host) --- */
    BEGIN(emu_vec128_t);
    FIELD(emu_vec128_t, u8);
    FIELD(emu_vec128_t, u64);
    FIELD(emu_vec128_t, f64);
    end_struct();

    BEGIN(emu_x86_regs_t);
    FIELD(emu_x86_regs_t, rax);
    FIELD(emu_x86_regs_t, rip);
    FIELD(emu_x86_regs_t, rflags);
    FIELD(emu_x86_regs_t, xmm);
    end_struct();

    BEGIN(emu_arm64_regs_t);
    FIELD(emu_arm64_regs_t, x);
    FIELD(emu_arm64_regs_t, sp);
    FIELD(emu_arm64_regs_t, pc);
    FIELD(emu_arm64_regs_t, nzcv);
    FIELD(emu_arm64_regs_t, v);
    end_struct();

    BEGIN(emu_riscv_regs_t);
    FIELD(emu_riscv_regs_t, x);
    FIELD(emu_riscv_regs_t, pc);
    FIELD(emu_riscv_regs_t, f);
    end_struct();

    BEGIN(emu_arm_regs_t);
    FIELD(emu_arm_regs_t, r);
    FIELD(emu_arm_regs_t, cpsr);
    FIELD(emu_arm_regs_t, q);
    end_struct();

    /* Result structs: the fault-as-data surface every binding decodes. The
     * register file lives at .regs; its layout is the matching *_regs_t above. */
    BEGIN(emu_result_t);
    FIELD(emu_result_t, ok);
    FIELD(emu_result_t, uc_err);
    FIELD(emu_result_t, faulted);
    FIELD(emu_result_t, fault_addr);
    FIELD(emu_result_t, fault_kind);
    FIELD(emu_result_t, regs);
    end_struct();

    BEGIN(emu_arm64_result_t);
    FIELD(emu_arm64_result_t, ok);
    FIELD(emu_arm64_result_t, uc_err);
    FIELD(emu_arm64_result_t, faulted);
    FIELD(emu_arm64_result_t, fault_addr);
    FIELD(emu_arm64_result_t, fault_kind);
    FIELD(emu_arm64_result_t, regs);
    end_struct();

    BEGIN(emu_riscv_result_t);
    FIELD(emu_riscv_result_t, ok);
    FIELD(emu_riscv_result_t, uc_err);
    FIELD(emu_riscv_result_t, faulted);
    FIELD(emu_riscv_result_t, fault_addr);
    FIELD(emu_riscv_result_t, fault_kind);
    FIELD(emu_riscv_result_t, regs);
    end_struct();

    BEGIN(emu_arm_result_t);
    FIELD(emu_arm_result_t, ok);
    FIELD(emu_arm_result_t, uc_err);
    FIELD(emu_arm_result_t, faulted);
    FIELD(emu_arm_result_t, fault_addr);
    FIELD(emu_arm_result_t, fault_kind);
    FIELD(emu_arm_result_t, regs);
    end_struct();

    printf("\n    ],\n");

    /* --- Callee-saved sentinels (host arch) --- */
    printf("    \"sentinels\": {");
#if defined(__x86_64__)
    printf("\n      \"RBX\": \"0x%lx\", \"RBP\": \"0x%lx\", \"R12\": \"0x%lx\",\n"
           "      \"R13\": \"0x%lx\", \"R14\": \"0x%lx\", \"R15\": \"0x%lx\"\n",
           ASMTEST_SENTINEL_RBX, ASMTEST_SENTINEL_RBP, ASMTEST_SENTINEL_R12,
           ASMTEST_SENTINEL_R13, ASMTEST_SENTINEL_R14, ASMTEST_SENTINEL_R15);
#elif defined(__aarch64__)
    printf("\n      \"X19\": \"0x%lx\", \"X20\": \"0x%lx\", \"X21\": \"0x%lx\",\n"
           "      \"X22\": \"0x%lx\", \"X23\": \"0x%lx\", \"X24\": \"0x%lx\",\n"
           "      \"X25\": \"0x%lx\", \"X26\": \"0x%lx\", \"X27\": \"0x%lx\",\n"
           "      \"X28\": \"0x%lx\", \"X29\": \"0x%lx\"\n",
           ASMTEST_SENTINEL_X19, ASMTEST_SENTINEL_X20, ASMTEST_SENTINEL_X21,
           ASMTEST_SENTINEL_X22, ASMTEST_SENTINEL_X23, ASMTEST_SENTINEL_X24,
           ASMTEST_SENTINEL_X25, ASMTEST_SENTINEL_X26, ASMTEST_SENTINEL_X27,
           ASMTEST_SENTINEL_X28, ASMTEST_SENTINEL_X29);
#endif
    printf("    },\n");

    /* --- Condition-flag bit masks (host arch) --- */
    printf("    \"flags\": {");
#if defined(__x86_64__)
    printf("\n      \"CF\": %lu, \"PF\": %lu, \"ZF\": %lu, \"SF\": %lu, "
           "\"OF\": %lu\n",
           ASMTEST_CF, ASMTEST_PF, ASMTEST_ZF, ASMTEST_SF, ASMTEST_OF);
#elif defined(__aarch64__)
    printf("\n      \"VF\": %lu, \"CF\": %lu, \"ZF\": %lu, \"NF\": %lu\n",
           ASMTEST_VF, ASMTEST_CF, ASMTEST_ZF, ASMTEST_NF);
#endif
    printf("    }\n");

    printf("  }\n}\n");
    return 0;
}
