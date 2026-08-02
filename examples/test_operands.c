/*
 * test_operands.c — the Phase 0 operand read/write-set enumerator (asmtest_operands
 * in asmtest_valtrace.h), over a HAND-PICKED instruction table on both armed arches
 * (x86-64 + arm64). Mirrors test_ibs's split: a small always-on part, and a
 * Capstone-gated part that self-skips (`# SKIP`, exit 0) on a host without Capstone.
 *
 * The table asserts the full set — including the IMPLICIT operands the plan calls
 * out: `eflags` written by `cmp`, `rsp` read+written by `push`, and a `gs:`-segmented
 * memory operand. x86-64, arm64, (60-arm32-riscv-author-mode.md T1) arm32, and
 * (T3) riscv64 are armed (riscv64 additionally gated on Capstone >= 5, below
 * which it degrades to the old "enumerates nothing" stub).
 */
#include "asmtest_valtrace.h"

#include <stdio.h>
#include <string.h>

static int checks, failures;
#define CHECK(c, m)                                                            \
    do {                                                                       \
        checks++;                                                              \
        printf((c) ? "ok %d - %s\n" : "not ok %d - %s\n", checks, m);          \
        if (!(c))                                                              \
            failures++;                                                        \
    } while (0)

#ifdef ASMTEST_HAVE_CAPSTONE
#include <capstone/capstone.h>

static int has_reg(const at_val_rec_t *b, size_t n, uint32_t id) {
    for (size_t i = 0; i < n; i++)
        if (b[i].kind == AT_LOC_REG && b[i].reg == id)
            return 1;
    return 0;
}
static int has_mem_seg(const at_val_rec_t *b, size_t n, uint32_t seg) {
    for (size_t i = 0; i < n; i++)
        if (b[i].kind != AT_LOC_REG && b[i].reg == seg)
            return 1;
    return 0;
}
static int has_mem_base(const at_val_rec_t *b, size_t n, uint32_t base) {
    for (size_t i = 0; i < n; i++)
        if (b[i].kind != AT_LOC_REG && b[i].base == base)
            return 1;
    return 0;
}

static void test_x86(void) {
    at_val_rec_t rd[64], wr[64];
    size_t nr, nw, len;

    /* mov rax, rdi — read {rdi}, write {rax} */
    const uint8_t mov[] = {0x48, 0x89, 0xf8};
    nr = 64;
    nw = 64;
    len = asmtest_operands(ASMTEST_ARCH_X86_64, mov, sizeof mov, 0, rd, &nr, wr,
                           &nw);
    CHECK(len == 3, "x86 mov rax,rdi: decodes (len 3)");
    CHECK(has_reg(rd, nr, X86_REG_RDI), "x86 mov: reads rdi");
    CHECK(has_reg(wr, nw, X86_REG_RAX), "x86 mov: writes rax");
    CHECK(!has_reg(wr, nw, X86_REG_RDI), "x86 mov: does not write rdi");

    /* cmp rax, rbx — read {rax,rbx}, write {eflags} (implicit) */
    const uint8_t cmp[] = {0x48, 0x39, 0xd8};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_X86_64, cmp, sizeof cmp, 0, rd, &nr, wr, &nw);
    CHECK(has_reg(rd, nr, X86_REG_RAX) && has_reg(rd, nr, X86_REG_RBX),
          "x86 cmp: reads rax and rbx");
    CHECK(has_reg(wr, nw, X86_REG_EFLAGS),
          "x86 cmp: writes eflags (implicit operand)");

    /* push rax — read {rsp,rax}, write {rsp} (implicit rsp both directions) */
    const uint8_t push[] = {0x50};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_X86_64, push, sizeof push, 0, rd, &nr, wr,
                     &nw);
    CHECK(has_reg(rd, nr, X86_REG_RSP), "x86 push: reads rsp (implicit)");
    CHECK(has_reg(wr, nw, X86_REG_RSP), "x86 push: writes rsp (implicit)");
    CHECK(has_reg(rd, nr, X86_REG_RAX), "x86 push: reads rax");

    /* mov rax, gs:[0x10] — a gs:-segmented memory READ + write {rax} */
    const uint8_t gs[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x10, 0x00, 0x00, 0x00};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_X86_64, gs, sizeof gs, 0, rd, &nr, wr, &nw);
    CHECK(has_mem_seg(rd, nr, X86_REG_GS),
          "x86 mov rax,gs:[0x10]: gs-segmented memory read operand");
    CHECK(has_reg(wr, nw, X86_REG_RAX), "x86 gs mov: writes rax");
}

static void test_arm64(void) {
    at_val_rec_t rd[64], wr[64];
    size_t nr, nw, len;

    /* add x0, x1, x2 — read {x1,x2}, write {x0} */
    const uint8_t add[] = {0x20, 0x00, 0x02, 0x8b};
    nr = 64;
    nw = 64;
    len = asmtest_operands(ASMTEST_ARCH_ARM64, add, sizeof add, 0, rd, &nr, wr,
                           &nw);
    CHECK(len == 4, "arm64 add x0,x1,x2: decodes (len 4)");
    CHECK(has_reg(rd, nr, ARM64_REG_X1) && has_reg(rd, nr, ARM64_REG_X2),
          "arm64 add: reads x1 and x2");
    CHECK(has_reg(wr, nw, ARM64_REG_X0), "arm64 add: writes x0");

    /* ldr x0, [x1] — memory READ based on x1, write {x0} */
    const uint8_t ldr[] = {0x20, 0x00, 0x40, 0xf9};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_ARM64, ldr, sizeof ldr, 0, rd, &nr, wr, &nw);
    CHECK(has_mem_base(rd, nr, ARM64_REG_X1),
          "arm64 ldr x0,[x1]: memory read operand based on x1");
    CHECK(has_reg(wr, nw, ARM64_REG_X0), "arm64 ldr: writes x0");
}

/* 60-arm32-riscv-author-mode.md T1: ARM32 (A32) is now armed, mirroring
 * test_arm64 — a straight add + a base-register load. */
static void test_arm32(void) {
    at_val_rec_t rd[64], wr[64];
    size_t nr, nw, len;

    /* add r0, r1, r2 — read {r1,r2}, write {r0} */
    const uint8_t add[] = {0x02, 0x00, 0x81, 0xe0};
    nr = 64;
    nw = 64;
    len = asmtest_operands(ASMTEST_ARCH_ARM32, add, sizeof add, 0, rd, &nr, wr,
                           &nw);
    CHECK(len == 4, "arm32 add r0,r1,r2: decodes (len 4)");
    CHECK(has_reg(rd, nr, ARM_REG_R1) && has_reg(rd, nr, ARM_REG_R2),
          "arm32 add: reads r1 and r2");
    CHECK(has_reg(wr, nw, ARM_REG_R0), "arm32 add: writes r0");

    /* ldr r0, [r1] — memory READ based on r1, write {r0} */
    const uint8_t ldr[] = {0x00, 0x00, 0x91, 0xe5};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_ARM32, ldr, sizeof ldr, 0, rd, &nr, wr, &nw);
    CHECK(has_mem_base(rd, nr, ARM_REG_R1),
          "arm32 ldr r0,[r1]: memory read operand based on r1");
    CHECK(has_reg(wr, nw, ARM_REG_R0), "arm32 ldr: writes r0");
}

/* 60-arm32-riscv-author-mode.md T3: RISCV64 (RV64) is now armed too — gated
 * on Capstone >= 5 (RISCV_REG_* / CS_ARCH_RISCV do not exist before that; below
 * Capstone 5 this degrades back to the old "enumerates nothing" stub, mirror-
 * ing dataflow_operands.c's own cs_target() guard). Unlike arm64/arm32, this
 * enumerator does NOT go through cs_regs_access (Capstone's RISCV backend
 * does not implement it — CS_ERR_ARCH always) — src/dataflow_operands.c's
 * add_riscv_ops infers direction from the RV64I instruction format instead,
 * so this exercises a load (dest write, base-register + mem read) AND a
 * store (source value read, base-register read, mem WRITE — proving op[0]
 * is correctly read-not-written for S-type, the one case that would go
 * wrong under a naive "operand[0] is always the destination" rule). */
static void test_riscv64(void) {
#if defined(CS_API_MAJOR) && (CS_API_MAJOR >= 5)
    at_val_rec_t rd[64], wr[64];
    size_t nr, nw, len;

    /* add a2, a0, a1 — read {a0,a1}, write {a2} */
    const uint8_t add[] = {0x33, 0x06, 0xb5, 0x00};
    nr = 64;
    nw = 64;
    len = asmtest_operands(ASMTEST_ARCH_RISCV64, add, sizeof add, 0, rd, &nr,
                           wr, &nw);
    CHECK(len == 4, "riscv64 add a2,a0,a1: decodes (len 4)");
    CHECK(has_reg(rd, nr, RISCV_REG_A0) && has_reg(rd, nr, RISCV_REG_A1),
          "riscv64 add: reads a0 and a1");
    CHECK(has_reg(wr, nw, RISCV_REG_A2), "riscv64 add: writes a2");

    /* ld a0, 0(a1) — memory READ based on a1, write {a0} */
    const uint8_t ld[] = {0x03, 0xb5, 0x05, 0x00};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_RISCV64, ld, sizeof ld, 0, rd, &nr, wr, &nw);
    CHECK(has_mem_base(rd, nr, RISCV_REG_A1),
          "riscv64 ld a0,0(a1): memory read operand based on a1");
    CHECK(has_reg(rd, nr, RISCV_REG_A1),
          "riscv64 ld: also reads a1 as a plain register (the address base)");
    CHECK(has_reg(wr, nw, RISCV_REG_A0), "riscv64 ld: writes a0");

    /* sd a1, 0(a0) — reads {a1 (the stored value), a0 (the base)}, writes
     * memory only — never a0/a1 themselves (the S-type case). */
    const uint8_t sd[] = {0x23, 0x30, 0xb5, 0x00};
    nr = 64;
    nw = 64;
    asmtest_operands(ASMTEST_ARCH_RISCV64, sd, sizeof sd, 0, rd, &nr, wr, &nw);
    CHECK(has_reg(rd, nr, RISCV_REG_A1),
          "riscv64 sd a1,0(a0): reads a1 (the stored value)");
    CHECK(has_mem_base(wr, nw, RISCV_REG_A0),
          "riscv64 sd a1,0(a0): memory WRITE operand based on a0");
    CHECK(!has_reg(wr, nw, RISCV_REG_A0) && !has_reg(wr, nw, RISCV_REG_A1),
          "riscv64 sd: writes memory only, never a register (S-type)");
#else
    at_val_rec_t rd[8], wr[8];
    size_t nr = 8, nw = 8;
    const uint8_t bytes[] = {0x00, 0x00, 0x00, 0x00};
    CHECK(asmtest_operands(ASMTEST_ARCH_RISCV64, bytes, sizeof bytes, 0, rd,
                           &nr, wr, &nw) == 0 &&
              nr == 0 && nw == 0,
          "stub: RISCV64 enumerates nothing (Capstone < 5)");
#endif
}
#endif /* ASMTEST_HAVE_CAPSTONE */

int main(void) {
    CHECK(asmtest_operands_available() == (
#ifdef ASMTEST_HAVE_CAPSTONE
                                              true
#else
                                              false
#endif
                                              ),
          "operands_available() matches the build");

    /* NULL / out-of-range are rejected regardless of Capstone. */
    size_t nr = 4, nw = 4;
    at_val_rec_t rd[4], wr[4];
    CHECK(asmtest_operands(ASMTEST_ARCH_X86_64, NULL, 0, 0, rd, &nr, wr, &nw) ==
              0,
          "operands: NULL code -> 0");

#ifdef ASMTEST_HAVE_CAPSTONE
    test_x86();
    test_arm64();
    test_arm32();
    test_riscv64();
#else
    printf("# SKIP operand enumerator: built without Capstone\n");
#endif

    printf("1..%d\n", checks);
    if (failures)
        printf("# %d/%d checks FAILED\n", failures, checks);
    return failures ? 1 : 0;
}
