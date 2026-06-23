/*
 * emu.c — Unicorn-Engine-backed emulator tier (Phase 4), x86-64 guest.
 */
#include "asmtest_emu.h"

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
}

bool emu_call(emu_t *e, const void *fn, size_t code_len, const long *args,
              int nargs, uint64_t max_insns, emu_result_t *out) {
    static const int arg_regs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI,
                                    UC_X86_REG_RDX, UC_X86_REG_RCX,
                                    UC_X86_REG_R8,  UC_X86_REG_R9};
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);

    if (uc_mem_write(uc, EMU_CODE_BASE, fn, code_len) != UC_ERR_OK) {
        out->uc_err = -1;
        return false;
    }

    /* Stack: place a sentinel return address so `ret` lands on EMU_RET_MAGIC,
     * where emulation stops. rsp ends up 8 (mod 16), as after a real call. */
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 8;
    uint64_t ret = EMU_RET_MAGIC;
    uc_mem_write(uc, sp, &ret, sizeof ret);
    uc_reg_write(uc, UC_X86_REG_RSP, &sp);

    for (int i = 0; i < nargs && i < 6; i++) {
        uint64_t v = (uint64_t)args[i];
        uc_reg_write(uc, arg_regs[i], &v);
    }

    fault_rec_t fr = {0};
    uc_hook hh;
    uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void *)on_invalid_mem, &fr, 1,
                0);

    uc_err err =
        uc_emu_start(uc, EMU_CODE_BASE, EMU_RET_MAGIC, 0, (size_t)max_insns);
    uc_hook_del(uc, hh);

    out->uc_err = (int)err;
    out->faulted = fr.faulted;
    out->fault_addr = fr.addr;
    out->fault_kind = fr.kind;
    read_all_regs(uc, &out->regs);
    out->ok = (err == UC_ERR_OK) && !fr.faulted;
    return out->ok;
}
