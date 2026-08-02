/*
 * dataflow_operands.c — the Capstone operand read/write-set enumerator for the
 * data-flow tier (asmtest_operands / asmtest_operands_available in
 * asmtest_valtrace.h). The one Capstone-dependent piece of the L0 substrate; kept
 * in its own translation unit so the pure sink + L1/L2 (dataflow.c) stay
 * dependency-free.
 *
 * It extends the IMM-only operand read in disasm.c to the full read/write set:
 *   - cs_regs_access yields ALL register accesses, explicit AND implicit — eflags,
 *     rsp on push/pop/call/ret, string counters — split into read / write sets;
 *   - the explicit operand loop adds MEMORY operands with their
 *     base/index/scale/disp/segment terms (the effective address is resolved by a
 *     PRODUCER at run time), direction from Capstone's per-operand .access.
 *
 * ONE persistent csh per arch (detail mode on), opened lazily and cached — never a
 * per-call cs_open/cs_close on the hot path (the grep gate in mk/dataflow.mk
 * asserts a single cs_open call site). x86-64, arm64, (60-arm32-riscv-
 * author-mode.md T1) arm32, and (T3) riscv64 are armed (all four already
 * decode elsewhere); riscv64 additionally needs Capstone >= 5 (cs_target()
 * degrades it back to a stub below that). Without Capstone every entry point
 * degrades to a no-op, so the file always compiles.
 */
#include "asmtest_valtrace.h"

#include <string.h>

bool asmtest_operands_available(void) {
#ifdef ASMTEST_HAVE_CAPSTONE
    return true;
#else
    return false;
#endif
}

#ifdef ASMTEST_HAVE_CAPSTONE
#include <capstone/capstone.h>

/* Map an asmtest_arch_t to Capstone's (arch, mode). x86-64, arm64, and (T1)
 * arm32 are armed for operand enumeration; RISCV64 (T3) joins them, gated on
 * Capstone >= 5 (CS_ARCH_RISCV / RISCV_OP_MEM do not exist before that —
 * src/disasm.c's own cs_target() already gates the same way; this mirrors
 * it exactly, including CS_MODE_RISCVC so a compressed (16-bit) instruction
 * the assembler emitted decodes correctly alongside the base 32-bit
 * encoding). Below Capstone 5 this returns false, degrading to 0 exactly as
 * it did before T3, rather than a hard compile failure. ARM32 decodes ARM
 * (A32) mode, never Thumb — the existing ASM_ARM32 assembly path already
 * commits to ARM mode, and this enumerator inherits that choice rather than
 * deciding it (60-arm32-riscv-author-mode.md T1). */
static bool cs_target(asmtest_arch_t arch, cs_arch *a, cs_mode *m) {
    switch (arch) {
    case ASMTEST_ARCH_X86_64:
        *a = CS_ARCH_X86;
        *m = CS_MODE_64;
        return true;
    case ASMTEST_ARCH_ARM64:
        *a = CS_ARCH_ARM64;
        *m = CS_MODE_LITTLE_ENDIAN;
        return true;
    case ASMTEST_ARCH_ARM32:
        *a = CS_ARCH_ARM;
        *m = CS_MODE_ARM;
        return true;
    case ASMTEST_ARCH_RISCV64:
#if defined(CS_API_MAJOR) && (CS_API_MAJOR >= 5)
        *a = CS_ARCH_RISCV;
        *m = (cs_mode)(CS_MODE_RISCV64 | CS_MODE_RISCVC);
        return true;
#else
        return false; /* RISC-V operand enumeration needs Capstone >= 5 */
#endif
    }
    return false;
}

/* Persistent per-arch handle cache (index = asmtest_arch_t). State: 0 = unopened,
 * 1 = ready, -1 = permanently failed. Single-threaded use (the test harness and
 * the emulator producer both drive it from one thread), so no lock. The SOLE
 * cs_open call site — the grep gate depends on that. */
static csh g_handle[4];
static int g_state[4];

static csh get_handle(asmtest_arch_t arch) {
    int idx = (int)arch;
    if (idx < 0 || idx >= 4)
        return 0;
    if (g_state[idx] == 1)
        return g_handle[idx];
    if (g_state[idx] == -1)
        return 0;
    cs_arch a;
    cs_mode m;
    if (!cs_target(arch, &a, &m)) {
        g_state[idx] = -1;
        return 0;
    }
    csh h;
    if (cs_open(a, m, &h) != CS_ERR_OK) {
        g_state[idx] = -1;
        return 0;
    }
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    g_handle[idx] = h;
    g_state[idx] = 1;
    return h;
}

/* Append a register record to a caller buffer, respecting its capacity. */
static void put_reg(at_val_rec_t *buf, size_t *cnt, size_t cap, uint32_t reg,
                    bool is_write) {
    if (buf == NULL || *cnt >= cap)
        return;
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_REG;
    r.reg = reg;
    r.is_write = is_write;
    buf[(*cnt)++] = r;
}

/* Append a memory-operand record (decode-time addressing terms; addr resolved by a
 * producer later). */
static void put_mem(at_val_rec_t *buf, size_t *cnt, size_t cap, uint32_t seg,
                    uint32_t base, uint32_t index, int32_t scale, int64_t disp,
                    uint16_t size, bool is_write) {
    if (buf == NULL || *cnt >= cap)
        return;
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_MEM_ABS; /* space tag; a producer may re-tag to MEM_OFF */
    r.reg = seg;
    r.base = base;
    r.index = index;
    r.scale = scale;
    r.disp = disp;
    r.size = size;
    r.is_write = is_write;
    buf[(*cnt)++] = r;
}

/* Capstone (through at least 5.0) mis-reports the DESTINATION memory operand of a VEX/EVEX
 * vector STORE — `vmovdqu [rsi],ymm1` / `vmovaps [rsi],ymm1` come back access=READ, whereas the
 * SSE form `movdqu [rsi],xmm1` is correctly access=WRITE. Left uncorrected, the def-use oracle
 * never records the store, so an AVX store->load edge is silently dropped. Correct it: a
 * MOV-family instruction's operand[0] (the Intel-order destination) is written, never read.
 * Scoped to the mov/vmov mnemonic + the operand-0 position, so GP/SSE moves (already correct) are
 * idempotent and loads (mem at op[1]) / RMW / compares are untouched. */
static bool x86_move_store_dest(const char *mnem, int op_index) {
    if (op_index != 0 || mnem == NULL)
        return false;
    return strncmp(mnem, "mov", 3) == 0 || strncmp(mnem, "vmov", 4) == 0;
}

/* Add the instruction's explicit MEMORY operands to the read / write buffers.
 * Direction comes from the per-operand .access bits (a read-modify-write operand
 * lands in BOTH), corrected for the Capstone AVX-store quirk above; on a Capstone
 * DIET build (no .access) it defaults to a read. */
static void add_mem_ops(cs_arch a, const cs_detail *d, bool diet,
                        const char *mnem, at_val_rec_t *reads, size_t *nr,
                        size_t rcap, at_val_rec_t *writes, size_t *nw,
                        size_t wcap) {
    if (a == CS_ARCH_X86) {
        for (int i = 0; i < d->x86.op_count; i++) {
            const cs_x86_op *op = &d->x86.operands[i];
            if (op->type != X86_OP_MEM)
                continue;
            uint32_t seg = op->mem.segment == X86_REG_INVALID
                               ? 0
                               : (uint32_t)op->mem.segment;
            uint32_t base =
                op->mem.base == X86_REG_INVALID ? 0 : (uint32_t)op->mem.base;
            uint32_t index =
                op->mem.index == X86_REG_INVALID ? 0 : (uint32_t)op->mem.index;
            bool w = !diet && (op->access & CS_AC_WRITE);
            bool rd = diet || (op->access & CS_AC_READ);
            if (x86_move_store_dest(mnem, i)) {
                w = true; /* a MOV store writes its op[0] destination... */
                rd =
                    false; /* ...and never reads it (fixes the AVX-store quirk) */
            }
            if (rd)
                put_mem(reads, nr, rcap, seg, base, index, op->mem.scale,
                        op->mem.disp, op->size, false);
            if (w)
                put_mem(writes, nw, wcap, seg, base, index, op->mem.scale,
                        op->mem.disp, op->size, true);
        }
    } else if (a == CS_ARCH_ARM64) {
        for (int i = 0; i < d->arm64.op_count; i++) {
            const cs_arm64_op *op = &d->arm64.operands[i];
            if (op->type != ARM64_OP_MEM)
                continue;
            uint32_t base =
                op->mem.base == ARM64_REG_INVALID ? 0 : (uint32_t)op->mem.base;
            uint32_t index = op->mem.index == ARM64_REG_INVALID
                                 ? 0
                                 : (uint32_t)op->mem.index;
            bool w = !diet && (op->access & CS_AC_WRITE);
            bool rd = diet || (op->access & CS_AC_READ);
            if (rd)
                put_mem(reads, nr, rcap, 0, base, index, 0, op->mem.disp, 0,
                        false);
            if (w)
                put_mem(writes, nw, wcap, 0, base, index, 0, op->mem.disp, 0,
                        true);
        }
    } else if (a == CS_ARCH_ARM) {
        /* AArch32 (A32): the arm64 branch's shape, adapted for cs_arm_op's
         * field names (arm_op_mem: base/index/scale/disp/lshift — no `size`
         * on the operand itself, exactly like the arm64 branch above, which
         * likewise passes size 0 rather than reading a field that is not
         * there; 60-arm32-riscv-author-mode.md T1). */
        for (int i = 0; i < d->arm.op_count; i++) {
            const cs_arm_op *op = &d->arm.operands[i];
            if (op->type != ARM_OP_MEM)
                continue;
            uint32_t base =
                op->mem.base == ARM_REG_INVALID ? 0 : (uint32_t)op->mem.base;
            uint32_t index =
                op->mem.index == ARM_REG_INVALID ? 0 : (uint32_t)op->mem.index;
            bool w = !diet && (op->access & CS_AC_WRITE);
            bool rd = diet || (op->access & CS_AC_READ);
            if (rd)
                put_mem(reads, nr, rcap, 0, base, index, 0, op->mem.disp, 0,
                        false);
            if (w)
                put_mem(writes, nw, wcap, 0, base, index, 0, op->mem.disp, 0,
                        true);
        }
    }
}

#if defined(CS_API_MAJOR) && (CS_API_MAJOR >= 5)
/* RISC-V (T3) exact mnemonic sets this file's operand classification keys
 * off — the base GP loads/stores/branches. Not a prefix match: several
 * arithmetic mnemonics ALSO start with 's' (sub, sll/srl/sra, slt/sltu and
 * their immediate forms), so only these closed lists are matched. */
static bool riscv_is_store(const char *mnem) {
    return mnem != NULL &&
           (strcmp(mnem, "sb") == 0 || strcmp(mnem, "sh") == 0 ||
            strcmp(mnem, "sw") == 0 || strcmp(mnem, "sd") == 0);
}

static bool riscv_is_branch(const char *mnem) {
    static const char *const br[] = {"beq",  "bne",  "blt",  "bge",
                                     "bltu", "bgeu", "beqz", "bnez",
                                     "blez", "bgez", "bltz", "bgtz"};
    if (mnem == NULL)
        return false;
    for (size_t i = 0; i < sizeof br / sizeof br[0]; i++)
        if (strcmp(mnem, br[i]) == 0)
            return true;
    return false;
}

/* RISC-V (T3) operand walk — NOT a call site of cs_regs_access + add_mem_ops
 * like every other arch above. Capstone's RISCV backend implements neither:
 *   - cs_regs_access: RISCVModule.c never sets handle->reg_access, so
 *     cs_regs_access(..., CS_ARCH_RISCV ...) returns CS_ERR_ARCH
 *     unconditionally (confirmed against the pinned Capstone 5.0.1 source,
 *     cs.c's cs_regs_access — "this arch is unsupported yet"), never
 *     CS_ERR_OK, so the reads/writes it would fill for every other arch
 *     here are simply never populated for RISC-V.
 *   - a per-operand `.access` field: cs_riscv_op is `{type; union{reg; imm;
 *     mem};}` — no access bits at all, unlike cs_x86_op/cs_arm64_op/
 *     cs_arm_op, so add_mem_ops's op->access pattern does not even compile
 *     against it.
 * So direction is inferred from the RV32I/RV64I base ISA's own regular
 * instruction-format rules instead, keyed on the CLOSED mnemonic sets above
 * (Capstone's own operand ORDER already mirrors the assembly syntax's
 * dest-first form, verified against the pinned build): R/I/U/J-type
 * arithmetic and loads put the destination register at operand[0]
 * (WRITE) and read everything else; S-type stores put the STORED VALUE at
 * operand[0] (READ — a store writes memory, never a register) and read
 * everything else; B-type branches read every register operand and write
 * nothing. A MEM operand's base register is always an implicit READ (it
 * feeds the address computation on both a load and a store), recorded
 * alongside its `put_mem` memory-location record — mirroring what
 * cs_regs_access gives every other arch here "for free" as an implicit
 * base-register read. RISCV_OP_IMM carries no location to record. Base
 * integer ISA only (D7: no F/D-extension mnemonics recognised), mirroring
 * this file's arm64/arm32 integer-only scope. A pseudo-instruction Capstone
 * itself reports with zero operands (`ret` == `jalr x0, 0(ra)`) faithfully
 * enumerates nothing — `ra` is invisible to THIS producer for that specific
 * alias, exactly as Capstone reports it, never a fabricated read.
 *
 * DEDUPES read registers within one instruction (e.g. `add a3, a2, a2`,
 * whose rs1 and rs2 are the SAME register): cs_regs_access returns a SET
 * for every other arch here, so a register read twice by one instruction
 * still yields exactly one read record — a raw per-operand walk over
 * d->riscv.operands[] would otherwise emit it twice, inflating the def-use
 * graph with a duplicate edge (caught by tools/asmtrace_record.c's own
 * riscv-df-chain golden: `add a3, a2, a2` read-twice without this dedup
 * produced 3 def-use edges over the chain instead of the arm64/arm32
 * chains' 2, for the identical instruction shape). */
static void add_riscv_ops(const cs_detail *d, const char *mnem,
                          at_val_rec_t *reads, size_t *nr, size_t rcap,
                          at_val_rec_t *writes, size_t *nw, size_t wcap) {
    bool is_store = riscv_is_store(mnem);
    bool is_branch = riscv_is_branch(mnem);
    uint32_t seen[8];
    int nseen = 0;
    for (int i = 0; i < d->riscv.op_count; i++) {
        const cs_riscv_op *op = &d->riscv.operands[i];
        bool is_dest = (i == 0) && !is_store && !is_branch;
        if (op->type == RISCV_OP_REG) {
            if (is_dest) {
                put_reg(writes, nw, wcap, op->reg, true);
                continue; /* an R/I-type dest is written only, not also read */
            }
            uint32_t r = op->reg;
            bool dup = false;
            for (int j = 0; j < nseen; j++)
                if (seen[j] == r) {
                    dup = true;
                    break;
                }
            if (dup)
                continue;
            if (nseen < (int)(sizeof seen / sizeof seen[0]))
                seen[nseen++] = r;
            put_reg(reads, nr, rcap, r, false);
        } else if (op->type == RISCV_OP_MEM) {
            uint32_t base = op->mem.base == RISCV_REG_INVALID
                                ? 0
                                : (uint32_t)op->mem.base;
            if (op->mem.base != RISCV_REG_INVALID) {
                bool dup = false;
                for (int j = 0; j < nseen; j++)
                    if (seen[j] == base) {
                        dup = true;
                        break;
                    }
                if (!dup) {
                    if (nseen < (int)(sizeof seen / sizeof seen[0]))
                        seen[nseen++] = base;
                    put_reg(reads, nr, rcap, base, false); /* address base */
                }
            }
            put_mem(is_store ? writes : reads, is_store ? nw : nr,
                    is_store ? wcap : rcap, 0, base, 0, 0, op->mem.disp, 0,
                    is_store);
        }
        /* RISCV_OP_IMM: no location to record. */
    }
}
#endif /* CS_API_MAJOR >= 5 */
#endif /* ASMTEST_HAVE_CAPSTONE */

size_t asmtest_operands(asmtest_arch_t arch, const uint8_t *code, size_t len,
                        uint64_t off, at_val_rec_t *reads, size_t *nreads,
                        at_val_rec_t *writes, size_t *nwrites) {
    size_t rcap = nreads != NULL ? *nreads : 0;
    size_t wcap = nwrites != NULL ? *nwrites : 0;
    if (nreads != NULL)
        *nreads = 0;
    if (nwrites != NULL)
        *nwrites = 0;
#ifdef ASMTEST_HAVE_CAPSTONE
    if (code == NULL || off >= len)
        return 0;
    csh h = get_handle(arch);
    if (h == 0)
        return 0;
    cs_arch a;
    cs_mode m;
    (void)cs_target(arch, &a, &m); /* h != 0 implies this succeeds */

    cs_insn *insn = NULL;
    size_t count = cs_disasm(h, code + off, len - (size_t)off, off, 1, &insn);
    if (count == 0)
        return 0;
    size_t ilen = insn[0].size;
    const cs_detail *d = insn[0].detail;
    size_t rc = 0, wc = 0;
    if (d != NULL) {
#if defined(CS_API_MAJOR) && (CS_API_MAJOR >= 5)
        if (a == CS_ARCH_RISCV) {
            /* Neither cs_regs_access nor add_mem_ops's op->access pattern
             * works for RISC-V (see add_riscv_ops's own comment) — one
             * dedicated walk covers both register and memory operands. */
            add_riscv_ops(d, insn[0].mnemonic, reads, &rc, rcap, writes, &wc,
                          wcap);
        } else
#endif
        {
            bool diet = cs_support(CS_SUPPORT_DIET);
            cs_regs rr, rw;
            uint8_t nr = 0, nw = 0;
            if (cs_regs_access(h, &insn[0], rr, &nr, rw, &nw) == CS_ERR_OK) {
                for (uint8_t i = 0; i < nr; i++)
                    put_reg(reads, &rc, rcap, rr[i], false);
                for (uint8_t i = 0; i < nw; i++)
                    put_reg(writes, &wc, wcap, rw[i], true);
            }
            add_mem_ops(a, d, diet, insn[0].mnemonic, reads, &rc, rcap,
                        writes, &wc, wcap);
        }
    }
    if (nreads != NULL)
        *nreads = rc;
    if (nwrites != NULL)
        *nwrites = wc;
    cs_free(insn, count);
    return ilen;
#else
    (void)arch;
    (void)code;
    (void)len;
    (void)off;
    (void)reads;
    (void)writes;
    (void)rcap;
    (void)wcap;
    return 0;
#endif
}
