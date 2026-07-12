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
 * asserts a single cs_open call site). Only x86-64 and arm64 are armed (both
 * already decode elsewhere); ARM32 / RISCV64 are stubbed (return 0). Without
 * Capstone every entry point degrades to a no-op, so the file always compiles.
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

/* Map an asmtest_arch_t to Capstone's (arch, mode). Only x86-64 and arm64 are
 * armed for operand enumeration (the plan stubs ARM32 / RISCV64 here); returns
 * false for the rest so the enumerator degrades to 0. */
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
    case ASMTEST_ARCH_RISCV64:
        return false; /* stubbed: no operand model armed for these yet */
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

/* Add the instruction's explicit MEMORY operands to the read / write buffers.
 * Direction comes from the per-operand .access bits (a read-modify-write operand
 * lands in BOTH); on a Capstone DIET build (no .access) it defaults to a read. */
static void add_mem_ops(cs_arch a, const cs_detail *d, bool diet,
                        at_val_rec_t *reads, size_t *nr, size_t rcap,
                        at_val_rec_t *writes, size_t *nw, size_t wcap) {
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
    }
}
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
        bool diet = cs_support(CS_SUPPORT_DIET);
        cs_regs rr, rw;
        uint8_t nr = 0, nw = 0;
        if (cs_regs_access(h, &insn[0], rr, &nr, rw, &nw) == CS_ERR_OK) {
            for (uint8_t i = 0; i < nr; i++)
                put_reg(reads, &rc, rcap, rr[i], false);
            for (uint8_t i = 0; i < nw; i++)
                put_reg(writes, &wc, wcap, rw[i], true);
        }
        add_mem_ops(a, d, diet, reads, &rc, rcap, writes, &wc, wcap);
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
