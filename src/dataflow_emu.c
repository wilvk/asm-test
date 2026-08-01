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
 * PER-GUEST (R5, docs/internal/archive/gui/32-per-guest-value-producer.md). The guest
 * decisions that were inline x86-64 — the Unicorn open mode, the Capstone->Unicorn
 * register map (scalar AND the wide/XMM read), the operand-enumerator arch tag, the
 * ABI argument registers (integer AND SSE-class FP), the register-file init, and the
 * call/return setup (a stacked return address on x86-64, the link register on
 * AArch64) — are now a `df_guest` descriptor. The run selects one by guest arch and
 * is otherwise arch-neutral, so a second guest is a new df_guest instance, not a
 * rewrite. The x86-64 path is byte-identical to before (asmtrace-golden-check);
 * AArch64 is the first added guest (integer-arg, scalar value fabric — its wide
 * NEON/SVE read and FP-arg marshalling are future seams, left NULL).
 *
 * Requires Unicorn + Capstone (the operand enumerator); built only when libunicorn
 * is present (see mk/dataflow.mk), so the whole file is under that gate.
 */
#include "asmtest_valtrace.h"

#include "asmtest_dataflow_internal.h" /* DF_* layout + the seed/capture seam (R3) */
#include "asmtest_grow.h" /* asmtest_grow / _pow2 — overflow-checked pool growth (S6) */
#include <capstone/capstone.h> /* X86_REG_* / ARM64_REG_* ids from the enumerator */
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* Guest layout (DF_CODE_BASE / DF_CODE_SIZE / DF_STACK_BASE / DF_STACK_SIZE /
 * DF_RET_MAGIC) — matches src/emu.c's constants so behaviour is identical, and is
 * the SHARED seam (asmtest_dataflow_internal.h) the emu_t-hosted producer keys on
 * too. Shared by every guest: Unicorn maps whatever addresses it is told, so the
 * same fixed bases host an x86-64 or an AArch64 routine equally (the code base is
 * where the routine runs; the sentinel return address is where a clean run stops). */

/* A one-step scratch buffer of operand records, finalized + appended when the next
 * step begins (or the run ends). */
typedef struct {
    at_val_rec_t *v;
    size_t n, cap;
} recbuf;

/* The per-guest seam (R5). Everything the run does that is arch-specific is one of
 * these; the run itself is arch-neutral. */
typedef struct df_guest {
    const char *name;    /* chrome: "x86_64" / "arm64" */
    uc_arch arch;        /* Unicorn open arch */
    uc_mode mode;        /* Unicorn open mode */
    asmtest_arch_t op_arch; /* operand-enumerator arch tag (asmtest_operands) */
    /* Map a Capstone register id to its Unicorn id (folding a sub-register to its
     * full-width container), or -1 for a register this guest does not model. */
    int (*cap_to_uc)(uint32_t cap_reg);
    /* Read a WIDE (>8-byte, e.g. 128-bit XMM) register operand into buf[16], or
     * false for a reg this guest has no wide view of. NULL when the guest captures
     * no wide register file (arm64 NEON/SVE is a future seam) — the operand then
     * takes the scalar path or stays value-less, never a foreign-arch misread (the
     * arm64/x86 Capstone reg-id spaces overlap numerically). */
    bool (*read_wide)(uc_engine *uc, uint32_t cap_reg, uint8_t buf[16]);
    const int *arg_regs; /* Unicorn integer arg registers, ABI order */
    int nargs;           /* how many of arg_regs the ABI fills before spilling */
    /* Marshal SSE-class / FP double args into the guest's FP arg registers, or NULL
     * when the guest marshals no FP args (arm64 is integer-arg only here). */
    void (*setup_fp_args)(uc_engine *uc, const double *fargs, int nfargs);
    /* Zero the general-purpose file + init the flags register — the deterministic
     * entry state that makes the output a golden rather than a sample. */
    void (*init_regs)(uc_engine *uc);
    /* Set the stack pointer and arrange the return path so a `ret` reaches
     * `ret_addr` and the run stops: x86-64 pushes it on the stack, AArch64 puts it
     * in the link register. `stack_top` is the top of the mapped stack region. */
    void (*setup_call)(uc_engine *uc, uint64_t stack_top, uint64_t ret_addr);
} df_guest;

typedef struct {
    uc_engine *uc;
    const df_guest *g;
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

/* --- x86-64 guest ---------------------------------------------------------- */

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

/* R4 (31-wide-register-deck.md T3): read a WIDE (128-bit XMM) register operand into
 * `buf` (16 bytes). Matched by REG ID (XMM0..15), not by the operand's reported
 * size — the enumerator leaves register-operand `size` 0 (it sets width only for
 * memory), so a size gate would never fire. A value the GP df_reg_read below cannot
 * express (it returns a u64), so an FP/vector operand's bytes spill to the valtrace
 * `wide[]` side buffer exactly as a >8-byte memory value does (R1 T3 serializes them
 * as the `bytes` hex string). Returns false for a non-XMM reg (the caller then falls
 * back to the scalar GP path, or leaves the operand value-less — faithful). */
static bool df_reg_read_wide(uc_engine *uc, uint32_t cap_reg, uint8_t buf[16]) {
    if (cap_reg < X86_REG_XMM0 || cap_reg > X86_REG_XMM0 + 15)
        return false;
    return uc_reg_read(uc, UC_X86_REG_XMM0 + (int)(cap_reg - X86_REG_XMM0),
                       buf) == UC_ERR_OK;
}

static void df_init_x86(uc_engine *uc) {
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

/* SysV: rsp ≡ 8 (mod 16) at entry, with the sentinel return address on top of the
 * stack (a `ret` pops it into rip and the run stops). */
static void setup_call_x86(uc_engine *uc, uint64_t stack_top, uint64_t ret_addr) {
    uint64_t sp = stack_top - 8;
    uc_mem_write(uc, sp, &ret_addr, sizeof ret_addr);
    uc_reg_write(uc, UC_X86_REG_RSP, &sp);
}

/* R4 T3: SSE-class double args into xmm0..7 (the SysV FP arg registers). A fresh
 * uc_open zeroes the vector file, so the high 8 bytes of each xmm stay 0 — a scalar
 * double occupies the low lane, exactly emu_call_fp's marshalling. */
static void setup_fp_args_x86(uc_engine *uc, const double *fargs, int nfargs) {
    for (int i = 0; i < nfargs && i < 8; i++) {
        uint8_t xv[16] = {0};
        memcpy(xv, &fargs[i], sizeof(double));
        uc_reg_write(uc, UC_X86_REG_XMM0 + i, xv);
    }
}

static const int x86_arg_regs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI,
                                    UC_X86_REG_RDX, UC_X86_REG_RCX,
                                    UC_X86_REG_R8,  UC_X86_REG_R9};

static const df_guest DF_GUEST_X86_64 = {
    "x86_64",          UC_ARCH_X86,      UC_MODE_64,   ASMTEST_ARCH_X86_64,
    cap_x86_to_uc,     df_reg_read_wide, x86_arg_regs, 6,
    setup_fp_args_x86, df_init_x86,      setup_call_x86,
};

/* --- AArch64 guest (R5 T2) -------------------------------------------------- */

/* Map a Capstone arm64 register id to its Unicorn UC_ARM64_REG_* id, folding a
 * 32-bit W view to its 64-bit X container (uc_reg_read returns the full 64 bits;
 * the record's size carries the real operand width). Capstone lists X0..X28 and
 * W0..W30 as contiguous blocks, and X29 == FP / X30 == LR (aliases); Unicorn
 * likewise has X0..X28 contiguous. The zero register (XZR/WZR) reads as 0 and is a
 * discard target, so it is left uncaptured (-1) rather than recorded as a bogus
 * value. Vector/FP registers are not modelled by this integer producer. */
static int cap_arm64_to_uc(uint32_t r) {
    if (r >= ARM64_REG_X0 && r <= ARM64_REG_X28)
        return (int)UC_ARM64_REG_X0 + (int)(r - ARM64_REG_X0);
    if (r >= ARM64_REG_W0 && r <= ARM64_REG_W28)
        return (int)UC_ARM64_REG_X0 + (int)(r - ARM64_REG_W0);
    switch (r) {
    case ARM64_REG_X29: /* == ARM64_REG_FP */
        return UC_ARM64_REG_X29;
    case ARM64_REG_X30: /* == ARM64_REG_LR */
        return UC_ARM64_REG_X30;
    case ARM64_REG_W29:
        return UC_ARM64_REG_X29;
    case ARM64_REG_W30:
        return UC_ARM64_REG_X30;
    case ARM64_REG_SP:
    case ARM64_REG_WSP:
        return UC_ARM64_REG_SP;
    case ARM64_REG_NZCV:
        return UC_ARM64_REG_NZCV;
    default:
        return -1;
    }
}

static void df_init_arm64(uc_engine *uc) {
    uint64_t z = 0;
    for (int r = UC_ARM64_REG_X0; r <= UC_ARM64_REG_X28; r++)
        uc_reg_write(uc, r, &z);
    uc_reg_write(uc, UC_ARM64_REG_X29, &z); /* FP */
    uc_reg_write(uc, UC_ARM64_REG_X30, &z); /* LR */
    uc_reg_write(uc, UC_ARM64_REG_NZCV, &z);
}

/* AAPCS64: SP is 16-byte aligned at entry and the return address is in the link
 * register (x30), not on the stack — a `ret` branches to x30, so pointing it at the
 * sentinel stops the run exactly as the stacked x86-64 return does. */
static void setup_call_arm64(uc_engine *uc, uint64_t stack_top,
                             uint64_t ret_addr) {
    uc_reg_write(uc, UC_ARM64_REG_SP, &stack_top);
    uc_reg_write(uc, UC_ARM64_REG_X30, &ret_addr);
}

static const int arm64_arg_regs[8] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                      UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                      UC_ARM64_REG_X4, UC_ARM64_REG_X5,
                                      UC_ARM64_REG_X6, UC_ARM64_REG_X7};

/* AArch64 captures the scalar integer fabric only: no wide-register read (NEON/SVE
 * is a future seam — NULL, never the x86 XMM read, whose id range would misfire on
 * the numerically-overlapping arm64 reg ids) and no FP-arg marshalling. */
static const df_guest DF_GUEST_ARM64 = {
    "arm64",         UC_ARCH_ARM64,  UC_MODE_ARM,    ASMTEST_ARCH_ARM64,
    cap_arm64_to_uc, NULL,           arm64_arg_regs, 8,
    NULL,            df_init_arm64,  setup_call_arm64,
};

/* Non-static (declared in asmtest_dataflow_internal.h) so the emu_t-hosted producer
 * (src/dataflow_resume.c, R3) selects the same descriptor the standalone run does. */
const df_guest *df_guest_for(asmtest_arch_t arch) {
    switch (arch) {
    case ASMTEST_ARCH_X86_64:
        return &DF_GUEST_X86_64;
    case ASMTEST_ARCH_ARM64:
        return &DF_GUEST_ARM64;
    default:
        return NULL; /* ARM32 / RISCV64: no guest armed (the enumerator stubs them) */
    }
}

/* --- the arch-neutral run -------------------------------------------------- */

static bool df_reg_read(const df_guest *g, uc_engine *uc, uint32_t cap_reg,
                        uint64_t *out) {
    int uc_id = g->cap_to_uc(cap_reg);
    if (uc_id < 0)
        return false;
    uint64_t v = 0;
    if (uc_reg_read(uc, uc_id, &v) != UC_ERR_OK)
        return false;
    *out = v;
    return true;
}

/* Capture register operand `r`'s value from the live guest — the wide (128-bit XMM,
 * x86-64) path spilling to `vt->wide[]` (R4 31 T3), else the scalar GP path inline.
 * On the wide path it stamps `size` to 16 so the serializer emits the full
 * register's bytes (the enumerator left it 0). A guest with no wide register file
 * (arm64) has read_wide == NULL and always takes the scalar path; a reg the value
 * channel cannot read leaves the record value-less (faithful "not measured"). */
static void df_capture_reg_value(const df_guest *g, uc_engine *uc,
                                 asmtest_valtrace_t *vt, at_val_rec_t *r) {
    uint8_t wbuf[16];
    uint64_t val;
    if (g->read_wide && g->read_wide(uc, r->reg, wbuf)) {
        size_t off = asmtest_valtrace_stash_wide(vt, wbuf, 16);
        if (off != (size_t)-1) {
            r->size = 16;
            r->wide = true;
            r->wide_off = (uint32_t)off;
            r->value_valid = true;
        }
    } else if (df_reg_read(g, uc, r->reg, &val)) {
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
            df_capture_reg_value(c->g, c->uc, c->vt, r); /* wide XMM or scalar GP */
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
    asmtest_operands(c->g->op_arch, c->code, c->code_len, off, rd, &nr, wr, &nw);
    for (size_t i = 0; i < nr; i++) {
        if (rd[i].kind != AT_LOC_REG)
            continue; /* mem reads arrive via the mem hook */
        at_val_rec_t r = rd[i];
        df_capture_reg_value(c->g, c->uc, c->vt, &r); /* wide XMM or scalar GP */
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

/*
 * Run `code` under Unicorn as guest `g` and fill *vt with its value trace, with
 * `niargs` integer args (the guest's arg_regs) and — R4 T3, x86-64 only — `nfargs`
 * SSE-class double args (xmm0..7). Returns 0 on a clean run to the sentinel return
 * address, 1 if the guest faulted / errored (a partial trace is still produced), -1
 * on a setup failure. `max_insns` caps the step count (0 = unbounded).
 */
/* Shared seam (asmtest_dataflow_internal.h): seed a fresh entry state for guest `g`
 * on an ALREADY-mapped `uc`. Write the routine bytes (no trap-fill — a fresh
 * Unicorn map is zero-filled), run the guest's deterministic register init +
 * call/return setup, and marshal the integer (and, where the guest supports it, FP)
 * args. Byte-for-byte the standalone setup, now callable on an emu_t's engine too
 * (the R3 hosted producer). `uc` is the opaque struct uc_struct from the header. */
void asmtest_df_seed(const df_guest *g, struct uc_struct *uc_s,
                     const uint8_t *code, size_t code_len, const long *iargs,
                     int niargs, const double *fargs, int nfargs) {
    uc_engine *uc = (uc_engine *)uc_s;
    size_t clen = code_len > DF_CODE_SIZE ? DF_CODE_SIZE : code_len;
    uc_mem_write(uc, DF_CODE_BASE, code, clen);
    g->init_regs(uc);
    g->setup_call(uc, DF_STACK_BASE + DF_STACK_SIZE, DF_RET_MAGIC);
    for (int i = 0; i < niargs && i < g->nargs; i++) {
        uint64_t v = (uint64_t)iargs[i];
        uc_reg_write(uc, g->arg_regs[i], &v);
    }
    if (g->setup_fp_args)
        g->setup_fp_args(uc, fargs, nfargs);
}

/* UC_HOOK_MEM_INVALID: record ONLY THE FIRST invalid access (mirrors
 * src/emu.c's on_invalid_mem/kind_of pattern) and refuse the retry — same
 * convention as emu.c: leave the access invalid and stop. */
static int df_fault_kind_of(uc_mem_type type) {
    switch (type) {
    case UC_MEM_READ_UNMAPPED:
    case UC_MEM_READ_PROT:
        return 1; /* EMU_FAULT_READ */
    case UC_MEM_WRITE_UNMAPPED:
    case UC_MEM_WRITE_PROT:
        return 2; /* EMU_FAULT_WRITE */
    case UC_MEM_FETCH_UNMAPPED:
    case UC_MEM_FETCH_PROT:
        return 3; /* EMU_FAULT_FETCH */
    default:
        return 0; /* EMU_FAULT_NONE */
    }
}

static bool df_on_invalid_mem(uc_engine *uc, uc_mem_type type,
                              uint64_t address, int size, int64_t value,
                              void *user) {
    (void)uc;
    (void)size;
    (void)value;
    df_fault_t *fo = (df_fault_t *)user;
    if (!fo->faulted) {
        fo->faulted = 1;
        fo->addr = address;
        fo->kind = df_fault_kind_of(type);
    }
    return false; /* do not retry: leave the access invalid and stop */
}

/* Shared seam: attach the value-capture hooks, run guest `g` from `start_addr` to
 * the sentinel, finalize the last step, and detach — one engine, whoever owns it.
 * `base` is the offset origin for the step spine (DF_CODE_BASE); `extra_hook` (or
 * NULL) is one more UC_HOOK_CODE the checkpoint path uses to snapshot at a step.
 * `fault_out` (or NULL) reports the first invalid memory access. */
int asmtest_df_capture(const df_guest *g, struct uc_struct *uc_s,
                       const uint8_t *code, size_t code_len, uint64_t base,
                       uint64_t start_addr, uint64_t max_insns,
                       asmtest_valtrace_t *vt, df_code_hook_t extra_hook,
                       void *extra_user, df_fault_t *fault_out) {
    uc_engine *uc = (uc_engine *)uc_s;
    df_ctx c;
    memset(&c, 0, sizeof c);
    c.uc = uc;
    c.g = g;
    c.vt = vt;
    c.code = code;
    c.code_len = code_len;
    c.base = base;

    if (fault_out != NULL)
        memset(fault_out, 0, sizeof *fault_out);

    uc_hook hcode = 0, hread = 0, hwrite = 0, hextra = 0, hinvalid = 0;
    uc_hook_add(uc, &hcode, UC_HOOK_CODE, (void *)df_on_code, &c, 1, 0);
    uc_hook_add(uc, &hread, UC_HOOK_MEM_READ_AFTER, (void *)df_on_mem, &c, 1,
                0);
    uc_hook_add(uc, &hwrite, UC_HOOK_MEM_WRITE, (void *)df_on_mem, &c, 1, 0);
    if (extra_hook != NULL)
        uc_hook_add(uc, &hextra, UC_HOOK_CODE, (void *)extra_hook, extra_user,
                    1, 0);
    if (fault_out != NULL)
        uc_hook_add(uc, &hinvalid, UC_HOOK_MEM_INVALID,
                    (void *)df_on_invalid_mem, fault_out, 1, 0);

    uc_err err = uc_emu_start(uc, start_addr, DF_RET_MAGIC, 0, (size_t)max_insns);
    if (c.have_cur)
        df_finalize(&c); /* last step's deferred writes + append */

    uc_hook_del(uc, hcode);
    uc_hook_del(uc, hread);
    uc_hook_del(uc, hwrite);
    if (hextra)
        uc_hook_del(uc, hextra);
    if (fault_out != NULL)
        uc_hook_del(uc, hinvalid);
    free(c.cur.v);
    return err == UC_ERR_OK ? 0 : 1;
}

static int df_run_impl(const df_guest *g, const uint8_t *code, size_t code_len,
                       const long *iargs, int niargs, const double *fargs,
                       int nfargs, uint64_t max_insns, asmtest_valtrace_t *vt) {
    if (vt == NULL || code == NULL || g == NULL)
        return -1;
    vt->mem_space = AT_LOC_MEM_ABS;

    uc_engine *uc = NULL;
    if (uc_open(g->arch, g->mode, &uc) != UC_ERR_OK)
        return -1;
    if (uc_mem_map(uc, DF_CODE_BASE, DF_CODE_SIZE, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_map(uc, DF_STACK_BASE, DF_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) {
        uc_close(uc);
        return -1;
    }
    asmtest_df_seed(g, uc, code, code_len, iargs, niargs, fargs, nfargs);
    int rc = asmtest_df_capture(g, uc, code, code_len, DF_CODE_BASE,
                                DF_CODE_BASE, max_insns, vt, NULL, NULL, NULL);
    uc_close(uc);
    return rc;
}

/* The integer-arg x86-64 producer (the historical signature every caller uses):
 * run `code` with integer args only, no FP/vector marshalling. */
int asmtest_dataflow_emu_run(const uint8_t *code, size_t code_len,
                             const long *args, int nargs, uint64_t max_insns,
                             asmtest_valtrace_t *vt) {
    return df_run_impl(&DF_GUEST_X86_64, code, code_len, args, nargs, NULL, 0,
                       max_insns, vt);
}

/* R4 T3: the FP/vector-arg x86-64 producer — additionally marshals `nfargs`
 * SSE-class double args into xmm0..7 (SysV), retiring the "integer-arg only" corpus
 * limit. */
int asmtest_dataflow_emu_run_fp(const uint8_t *code, size_t code_len,
                                const long *iargs, int niargs,
                                const double *fargs, int nfargs,
                                uint64_t max_insns, asmtest_valtrace_t *vt) {
    return df_run_impl(&DF_GUEST_X86_64, code, code_len, iargs, niargs, fargs,
                       nfargs, max_insns, vt);
}

/* R5: the per-guest entry — run `code` through the guest for `arch` (integer args
 * only; FP-arg marshalling is a per-guest capability the x86-64 _fp entry above
 * exposes). Returns -1 for an arch with no armed guest. asmtest_dataflow_emu_run is
 * exactly this pinned to x86-64. */
int asmtest_dataflow_emu_run_arch(asmtest_arch_t arch, const uint8_t *code,
                                  size_t code_len, const long *args, int nargs,
                                  uint64_t max_insns, asmtest_valtrace_t *vt) {
    return df_run_impl(df_guest_for(arch), code, code_len, args, nargs, NULL, 0,
                       max_insns, vt);
}
