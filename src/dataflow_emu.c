/*
 * dataflow_emu.c — the EMULATOR L0 producer (Phase 2): runs a routine's bytes under
 * Unicorn and fills an asmtest_valtrace_t with the per-step value trace, so the
 * shared L1/L2 (dataflow.c) can be exercised end-to-end with NO hardware. It is the
 * CI proving ground and the reference oracle for the live capture tiers.
 *
 * It is a SELF-CONTAINED Unicorn client (its own uc_engine, mirroring the x86-64
 * SysV setup in emu.c) rather than an extension of the emulator tier, so it adds no
 * coupling to src/emu.c. Value capture follows the plan's timing exactly:
 *   - the UC_HOOK_CODE callback fires PRE-instruction, so register READs are read
 *     via uc_reg_read there (source state); register WRITEs are captured at the
 *     NEXT code hook (destination state), and the last step at emu-stop;
 *   - MEMORY values come from the hooks (UC_HOOK_MEM_READ_AFTER delivers the loaded
 *     value — a plain READ hook would see 0; UC_HOOK_MEM_WRITE delivers the store).
 *
 * The def-use graph the analysis layer builds is keyed on LOCATIONS (register ids /
 * effective addresses), so the slice is correct even where a value is unmapped;
 * the captured values are the human-readable annotation. Output is REPLAY, not
 * observation — labelled as such by the caller.
 *
 * Requires Unicorn + Capstone (the operand enumerator); built only when libunicorn
 * is present (see mk/dataflow.mk), so the whole file is under that gate.
 */
#include "asmtest_valtrace.h"

#include "asmtest_grow.h" /* asmtest_grow / _pow2 — overflow-checked pool growth (S6) */
#include <capstone/capstone.h> /* X86_REG_* ids from the operand enumerator */
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* Guest layout — matches src/emu.c's constants so behaviour is identical. */
#define DF_CODE_BASE  0x00100000UL
#define DF_CODE_SIZE  0x00010000UL
#define DF_STACK_BASE 0x00200000UL
#define DF_STACK_SIZE 0x00010000UL
#define DF_RET_MAGIC  0x00f00000UL

/* A one-step scratch buffer of operand records, finalized + appended when the next
 * step begins (or the run ends). */
typedef struct {
    at_val_rec_t *v;
    size_t n, cap;
} recbuf;

typedef struct {
    uc_engine *uc;
    asmtest_valtrace_t *vt;
    const uint8_t *code;
    size_t code_len;
    uint64_t base;
    int have_cur;
    uint64_t cur_off;
    recbuf cur;
} df_ctx;

static void recbuf_push(recbuf *rb, const at_val_rec_t *r) {
    if (rb->n == rb->cap &&
        !asmtest_grow((void **)&rb->v, &rb->cap, rb->n + 1, sizeof *rb->v))
        return;
    rb->v[rb->n++] = *r;
}

/* Map a Capstone x86 register id to its Unicorn UC_X86_REG_* id, folding a 32/16/8-
 * bit sub-register to its 64-bit container (uc_reg_read then returns the full
 * width; the record's size carries the real operand width). Returns -1 for a
 * register we do not model, in which case the value is simply left uncaptured. */
static int cap_x86_to_uc(uint32_t r) {
    switch (r) {
    case X86_REG_RAX:
    case X86_REG_EAX:
    case X86_REG_AX:
    case X86_REG_AL:
    case X86_REG_AH:
        return UC_X86_REG_RAX;
    case X86_REG_RBX:
    case X86_REG_EBX:
    case X86_REG_BX:
    case X86_REG_BL:
    case X86_REG_BH:
        return UC_X86_REG_RBX;
    case X86_REG_RCX:
    case X86_REG_ECX:
    case X86_REG_CX:
    case X86_REG_CL:
    case X86_REG_CH:
        return UC_X86_REG_RCX;
    case X86_REG_RDX:
    case X86_REG_EDX:
    case X86_REG_DX:
    case X86_REG_DL:
    case X86_REG_DH:
        return UC_X86_REG_RDX;
    case X86_REG_RSI:
    case X86_REG_ESI:
    case X86_REG_SI:
    case X86_REG_SIL:
        return UC_X86_REG_RSI;
    case X86_REG_RDI:
    case X86_REG_EDI:
    case X86_REG_DI:
    case X86_REG_DIL:
        return UC_X86_REG_RDI;
    case X86_REG_RBP:
    case X86_REG_EBP:
    case X86_REG_BP:
    case X86_REG_BPL:
        return UC_X86_REG_RBP;
    case X86_REG_RSP:
    case X86_REG_ESP:
    case X86_REG_SP:
    case X86_REG_SPL:
        return UC_X86_REG_RSP;
    case X86_REG_R8:
        return UC_X86_REG_R8;
    case X86_REG_R9:
        return UC_X86_REG_R9;
    case X86_REG_R10:
        return UC_X86_REG_R10;
    case X86_REG_R11:
        return UC_X86_REG_R11;
    case X86_REG_R12:
        return UC_X86_REG_R12;
    case X86_REG_R13:
        return UC_X86_REG_R13;
    case X86_REG_R14:
        return UC_X86_REG_R14;
    case X86_REG_R15:
        return UC_X86_REG_R15;
    case X86_REG_RIP:
        return UC_X86_REG_RIP;
    case X86_REG_EFLAGS:
        return UC_X86_REG_EFLAGS;
    default:
        return -1;
    }
}

static bool df_reg_read(uc_engine *uc, uint32_t cap_reg, uint64_t *out) {
    int uc_id = cap_x86_to_uc(cap_reg);
    if (uc_id < 0)
        return false;
    uint64_t v = 0;
    if (uc_reg_read(uc, uc_id, &v) != UC_ERR_OK)
        return false;
    *out = v;
    return true;
}

/* R4 (31-wide-register-deck.md T3): read a WIDE (128-bit XMM) register operand into
 * `buf` (16 bytes). Matched by REG ID (XMM0..15), not by the operand's reported
 * size — the enumerator leaves register-operand `size` 0 (it sets width only for
 * memory), so a size gate would never fire. A value the GP df_reg_read above cannot
 * express (it returns a u64), so an FP/vector operand's bytes spill to the valtrace
 * `wide[]` side buffer exactly as a >8-byte memory value does (R1 T3 serializes them
 * as the `bytes` hex string). Returns false for a non-XMM reg (the caller then falls
 * back to the scalar GP path, or leaves the operand value-less — honest). */
static bool df_reg_read_wide(uc_engine *uc, uint32_t cap_reg, uint8_t buf[16]) {
    if (cap_reg < X86_REG_XMM0 || cap_reg > X86_REG_XMM0 + 15)
        return false;
    return uc_reg_read(uc, UC_X86_REG_XMM0 + (int)(cap_reg - X86_REG_XMM0),
                       buf) == UC_ERR_OK;
}

/* R4 T3: capture register operand `r`'s value from the live guest — the wide
 * (128-bit XMM) path spilling to `vt->wide[]`, else the scalar GP path inline. On
 * the wide path it stamps `size` to 16 so the serializer emits the full register's
 * bytes (the enumerator left it 0). Sets r->value/r->wide/r->wide_off/r->size/
 * r->value_valid on success; leaves the record value-less on a reg the producer's
 * value channel cannot read (honest "not measured"). */
static void df_capture_reg_value(uc_engine *uc, asmtest_valtrace_t *vt,
                                 at_val_rec_t *r) {
    uint8_t wbuf[16];
    uint64_t val;
    if (df_reg_read_wide(uc, r->reg, wbuf)) {
        size_t off = asmtest_valtrace_stash_wide(vt, wbuf, 16);
        if (off != (size_t)-1) {
            r->size = 16;
            r->wide = true;
            r->wide_off = (uint32_t)off;
            r->value_valid = true;
        }
    } else if (df_reg_read(uc, r->reg, &val)) {
        r->value = val;
        r->value_valid = true;
    }
}

/* Fill the current step's deferred register-WRITE values (readable now that the
 * instruction has executed) and append the step to the value trace. */
static void df_finalize(df_ctx *c) {
    for (size_t i = 0; i < c->cur.n; i++) {
        at_val_rec_t *r = &c->cur.v[i];
        if (r->is_write && r->kind == AT_LOC_REG && !r->value_valid)
            df_capture_reg_value(c->uc, c->vt, r); /* wide XMM or scalar GP */
    }
    asmtest_valtrace_append(c->vt, c->cur_off, c->cur.v, c->cur.n);
    c->have_cur = 0;
    c->cur.n = 0;
}

/* UC_HOOK_CODE: fires pre-instruction. Finalize the previous step, then open the
 * current one: record register reads (values captured now = source state) and note
 * register writes (values deferred to the next hook). Memory operands are left to
 * the mem hooks, which carry the authoritative effective address + value. */
static void df_on_code(uc_engine *uc, uint64_t address, uint32_t size,
                       void *user) {
    (void)uc;
    (void)size;
    df_ctx *c = (df_ctx *)user;
    if (c->have_cur)
        df_finalize(c);
    uint64_t off = address - c->base;
    c->cur.n = 0;
    c->cur_off = off;
    c->have_cur = 1;

    at_val_rec_t rd[64], wr[64];
    size_t nr = 64, nw = 64;
    asmtest_operands(ASMTEST_ARCH_X86_64, c->code, c->code_len, off, rd, &nr,
                     wr, &nw);
    for (size_t i = 0; i < nr; i++) {
        if (rd[i].kind != AT_LOC_REG)
            continue; /* mem reads arrive via the mem hook */
        at_val_rec_t r = rd[i];
        df_capture_reg_value(c->uc, c->vt, &r); /* wide XMM or scalar GP */
        recbuf_push(&c->cur, &r);
    }
    for (size_t i = 0; i < nw; i++) {
        if (wr[i].kind != AT_LOC_REG)
            continue; /* mem writes arrive via the mem hook */
        at_val_rec_t r = wr[i];
        r.value_valid = false; /* filled at the next code hook / at stop */
        recbuf_push(&c->cur, &r);
    }
}

/* UC_HOOK_MEM_READ_AFTER (delivers the loaded value) and UC_HOOK_MEM_WRITE: attach
 * an absolute-address memory record to the current step. */
static void df_on_mem(uc_engine *uc, uc_mem_type type, uint64_t addr, int size,
                      int64_t value, void *user) {
    (void)uc;
    df_ctx *c = (df_ctx *)user;
    if (!c->have_cur)
        return;
    at_val_rec_t r;
    memset(&r, 0, sizeof r);
    r.kind = AT_LOC_MEM_ABS;
    r.addr = addr;
    r.size = (uint16_t)(size < 0 ? 0 : size);
    r.is_write = (type == UC_MEM_WRITE);
    if (r.size <= 8) {
        r.value = (uint64_t)value;
        r.value_valid = true;
    }
    recbuf_push(&c->cur, &r);
}

static void df_zero_gp(uc_engine *uc) {
    static const int gp[] = {UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX,
                             UC_X86_REG_RDX, UC_X86_REG_RSI, UC_X86_REG_RDI,
                             UC_X86_REG_RBP, UC_X86_REG_R8,  UC_X86_REG_R9,
                             UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12,
                             UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};
    uint64_t z = 0;
    for (size_t i = 0; i < sizeof gp / sizeof gp[0]; i++)
        uc_reg_write(uc, gp[i], &z);
    uint64_t flags = 2; /* EFLAGS reserved bit 1 */
    uc_reg_write(uc, UC_X86_REG_EFLAGS, &flags);
}

/*
 * Run `code` (x86-64 SysV) under Unicorn and fill *vt with its value trace, with
 * `niargs` integer args (rdi, rsi, rdx, rcx, r8, r9) and — R4 T3 — `nfargs`
 * SSE-class double args (xmm0..7), so an FP/vector-argument routine sees its
 * arguments in the right registers. Returns 0 on a clean run to the sentinel return
 * address, 1 if the guest faulted / errored (a partial trace is still produced), -1
 * on a setup failure. `max_insns` caps the step count (0 = unbounded).
 */
static int df_run_impl(const uint8_t *code, size_t code_len, const long *iargs,
                       int niargs, const double *fargs, int nfargs,
                       uint64_t max_insns, asmtest_valtrace_t *vt) {
    if (vt == NULL || code == NULL)
        return -1;
    vt->mem_space = AT_LOC_MEM_ABS;

    uc_engine *uc = NULL;
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK)
        return -1;
    if (uc_mem_map(uc, DF_CODE_BASE, DF_CODE_SIZE, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_map(uc, DF_STACK_BASE, DF_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) {
        uc_close(uc);
        return -1;
    }
    size_t clen = code_len > DF_CODE_SIZE ? DF_CODE_SIZE : code_len;
    if (uc_mem_write(uc, DF_CODE_BASE, code, clen) != UC_ERR_OK) {
        uc_close(uc);
        return -1;
    }
    df_zero_gp(uc);

    /* SysV: rsp ≡ 8 (mod 16) at entry, sentinel return address on top. */
    static const int arg_regs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI,
                                    UC_X86_REG_RDX, UC_X86_REG_RCX,
                                    UC_X86_REG_R8,  UC_X86_REG_R9};
    uint64_t sp = DF_STACK_BASE + DF_STACK_SIZE - 8;
    uint64_t ret = DF_RET_MAGIC;
    uc_mem_write(uc, sp, &ret, sizeof ret);
    uc_reg_write(uc, UC_X86_REG_RSP, &sp);
    for (int i = 0; i < niargs && i < 6; i++) {
        uint64_t v = (uint64_t)iargs[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }
    /* R4 T3: SSE-class double args into xmm0..7 (the SysV FP arg registers). A
     * fresh uc_open zeroes the vector file, so the high 8 bytes of each xmm stay 0
     * — a scalar double occupies the low lane, exactly emu_call_fp's marshalling. */
    for (int i = 0; i < nfargs && i < 8; i++) {
        uint8_t xv[16] = {0};
        memcpy(xv, &fargs[i], sizeof(double));
        uc_reg_write(uc, UC_X86_REG_XMM0 + i, xv);
    }

    df_ctx c;
    memset(&c, 0, sizeof c);
    c.uc = uc;
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = DF_CODE_BASE;

    uc_hook hcode = 0, hread = 0, hwrite = 0;
    uc_hook_add(uc, &hcode, UC_HOOK_CODE, (void *)df_on_code, &c, 1, 0);
    uc_hook_add(uc, &hread, UC_HOOK_MEM_READ_AFTER, (void *)df_on_mem, &c, 1,
                0);
    uc_hook_add(uc, &hwrite, UC_HOOK_MEM_WRITE, (void *)df_on_mem, &c, 1, 0);

    uc_err err =
        uc_emu_start(uc, DF_CODE_BASE, DF_RET_MAGIC, 0, (size_t)max_insns);
    if (c.have_cur)
        df_finalize(&c); /* last step's deferred writes + append */

    uc_hook_del(uc, hcode);
    uc_hook_del(uc, hread);
    uc_hook_del(uc, hwrite);
    free(c.cur.v);
    uc_close(uc);
    return err == UC_ERR_OK ? 0 : 1;
}

/* The integer-arg producer (the historical signature every caller uses): run `code`
 * with integer args only, no FP/vector marshalling. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt) {
    return df_run_impl(code, code_len, args, nargs, NULL, 0, max_insns, vt);
}

/* R4 T3: the FP/vector-arg producer — additionally marshals `nfargs` SSE-class
 * double args into xmm0..7 (SysV), retiring the "integer-arg only" corpus limit. */
int asmtest_dataflow_emu_run_fp(const uint8_t *code, size_t code_len,
                                const long *iargs, int niargs,
                                const double *fargs, int nfargs,
                                uint64_t max_insns, asmtest_valtrace_t *vt) {
    return df_run_impl(code, code_len, iargs, niargs, fargs, nfargs, max_insns,
                       vt);
}
