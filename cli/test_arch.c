/* test_arch.c — headless unit test for the register/step/watch arch seam.
 *
 * cli/asmspy_arch.h lifts asmspy's x86-64-hardcoded register reads (rip/rsp/rax/
 * orig_rax) and the AArch64 NT_ARM_HW_WATCH control-word encoding behind one
 * seam. A wrong field offset returns garbage and a wrong DBGWCR/BAS split arms
 * the wrong bytes — both silent at runtime. This pins them PURELY (no ptrace, no
 * hardware), so it runs green on EVERY host and covers the AArch64 encoder even
 * on x86-64 where no AArch64 watchpoint can fire. Built + run by `make cli-smoke`
 * before the ptrace smoke, exactly like cli/test_graphsort.c.
 *
 * Two cores:
 *   1. The register accessors resolve to the right field per arch — asserted by
 *      writing a sentinel into the underlying struct and reading it back through
 *      the accessor (the mapping IS the thing under test).
 *   2. The DBGWCR/DBGWVR/BAS encoder — asserted against the kernel's numeric
 *      contract (8-byte store = 0x1FF5, sub-word offset BAS, boundary reject).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "asmspy_arch.h"

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* ---- 1. register accessors map to the right field per arch ---- */
static void test_accessors(void) {
    asmspy_regs_t g;
    memset(&g, 0, sizeof g);

#if defined(__aarch64__)
    /* AArch64 user_pt_regs: regs[0] return, regs[8] syscall nr, regs[30] LR,
     * sp, pc, pstate. Confirm each accessor reads exactly its field. */
    g.r.pc = 0x1111222233334444ULL;
    g.r.sp = 0x0000fffffffff000ULL;
    g.r.regs[0] = 0xaaaabbbbccccddddULL;
    g.r.regs[8] = 0x000000000000005eULL; /* an arbitrary syscall number */
    g.r.regs[30] = 0x5555666677778888ULL;
    CHECK(asmspy_reg_pc(&g) == 0x1111222233334444ULL, "arm pc");
    CHECK(asmspy_reg_sp(&g) == 0x0000fffffffff000ULL, "arm sp");
    CHECK(asmspy_reg_ret(&g) == 0xaaaabbbbccccddddULL, "arm ret (x0)");
    CHECK(asmspy_reg_syscall_nr(&g) == 0x5eULL, "arm syscall nr (x8)");
    CHECK(asmspy_reg_lr(&g) == 0x5555666677778888ULL, "arm lr (x30)");
    asmspy_set_pc(&g, 0xdeadbeefULL);
    CHECK(g.r.pc == 0xdeadbeefULL, "arm set_pc");
    CHECK(asmspy_reg_pc(&g) == 0xdeadbeefULL, "arm set_pc read-back");

    /* The fixed user_pt_regs layout the GETREGSET(NT_PRSTATUS) read depends on. */
    _Static_assert(offsetof(struct user_regs_struct, regs) == 0,
                   "x0 at offset 0");
    _Static_assert(offsetof(struct user_regs_struct, sp) == 248, "sp @ 248");
    _Static_assert(offsetof(struct user_regs_struct, pc) == 256, "pc @ 256");
    _Static_assert(offsetof(struct user_regs_struct, pstate) == 264,
                   "pstate @ 264");
    _Static_assert(sizeof(struct user_regs_struct) == 272,
                   "user_pt_regs is 272 bytes");
#else
    /* x86-64: rip/rsp/rax/orig_rax; no link register (lr == 0). */
    g.r.rip = 0x1111222233334444ULL;
    g.r.rsp = 0x0000fffffffff000ULL;
    g.r.rax = 0xaaaabbbbccccddddULL;
    g.r.orig_rax = 0x0000000000000101ULL; /* SYS_openat on x86-64 */
    CHECK(asmspy_reg_pc(&g) == 0x1111222233334444ULL, "x86 pc (rip)");
    CHECK(asmspy_reg_sp(&g) == 0x0000fffffffff000ULL, "x86 sp (rsp)");
    CHECK(asmspy_reg_ret(&g) == 0xaaaabbbbccccddddULL, "x86 ret (rax)");
    CHECK(asmspy_reg_syscall_nr(&g) == 0x101ULL, "x86 syscall nr (orig_rax)");
    CHECK(asmspy_reg_lr(&g) == 0, "x86 has no link register");
    asmspy_set_pc(&g, 0xdeadbeefULL);
    CHECK(g.r.rip == 0xdeadbeefULL, "x86 set_pc");
    CHECK(asmspy_reg_pc(&g) == 0xdeadbeefULL, "x86 set_pc read-back");
#endif
}

/* ---- 2. AArch64 DBGWCR/DBGWVR/BAS encoder (pure; every host) ---- */
static void test_watch_encode(void) {
    /* The canonical 8-byte write watch: BAS=0xff, LSC=store(0b10), PAC=EL0(0b10),
     * E=1 -> (0xff<<5)|(0b10<<3)|(0b10<<1)|1 = 0x1FF5. */
    CHECK(asmspy_dbgwcr_word(0, 8) == 0x1FF5u,
          "8-byte write DBGWCR = 0x1FF5 (got 0x%x)", asmspy_dbgwcr_word(0, 8));
    /* --rw flips LSC to both (0b11): the store field 0b10 -> 0b11 adds 1<<3. */
    CHECK(asmspy_dbgwcr_word(1, 8) == 0x1FFDu,
          "8-byte rw DBGWCR = 0x1FFD (got 0x%x)", asmspy_dbgwcr_word(1, 8));

    /* Aligned sub-word watches at offset 0: BAS shrinks with len. */
    CHECK(((asmspy_dbgwcr_word(0, 1) >> 5) & 0xff) == 0x01u, "len1 BAS=0x01");
    CHECK(((asmspy_dbgwcr_word(0, 2) >> 5) & 0xff) == 0x03u, "len2 BAS=0x03");
    CHECK(((asmspy_dbgwcr_word(0, 4) >> 5) & 0xff) == 0x0fu, "len4 BAS=0x0f");

    /* A 4-byte watch at base+4 lands in the HIGH half of the 8-byte window:
     * DBGWVR = base, BAS = 0xf0 (bits 4..7). */
    asmspy_watch_enc_t e = asmspy_watch_encode(0x40000004ULL, 4, 0);
    CHECK(e.ok, "base+4 len4 must be encodable");
    CHECK(e.dbgwvr == 0x40000000ULL, "base+4 DBGWVR = base (got 0x%llx)",
          (unsigned long long)e.dbgwvr);
    CHECK(((e.dbgwcr >> 5) & 0xff) == 0xf0u, "base+4 BAS = 0xf0 (got 0x%x)",
          (e.dbgwcr >> 5) & 0xff);

    /* A 4-byte watch at base+6 would cross the 8-byte boundary -> rejected. */
    asmspy_watch_enc_t bad = asmspy_watch_encode(0x40000006ULL, 4, 0);
    CHECK(!bad.ok,
          "base+6 len4 crosses the 8-byte window and must be rejected");

    /* An 8-byte watch is only valid 8-aligned (offset 0). */
    asmspy_watch_enc_t mis = asmspy_watch_encode(0x40000001ULL, 8, 0);
    CHECK(!mis.ok, "base+1 len8 crosses the window and must be rejected");

    /* A byte watch at base+7 is the last legal byte in the window. */
    asmspy_watch_enc_t b7 = asmspy_watch_encode(0x40000007ULL, 1, 0);
    CHECK(b7.ok, "base+7 len1 must be encodable");
    CHECK(((b7.dbgwcr >> 5) & 0xff) == 0x80u,
          "base+7 len1 BAS = 0x80 (got 0x%x)", (b7.dbgwcr >> 5) & 0xff);
    CHECK(b7.dbgwvr == 0x40000000ULL, "base+7 DBGWVR = base");

    /* Bad lengths are rejected. */
    CHECK(!asmspy_watch_encode(0x40000000ULL, 3, 0).ok, "len3 rejected");
    CHECK(!asmspy_watch_encode(0x40000000ULL, 0, 0).ok, "len0 rejected");
}

int main(void) {
    test_accessors();
    test_watch_encode();
    if (failures) {
        fprintf(stderr, "test_arch: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("test_arch: PASS\n");
    return 0;
}
