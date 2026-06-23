/*
 * asmtest_emu.h — optional emulator tier (Phase 4), backed by Unicorn Engine.
 *
 * Where ASM_CALLn (capture.s) inspects a routine at the ABI boundary on real
 * hardware, the emulator runs a routine inside a fully isolated virtual CPU:
 * preload arbitrary registers and memory, run to `ret` or single-step N
 * instructions, then read back the COMPLETE register file — and turn any
 * invalid memory access into a precise, reported fault instead of a crash.
 *
 * x86-64 only for now (the emulated target, not the host — Unicorn emulates
 * x86-64 regardless of host arch). Build with the emu objects + -lunicorn;
 * the core framework has no dependency on this.
 */
#ifndef ASMTEST_EMU_H
#define ASMTEST_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Full x86-64 general-purpose register file plus rip/rflags. */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
} emu_x86_regs_t;

/* Kind of invalid access, mirrors Unicorn's uc_mem_type for faults. */
typedef enum {
    EMU_FAULT_NONE = 0,
    EMU_FAULT_READ,
    EMU_FAULT_WRITE,
    EMU_FAULT_FETCH,
} emu_fault_kind_t;

typedef struct {
    bool ok;                 /* ran to `ret` (or hit the step limit) cleanly */
    int uc_err;              /* raw Unicorn error code (0 == UC_ERR_OK)       */
    bool faulted;            /* an invalid memory access occurred             */
    uint64_t fault_addr;     /* faulting guest address, if faulted            */
    emu_fault_kind_t fault_kind;
    emu_x86_regs_t regs;     /* full register file after the run              */
} emu_result_t;

typedef struct emu emu_t;

/* Create/destroy an emulator (maps an internal code and stack region). */
emu_t *emu_open(void);
void emu_close(emu_t *e);

/* Map a guest RW region and read/write guest memory (addresses chosen by the
 * caller, distinct from the internal code/stack regions). */
bool emu_map(emu_t *e, uint64_t addr, size_t size);
bool emu_write(emu_t *e, uint64_t addr, const void *data, size_t len);
bool emu_read(emu_t *e, uint64_t addr, void *data, size_t len);

/*
 * Copy `code_len` bytes from `fn` into the emulator and run them with the
 * System V integer args (args[0..nargs)). code_len need only be >= the
 * routine's byte length; emulation stops at the routine's `ret`. If
 * max_insns > 0, stop after that many instructions instead (mid-routine
 * inspection). Results, including any fault, land in *out; returns out->ok.
 */
bool emu_call(emu_t *e, const void *fn, size_t code_len, const long *args,
              int nargs, uint64_t max_insns, emu_result_t *out);

/* ------------------------------------------------------------------ */
/* AArch64 guest (emulated regardless of host architecture)            */
/*                                                                     */
/* Cross-arch emulation can't copy bytes from a host-native routine, so */
/* emu_arm64_call takes raw AArch64 machine code directly.              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t x[31]; /* x0..x30 (x29 = fp, x30 = lr) */
    uint64_t sp, pc, nzcv;
} emu_arm64_regs_t;

typedef struct {
    bool ok;
    int uc_err;
    bool faulted;
    uint64_t fault_addr;
    emu_fault_kind_t fault_kind;
    emu_arm64_regs_t regs;
} emu_arm64_result_t;

typedef struct emu_arm64 emu_arm64_t;

emu_arm64_t *emu_arm64_open(void);
void emu_arm64_close(emu_arm64_t *e);
bool emu_arm64_map(emu_arm64_t *e, uint64_t addr, size_t size);
bool emu_arm64_write(emu_arm64_t *e, uint64_t addr, const void *data,
                     size_t len);
bool emu_arm64_read(emu_arm64_t *e, uint64_t addr, void *data, size_t len);

/* Run raw AArch64 machine code with integer args in x0..x5. Stops when the
 * routine's `ret` branches to the sentinel LR, or after max_insns. */
bool emu_arm64_call(emu_arm64_t *e, const void *code, size_t code_len,
                    const long *args, int nargs, uint64_t max_insns,
                    emu_arm64_result_t *out);

#endif /* ASMTEST_EMU_H */
