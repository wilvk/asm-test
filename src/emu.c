/*
 * emu.c — Unicorn-Engine-backed emulator tier (Phase 4+), with x86-64,
 * AArch64, RISC-V (RV64), and ARM32 guests.
 */
#include "asmtest_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* Guest memory layout (arbitrary, just needs to be mapped and non-overlapping). */
#define EMU_CODE_BASE  0x00100000UL
#define EMU_CODE_SIZE  0x00010000UL /* 64 KiB of code space  */
#define EMU_STACK_BASE 0x00200000UL
#define EMU_STACK_SIZE 0x00010000UL /* 64 KiB stack          */
#define EMU_RET_MAGIC  0x00f00000UL /* return target; unmapped, only a stop addr */

struct emu {
    uc_engine *uc;
};

typedef struct {
    bool faulted;
    uint64_t addr;
    emu_fault_kind_t kind;
} fault_rec_t;

static emu_fault_kind_t kind_of(uc_mem_type type) {
    switch (type) {
    case UC_MEM_READ_UNMAPPED:
    case UC_MEM_READ_PROT:
        return EMU_FAULT_READ;
    case UC_MEM_WRITE_UNMAPPED:
    case UC_MEM_WRITE_PROT:
        return EMU_FAULT_WRITE;
    case UC_MEM_FETCH_UNMAPPED:
    case UC_MEM_FETCH_PROT:
        return EMU_FAULT_FETCH;
    default:
        return EMU_FAULT_NONE;
    }
}

static bool on_invalid_mem(uc_engine *uc, uc_mem_type type, uint64_t address,
                           int size, int64_t value, void *user) {
    (void)uc;
    (void)size;
    (void)value;
    fault_rec_t *fr = (fault_rec_t *)user;
    if (!fr->faulted) {
        fr->faulted = true;
        fr->addr = address;
        fr->kind = kind_of(type);
    }
    return false; /* do not retry: leave the access invalid and stop */
}

/* Tracing context: where to record, and the base to subtract for offsets.
 * Shared verbatim by the x86-64 and AArch64 guests (offsets are arch-neutral). */
typedef struct {
    emu_trace_t *t;
    uint64_t base;
} trace_ctx_t;

/* UC_HOOK_CODE: fires once per executed instruction — the ordered trace. */
static void on_code(uc_engine *uc, uint64_t address, uint32_t size,
                    void *user) {
    (void)uc;
    (void)size;
    trace_ctx_t *c = (trace_ctx_t *)user;
    emu_trace_t *t = c->t;
    uint64_t off = address - c->base;
    if (t->insns != NULL) {
        if (t->insns_len < t->insns_cap)
            t->insns[t->insns_len++] = off;
        else
            t->truncated = true;
    }
    t->insns_total++;
}

/* UC_HOOK_BLOCK: fires at each basic-block entry — the coverage set. blocks[]
 * holds the DISTINCT starts (loops re-enter the same block; counted only in
 * blocks_total). The dedup scan is linear, fine for the small block counts of
 * a routine under test. */
static void on_block(uc_engine *uc, uint64_t address, uint32_t size,
                     void *user) {
    (void)uc;
    (void)size;
    trace_ctx_t *c = (trace_ctx_t *)user;
    emu_trace_t *t = c->t;
    uint64_t off = address - c->base;
    t->blocks_total++;
    if (t->blocks != NULL) {
        for (size_t i = 0; i < t->blocks_len; i++)
            if (t->blocks[i] == off)
                return; /* already covered */
        if (t->blocks_len < t->blocks_cap)
            t->blocks[t->blocks_len++] = off;
        else
            t->truncated = true;
    }
}

/* Add the trace hooks if tracing is requested; returns the two hook handles via
 * out-params (0 when tracing is off, so deleting them is harmless). */
static void add_trace_hooks(uc_engine *uc, trace_ctx_t *tc, uc_hook *hcode,
                            uc_hook *hblock) {
    *hcode = 0;
    *hblock = 0;
    if (tc->t == NULL)
        return;
    uc_hook_add(uc, hcode, UC_HOOK_CODE, (void *)on_code, tc, 1, 0);
    uc_hook_add(uc, hblock, UC_HOOK_BLOCK, (void *)on_block, tc, 1, 0);
}

/* Load a routine's bytes at EMU_CODE_BASE and flush Unicorn's translation-block
 * cache for it. The flush matters when an emulator handle is reused for a
 * different routine at the same address: without it, Unicorn would re-run the
 * previously translated (now stale) code instead of the bytes just written. */
static bool load_code(uc_engine *uc, const void *code, size_t code_len) {
    if (uc_mem_write(uc, EMU_CODE_BASE, code, code_len) != UC_ERR_OK)
        return false;
    uc_ctl_flush_tb(uc);
    return true;
}

emu_t *emu_open(void) {
    emu_t *e = (emu_t *)calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    if (uc_open(UC_ARCH_X86, UC_MODE_64, &e->uc) != UC_ERR_OK) {
        free(e);
        return NULL;
    }
    if (uc_mem_map(e->uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL) !=
            UC_ERR_OK ||
        uc_mem_map(e->uc, EMU_STACK_BASE, EMU_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) {
        uc_close(e->uc);
        free(e);
        return NULL;
    }
    return e;
}

void emu_close(emu_t *e) {
    if (e == NULL)
        return;
    uc_close(e->uc);
    free(e);
}

bool emu_map(emu_t *e, uint64_t addr, size_t size) {
    return uc_mem_map(e->uc, addr, size, UC_PROT_READ | UC_PROT_WRITE) ==
           UC_ERR_OK;
}

bool emu_write(emu_t *e, uint64_t addr, const void *data, size_t len) {
    return uc_mem_write(e->uc, addr, data, len) == UC_ERR_OK;
}

bool emu_read(emu_t *e, uint64_t addr, void *data, size_t len) {
    return uc_mem_read(e->uc, addr, data, len) == UC_ERR_OK;
}

static void read_all_regs(uc_engine *uc, emu_x86_regs_t *r) {
    uc_reg_read(uc, UC_X86_REG_RAX, &r->rax);
    uc_reg_read(uc, UC_X86_REG_RBX, &r->rbx);
    uc_reg_read(uc, UC_X86_REG_RCX, &r->rcx);
    uc_reg_read(uc, UC_X86_REG_RDX, &r->rdx);
    uc_reg_read(uc, UC_X86_REG_RSI, &r->rsi);
    uc_reg_read(uc, UC_X86_REG_RDI, &r->rdi);
    uc_reg_read(uc, UC_X86_REG_RBP, &r->rbp);
    uc_reg_read(uc, UC_X86_REG_RSP, &r->rsp);
    uc_reg_read(uc, UC_X86_REG_R8, &r->r8);
    uc_reg_read(uc, UC_X86_REG_R9, &r->r9);
    uc_reg_read(uc, UC_X86_REG_R10, &r->r10);
    uc_reg_read(uc, UC_X86_REG_R11, &r->r11);
    uc_reg_read(uc, UC_X86_REG_R12, &r->r12);
    uc_reg_read(uc, UC_X86_REG_R13, &r->r13);
    uc_reg_read(uc, UC_X86_REG_R14, &r->r14);
    uc_reg_read(uc, UC_X86_REG_R15, &r->r15);
    uc_reg_read(uc, UC_X86_REG_RIP, &r->rip);
    uc_reg_read(uc, UC_X86_REG_EFLAGS, &r->rflags);
    for (int i = 0; i < 16; i++) /* xmm0..15 are consecutive enum values */
        uc_reg_read(uc, UC_X86_REG_XMM0 + i, r->xmm[i].u8);
}

/* Shared System V setup: copy the routine in, plant the sentinel return address
 * on a 16-aligned-after-call stack, and load the integer args into the GP
 * argument registers. Returns false only if the code write fails. */
static bool emu_x86_setup_sysv(uc_engine *uc, const void *fn, size_t code_len,
                               const long *iargs, int niargs) {
    static const int arg_regs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI,
                                    UC_X86_REG_RDX, UC_X86_REG_RCX,
                                    UC_X86_REG_R8,  UC_X86_REG_R9};
    if (!load_code(uc, fn, code_len))
        return false;
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 8;
    uint64_t ret = EMU_RET_MAGIC;
    uc_mem_write(uc, sp, &ret, sizeof ret);
    uc_reg_write(uc, UC_X86_REG_RSP, &sp);
    for (int i = 0; i < niargs && i < 6; i++) {
        uint64_t v = (uint64_t)iargs[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }
    return true;
}

/* Shared x86-64 run-and-capture: registers and stack are already set up by the
 * caller (per the chosen ABI); this installs the fault/trace hooks, runs to the
 * sentinel return address, and reads back the full register file into *out. */
static bool emu_x86_run(uc_engine *uc, uint64_t max_insns, emu_result_t *out,
                        emu_trace_t *trace) {
    fault_rec_t fr = {0};
    uc_hook hh;
    uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void *)on_invalid_mem, &fr, 1,
                0);
    trace_ctx_t tc = {trace, EMU_CODE_BASE};
    uc_hook hcode, hblock;
    add_trace_hooks(uc, &tc, &hcode, &hblock);

    uc_err err =
        uc_emu_start(uc, EMU_CODE_BASE, EMU_RET_MAGIC, 0, (size_t)max_insns);
    uc_hook_del(uc, hh);
    if (trace != NULL) {
        uc_hook_del(uc, hcode);
        uc_hook_del(uc, hblock);
    }

    out->uc_err = (int)err;
    out->faulted = fr.faulted;
    out->fault_addr = fr.addr;
    out->fault_kind = fr.kind;
    read_all_regs(uc, &out->regs);
    out->ok = (err == UC_ERR_OK) && !fr.faulted;
    return out->ok;
}

bool emu_call_traced(emu_t *e, const void *fn, size_t code_len,
                     const long *args, int nargs, uint64_t max_insns,
                     emu_result_t *out, emu_trace_t *trace) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_x86_setup_sysv(uc, fn, code_len, args, nargs)) {
        out->uc_err = -1;
        return false;
    }
    return emu_x86_run(uc, max_insns, out, trace);
}

bool emu_call(emu_t *e, const void *fn, size_t code_len, const long *args,
              int nargs, uint64_t max_insns, emu_result_t *out) {
    return emu_call_traced(e, fn, code_len, args, nargs, max_insns, out, NULL);
}

bool emu_call_fp(emu_t *e, const void *fn, size_t code_len, const long *iargs,
                 int niargs, const double *fargs, int nfargs,
                 uint64_t max_insns, emu_result_t *out) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_x86_setup_sysv(uc, fn, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nfargs && i < 8; i++) {
        unsigned char x[16] = {0}; /* a double occupies the low 8 bytes of xmmN */
        memcpy(x, &fargs[i], sizeof(double));
        uc_reg_write(uc, UC_X86_REG_XMM0 + i, x);
    }
    return emu_x86_run(uc, max_insns, out, NULL);
}

bool emu_call_vec(emu_t *e, const void *fn, size_t code_len, const long *iargs,
                  int niargs, const emu_vec128_t *vargs, int nvargs,
                  uint64_t max_insns, emu_result_t *out) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_x86_setup_sysv(uc, fn, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nvargs && i < 8; i++)
        uc_reg_write(uc, UC_X86_REG_XMM0 + i, (void *)vargs[i].u8);
    return emu_x86_run(uc, max_insns, out, NULL);
}

/* Microsoft x64 ("Win64") calling convention on the same x86-64 engine. Differs
 * from System V in three ways exercised here: integer args go in rcx, rdx, r8,
 * r9 (not rdi, rsi, ...); the caller reserves 32 bytes of "shadow space" above
 * the return address before any stack args (so the 5th arg sits at [rsp+40] on
 * entry, not [rsp+8]); and rsi/rdi join the nonvolatile set (rbx, rbp, rsi,
 * rdi, r12-r15) instead of being argument/volatile registers. */
bool emu_call_win64_traced(emu_t *e, const void *fn, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_result_t *out, emu_trace_t *trace) {
    static const int arg_regs[4] = {UC_X86_REG_RCX, UC_X86_REG_RDX,
                                    UC_X86_REG_R8, UC_X86_REG_R9};
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);

    if (!load_code(uc, fn, code_len)) {
        out->uc_err = -1;
        return false;
    }

    /* Frame from rsp on entry: [rsp]=retaddr, [rsp+8..39]=shadow space,
     * [rsp+40+8*i]=stack arg (4+i). Pick rsp so it is 8 (mod 16) on entry, as
     * after a real `call` from a 16-aligned site. */
    int stack_args = nargs > 4 ? nargs - 4 : 0;
    uint64_t need = 8 /*retaddr*/ + 32 /*shadow*/ + 8 * (uint64_t)stack_args;
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - need;
    sp -= (sp - 8) % 16; /* move sp down until sp % 16 == 8 */

    uint64_t ret = EMU_RET_MAGIC;
    uc_mem_write(uc, sp, &ret, sizeof ret);
    for (int i = 0; i < stack_args; i++) {
        uint64_t v = (uint64_t)args[4 + i];
        uc_mem_write(uc, sp + 40 + 8 * (uint64_t)i, &v, sizeof v);
    }
    uc_reg_write(uc, UC_X86_REG_RSP, &sp);

    for (int i = 0; i < nargs && i < 4; i++) {
        uint64_t v = (uint64_t)args[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }

    return emu_x86_run(uc, max_insns, out, trace);
}

bool emu_call_win64(emu_t *e, const void *fn, size_t code_len, const long *args,
                    int nargs, uint64_t max_insns, emu_result_t *out) {
    return emu_call_win64_traced(e, fn, code_len, args, nargs, max_insns, out,
                                 NULL);
}

/* ------------------------------------------------------------------ */
/* AArch64 guest                                                       */
/* ------------------------------------------------------------------ */

struct emu_arm64 {
    uc_engine *uc;
};

emu_arm64_t *emu_arm64_open(void) {
    emu_arm64_t *e = (emu_arm64_t *)calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &e->uc) != UC_ERR_OK) {
        free(e);
        return NULL;
    }
    if (uc_mem_map(e->uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL) !=
            UC_ERR_OK ||
        uc_mem_map(e->uc, EMU_STACK_BASE, EMU_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) {
        uc_close(e->uc);
        free(e);
        return NULL;
    }
    return e;
}

void emu_arm64_close(emu_arm64_t *e) {
    if (e == NULL)
        return;
    uc_close(e->uc);
    free(e);
}

bool emu_arm64_map(emu_arm64_t *e, uint64_t addr, size_t size) {
    return uc_mem_map(e->uc, addr, size, UC_PROT_READ | UC_PROT_WRITE) ==
           UC_ERR_OK;
}

bool emu_arm64_write(emu_arm64_t *e, uint64_t addr, const void *data,
                     size_t len) {
    return uc_mem_write(e->uc, addr, data, len) == UC_ERR_OK;
}

bool emu_arm64_read(emu_arm64_t *e, uint64_t addr, void *data, size_t len) {
    return uc_mem_read(e->uc, addr, data, len) == UC_ERR_OK;
}

static void read_all_regs_arm64(uc_engine *uc, emu_arm64_regs_t *r) {
    for (int i = 0; i <= 28; i++) /* x0..x28 are consecutive enum values */
        uc_reg_read(uc, UC_ARM64_REG_X0 + i, &r->x[i]);
    uc_reg_read(uc, UC_ARM64_REG_X29, &r->x[29]);
    uc_reg_read(uc, UC_ARM64_REG_X30, &r->x[30]);
    uc_reg_read(uc, UC_ARM64_REG_SP, &r->sp);
    uc_reg_read(uc, UC_ARM64_REG_PC, &r->pc);
    uc_reg_read(uc, UC_ARM64_REG_NZCV, &r->nzcv);
    for (int i = 0; i < 32; i++) /* v0..v31 are consecutive enum values */
        uc_reg_read(uc, UC_ARM64_REG_V0 + i, r->v[i].u8);
}

/* Shared AAPCS64 setup: code + flush, sentinel lr, 16-aligned sp, integer args
 * in x0..x5. Returns false only on a code-write failure. */
static bool emu_arm64_setup(uc_engine *uc, const void *code, size_t code_len,
                            const long *iargs, int niargs) {
    static const int arg_regs[6] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                    UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                    UC_ARM64_REG_X4, UC_ARM64_REG_X5};
    if (!load_code(uc, code, code_len))
        return false;
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 16; /* 16-aligned */
    uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
    uint64_t lr = EMU_RET_MAGIC; /* `ret` branches to lr */
    uc_reg_write(uc, UC_ARM64_REG_X30, &lr);
    for (int i = 0; i < niargs && i < 6; i++) {
        uint64_t v = (uint64_t)iargs[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }
    return true;
}

/* Shared AArch64 run-and-capture (registers/stack already set up). */
static bool emu_arm64_run(uc_engine *uc, uint64_t max_insns,
                          emu_arm64_result_t *out, emu_trace_t *trace) {
    fault_rec_t fr = {0};
    uc_hook hh;
    uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void *)on_invalid_mem, &fr, 1,
                0);
    trace_ctx_t tc = {trace, EMU_CODE_BASE};
    uc_hook hcode, hblock;
    add_trace_hooks(uc, &tc, &hcode, &hblock);

    uc_err err =
        uc_emu_start(uc, EMU_CODE_BASE, EMU_RET_MAGIC, 0, (size_t)max_insns);
    uc_hook_del(uc, hh);
    if (trace != NULL) {
        uc_hook_del(uc, hcode);
        uc_hook_del(uc, hblock);
    }

    out->uc_err = (int)err;
    out->faulted = fr.faulted;
    out->fault_addr = fr.addr;
    out->fault_kind = fr.kind;
    read_all_regs_arm64(uc, &out->regs);
    out->ok = (err == UC_ERR_OK) && !fr.faulted;
    return out->ok;
}

bool emu_arm64_call_traced(emu_arm64_t *e, const void *code, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_arm64_result_t *out, emu_trace_t *trace) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_arm64_setup(uc, code, code_len, args, nargs)) {
        out->uc_err = -1;
        return false;
    }
    return emu_arm64_run(uc, max_insns, out, trace);
}

bool emu_arm64_call(emu_arm64_t *e, const void *code, size_t code_len,
                    const long *args, int nargs, uint64_t max_insns,
                    emu_arm64_result_t *out) {
    return emu_arm64_call_traced(e, code, code_len, args, nargs, max_insns, out,
                                 NULL);
}

bool emu_arm64_call_fp(emu_arm64_t *e, const void *code, size_t code_len,
                       const long *iargs, int niargs, const double *fargs,
                       int nfargs, uint64_t max_insns,
                       emu_arm64_result_t *out) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_arm64_setup(uc, code, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nfargs && i < 8; i++) {
        unsigned char x[16] = {0}; /* a double occupies the low 8 bytes of d_i */
        memcpy(x, &fargs[i], sizeof(double));
        uc_reg_write(uc, UC_ARM64_REG_V0 + i, x);
    }
    return emu_arm64_run(uc, max_insns, out, NULL);
}

bool emu_arm64_call_vec(emu_arm64_t *e, const void *code, size_t code_len,
                        const long *iargs, int niargs,
                        const emu_vec128_t *vargs, int nvargs,
                        uint64_t max_insns, emu_arm64_result_t *out) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_arm64_setup(uc, code, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nvargs && i < 8; i++)
        uc_reg_write(uc, UC_ARM64_REG_V0 + i, (void *)vargs[i].u8);
    return emu_arm64_run(uc, max_insns, out, NULL);
}

/* ------------------------------------------------------------------ */
/* RISC-V (RV64) guest                                                 */
/* ------------------------------------------------------------------ */

struct emu_riscv {
    uc_engine *uc;
};

emu_riscv_t *emu_riscv_open(void) {
    emu_riscv_t *e = (emu_riscv_t *)calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    if (uc_open(UC_ARCH_RISCV, UC_MODE_RISCV64, &e->uc) != UC_ERR_OK) {
        free(e);
        return NULL;
    }
    /* Unlike the x86-64/AArch64 backends, Unicorn's RISC-V core fetches the
     * instruction at the `until` address before honoring the stop, so an
     * unmapped EMU_RET_MAGIC would fault on the `ret`. Map a small read/exec
     * landing page there; uc_emu_start still stops before executing it. */
    if (uc_mem_map(e->uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL) !=
            UC_ERR_OK ||
        uc_mem_map(e->uc, EMU_STACK_BASE, EMU_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK ||
        uc_mem_map(e->uc, EMU_RET_MAGIC, 0x1000,
                   UC_PROT_READ | UC_PROT_EXEC) != UC_ERR_OK) {
        uc_close(e->uc);
        free(e);
        return NULL;
    }
    /* Enable the F/D floating-point unit: RISC-V starts with mstatus.FS = Off,
     * so an FP instruction would trap. Set FS (bits 14:13) to Dirty (11). */
    uint64_t mstatus = 0;
    uc_reg_read(e->uc, UC_RISCV_REG_MSTATUS, &mstatus);
    mstatus |= (uint64_t)0x6000;
    uc_reg_write(e->uc, UC_RISCV_REG_MSTATUS, &mstatus);
    return e;
}

void emu_riscv_close(emu_riscv_t *e) {
    if (e == NULL)
        return;
    uc_close(e->uc);
    free(e);
}

bool emu_riscv_map(emu_riscv_t *e, uint64_t addr, size_t size) {
    return uc_mem_map(e->uc, addr, size, UC_PROT_READ | UC_PROT_WRITE) ==
           UC_ERR_OK;
}

bool emu_riscv_write(emu_riscv_t *e, uint64_t addr, const void *data,
                     size_t len) {
    return uc_mem_write(e->uc, addr, data, len) == UC_ERR_OK;
}

bool emu_riscv_read(emu_riscv_t *e, uint64_t addr, void *data, size_t len) {
    return uc_mem_read(e->uc, addr, data, len) == UC_ERR_OK;
}

static void read_all_regs_riscv(uc_engine *uc, emu_riscv_regs_t *r) {
    for (int i = 0; i <= 31; i++) /* x0..x31 are consecutive enum values */
        uc_reg_read(uc, UC_RISCV_REG_X0 + i, &r->x[i]);
    uc_reg_read(uc, UC_RISCV_REG_PC, &r->pc);
    for (int i = 0; i < 32; i++) /* f0..f31 (D ext, 64-bit) */
        uc_reg_read(uc, UC_RISCV_REG_F0 + i, &r->f[i].u64[0]);
}

/* Shared RISC-V setup: code + flush, sentinel ra, 16-aligned sp, integer args
 * in a0..a7 (x10..x17). Returns false only on a code-write failure. */
static bool emu_riscv_setup(uc_engine *uc, const void *code, size_t code_len,
                            const long *iargs, int niargs) {
    static const int arg_regs[8] = {
        UC_RISCV_REG_X10, UC_RISCV_REG_X11, UC_RISCV_REG_X12, UC_RISCV_REG_X13,
        UC_RISCV_REG_X14, UC_RISCV_REG_X15, UC_RISCV_REG_X16, UC_RISCV_REG_X17};
    if (!load_code(uc, code, code_len))
        return false;
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 16; /* 16-aligned */
    uc_reg_write(uc, UC_RISCV_REG_SP, &sp);
    uint64_t ra = EMU_RET_MAGIC; /* `ret` is jalr x0, 0(ra) */
    uc_reg_write(uc, UC_RISCV_REG_RA, &ra);
    for (int i = 0; i < niargs && i < 8; i++) {
        uint64_t v = (uint64_t)iargs[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }
    return true;
}

/* Shared RISC-V run-and-capture (registers/stack already set up). */
static bool emu_riscv_run(uc_engine *uc, uint64_t max_insns,
                          emu_riscv_result_t *out, emu_trace_t *trace) {
    fault_rec_t fr = {0};
    uc_hook hh;
    uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void *)on_invalid_mem, &fr, 1,
                0);
    trace_ctx_t tc = {trace, EMU_CODE_BASE};
    uc_hook hcode, hblock;
    add_trace_hooks(uc, &tc, &hcode, &hblock);

    uc_err err =
        uc_emu_start(uc, EMU_CODE_BASE, EMU_RET_MAGIC, 0, (size_t)max_insns);
    uc_hook_del(uc, hh);
    if (trace != NULL) {
        uc_hook_del(uc, hcode);
        uc_hook_del(uc, hblock);
    }

    out->uc_err = (int)err;
    out->faulted = fr.faulted;
    out->fault_addr = fr.addr;
    out->fault_kind = fr.kind;
    read_all_regs_riscv(uc, &out->regs);
    out->ok = (err == UC_ERR_OK) && !fr.faulted;
    return out->ok;
}

bool emu_riscv_call_traced(emu_riscv_t *e, const void *code, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_riscv_result_t *out, emu_trace_t *trace) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_riscv_setup(uc, code, code_len, args, nargs)) {
        out->uc_err = -1;
        return false;
    }
    return emu_riscv_run(uc, max_insns, out, trace);
}

bool emu_riscv_call(emu_riscv_t *e, const void *code, size_t code_len,
                    const long *args, int nargs, uint64_t max_insns,
                    emu_riscv_result_t *out) {
    return emu_riscv_call_traced(e, code, code_len, args, nargs, max_insns, out,
                                 NULL);
}

bool emu_riscv_call_fp(emu_riscv_t *e, const void *code, size_t code_len,
                       const long *iargs, int niargs, const double *fargs,
                       int nfargs, uint64_t max_insns,
                       emu_riscv_result_t *out) {
    /* FP args go in fa0..fa7 == f10..f17. */
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_riscv_setup(uc, code, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nfargs && i < 8; i++)
        uc_reg_write(uc, UC_RISCV_REG_F10 + i, &fargs[i]);
    return emu_riscv_run(uc, max_insns, out, NULL);
}

/* ------------------------------------------------------------------ */
/* ARM32 (A32) guest                                                   */
/* ------------------------------------------------------------------ */

struct emu_arm {
    uc_engine *uc;
};

emu_arm_t *emu_arm_open(void) {
    emu_arm_t *e = (emu_arm_t *)calloc(1, sizeof *e);
    if (e == NULL)
        return NULL;
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &e->uc) != UC_ERR_OK) {
        free(e);
        return NULL;
    }
    if (uc_mem_map(e->uc, EMU_CODE_BASE, EMU_CODE_SIZE, UC_PROT_ALL) !=
            UC_ERR_OK ||
        uc_mem_map(e->uc, EMU_STACK_BASE, EMU_STACK_SIZE,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK) {
        uc_close(e->uc);
        free(e);
        return NULL;
    }
    /* Enable the VFP/NEON unit, off by default on this core: grant CP10/CP11
     * access (CPACR) and set FPEXC.EN, so a VFP instruction does not trap. */
    uint32_t cpacr = 0;
    uc_reg_read(e->uc, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= 0x00F00000; /* full access to CP10 and CP11 */
    uc_reg_write(e->uc, UC_ARM_REG_C1_C0_2, &cpacr);
    uint32_t fpexc = 0x40000000; /* FPEXC.EN */
    uc_reg_write(e->uc, UC_ARM_REG_FPEXC, &fpexc);
    return e;
}

void emu_arm_close(emu_arm_t *e) {
    if (e == NULL)
        return;
    uc_close(e->uc);
    free(e);
}

bool emu_arm_map(emu_arm_t *e, uint64_t addr, size_t size) {
    return uc_mem_map(e->uc, addr, size, UC_PROT_READ | UC_PROT_WRITE) ==
           UC_ERR_OK;
}

bool emu_arm_write(emu_arm_t *e, uint64_t addr, const void *data, size_t len) {
    return uc_mem_write(e->uc, addr, data, len) == UC_ERR_OK;
}

bool emu_arm_read(emu_arm_t *e, uint64_t addr, void *data, size_t len) {
    return uc_mem_read(e->uc, addr, data, len) == UC_ERR_OK;
}

static void read_all_regs_arm(uc_engine *uc, emu_arm_regs_t *r) {
    for (int i = 0; i <= 12; i++) /* r0..r12 are consecutive enum values */
        uc_reg_read(uc, UC_ARM_REG_R0 + i, &r->r[i]);
    uc_reg_read(uc, UC_ARM_REG_SP, &r->r[13]);
    uc_reg_read(uc, UC_ARM_REG_LR, &r->r[14]);
    uc_reg_read(uc, UC_ARM_REG_PC, &r->r[15]);
    uc_reg_read(uc, UC_ARM_REG_CPSR, &r->cpsr);
    /* d0..d31 (64-bit), packed two-per-q: d(2k) -> q[k].f64[0], d(2k+1) -> .f64[1]. */
    for (int i = 0; i < 32; i++)
        uc_reg_read(uc, UC_ARM_REG_D0 + i, &r->q[i / 2].f64[i % 2]);
}

/* Shared AAPCS setup: code + flush, sentinel lr, 8-aligned sp, integer args in
 * r0..r3 (32-bit). Returns false only on a code-write failure. */
static bool emu_arm_setup(uc_engine *uc, const void *code, size_t code_len,
                          const long *iargs, int niargs) {
    static const int arg_regs[4] = {UC_ARM_REG_R0, UC_ARM_REG_R1,
                                    UC_ARM_REG_R2, UC_ARM_REG_R3};
    if (!load_code(uc, code, code_len))
        return false;
    uint32_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 16; /* 8-aligned (AAPCS) */
    uc_reg_write(uc, UC_ARM_REG_SP, &sp);
    uint32_t lr = EMU_RET_MAGIC; /* `bx lr` branches to lr */
    uc_reg_write(uc, UC_ARM_REG_LR, &lr);
    for (int i = 0; i < niargs && i < 4; i++) {
        uint32_t v = (uint32_t)iargs[i]; /* ARM GP regs are 32-bit */
        uc_reg_write(uc, arg_regs[i], &v);
    }
    return true;
}

/* Shared ARM32 run-and-capture (registers/stack already set up). */
static bool emu_arm_run(uc_engine *uc, uint64_t max_insns,
                        emu_arm_result_t *out, emu_trace_t *trace) {
    fault_rec_t fr = {0};
    uc_hook hh;
    uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void *)on_invalid_mem, &fr, 1,
                0);
    trace_ctx_t tc = {trace, EMU_CODE_BASE};
    uc_hook hcode, hblock;
    add_trace_hooks(uc, &tc, &hcode, &hblock);

    uc_err err =
        uc_emu_start(uc, EMU_CODE_BASE, EMU_RET_MAGIC, 0, (size_t)max_insns);
    uc_hook_del(uc, hh);
    if (trace != NULL) {
        uc_hook_del(uc, hcode);
        uc_hook_del(uc, hblock);
    }

    out->uc_err = (int)err;
    out->faulted = fr.faulted;
    out->fault_addr = fr.addr;
    out->fault_kind = fr.kind;
    read_all_regs_arm(uc, &out->regs);
    out->ok = (err == UC_ERR_OK) && !fr.faulted;
    return out->ok;
}

bool emu_arm_call_traced(emu_arm_t *e, const void *code, size_t code_len,
                         const long *args, int nargs, uint64_t max_insns,
                         emu_arm_result_t *out, emu_trace_t *trace) {
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_arm_setup(uc, code, code_len, args, nargs)) {
        out->uc_err = -1;
        return false;
    }
    return emu_arm_run(uc, max_insns, out, trace);
}

bool emu_arm_call(emu_arm_t *e, const void *code, size_t code_len,
                  const long *args, int nargs, uint64_t max_insns,
                  emu_arm_result_t *out) {
    return emu_arm_call_traced(e, code, code_len, args, nargs, max_insns, out,
                               NULL);
}

bool emu_arm_call_fp(emu_arm_t *e, const void *code, size_t code_len,
                     const long *iargs, int niargs, const double *fargs,
                     int nfargs, uint64_t max_insns, emu_arm_result_t *out) {
    /* AAPCS-VFP: double args go in d0..d7. */
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_arm_setup(uc, code, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nfargs && i < 8; i++)
        uc_reg_write(uc, UC_ARM_REG_D0 + i, &fargs[i]);
    return emu_arm_run(uc, max_insns, out, NULL);
}

/* ------------------------------------------------------------------ */
/* Coverage reporting (Track C)                                        */
/* ------------------------------------------------------------------ */

bool emu_trace_covered(const emu_trace_t *t, uint64_t off) {
    if (t == NULL || t->blocks == NULL)
        return false;
    for (size_t i = 0; i < t->blocks_len; i++)
        if (t->blocks[i] == off)
            return true;
    return false;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Copy a trace's distinct block offsets into a freshly malloc'd, ascending
 * array (caller frees). *n receives the count; returns NULL on empty/oom. */
static uint64_t *sorted_blocks(const emu_trace_t *t, size_t *n) {
    *n = (t != NULL && t->blocks != NULL) ? t->blocks_len : 0;
    if (*n == 0)
        return NULL;
    uint64_t *s = (uint64_t *)malloc(*n * sizeof *s);
    if (s == NULL) {
        *n = 0;
        return NULL;
    }
    memcpy(s, t->blocks, *n * sizeof *s);
    qsort(s, *n, sizeof *s, cmp_u64);
    return s;
}

void emu_trace_report(const emu_trace_t *t, FILE *out) {
    if (t == NULL || out == NULL)
        return;
    fprintf(out,
            "coverage: %zu distinct blocks, %llu block entries, "
            "%llu instructions%s\n",
            t->blocks_len, (unsigned long long)t->blocks_total,
            (unsigned long long)t->insns_total,
            t->truncated ? " (truncated)" : "");
    size_t n;
    uint64_t *s = sorted_blocks(t, &n);
    if (s == NULL)
        return;
    fprintf(out, "  blocks:");
    for (size_t i = 0; i < n; i++)
        fprintf(out, " 0x%llx", (unsigned long long)s[i]);
    fprintf(out, "\n");
    free(s);
}

size_t emu_coverage_uncovered(const emu_trace_t *covered,
                              const emu_trace_t *universe, FILE *out) {
    if (universe == NULL || universe->blocks == NULL)
        return 0;
    size_t total = universe->blocks_len;
    uint64_t *miss = (uint64_t *)malloc((total ? total : 1) * sizeof *miss);
    if (miss == NULL)
        return 0;
    size_t nmiss = 0;
    for (size_t i = 0; i < total; i++)
        if (!emu_trace_covered(covered, universe->blocks[i]))
            miss[nmiss++] = universe->blocks[i];
    qsort(miss, nmiss, sizeof *miss, cmp_u64);
    if (out != NULL) {
        fprintf(out, "coverage: %zu/%zu blocks covered\n", total - nmiss,
                total);
        if (nmiss > 0) {
            fprintf(out, "  uncovered:");
            for (size_t i = 0; i < nmiss; i++)
                fprintf(out, " 0x%llx", (unsigned long long)miss[i]);
            fprintf(out, "\n");
        }
    }
    free(miss);
    return nmiss;
}

void emu_trace_lcov(const emu_trace_t *t, const char *name, FILE *out) {
    if (t == NULL || out == NULL)
        return;
    /* No debug info, so block byte-offsets stand in for source lines. */
    fprintf(out, "TN:\n");
    fprintf(out, "SF:%s\n", name != NULL ? name : "routine");
    size_t n;
    uint64_t *s = sorted_blocks(t, &n);
    for (size_t i = 0; i < n; i++)
        fprintf(out, "DA:%llu,1\n", (unsigned long long)s[i]);
    free(s);
    fprintf(out, "LF:%zu\nLH:%zu\n", n, n);
    fprintf(out, "end_of_record\n");
}
