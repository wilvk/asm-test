/* asmspy_arch.h — the register / single-step / watchpoint-encoding arch seam.
 *
 * asmspy's engines (cli/asmspy_engine.c) single-step, read the program counter /
 * return register / stack pointer, and rewind breakpoints. Those reads were
 * x86-64-hardcoded (`regs.rip`, `regs.rsp`, `regs.rax`, `PTRACE_GETREGS`); this
 * header lifts them behind one seam with an x86-64 body and an AArch64 body, so
 * every engine runs on both architectures without naming a register directly.
 *
 * It mirrors the seam already proven in the LIBRARY's out-of-process stepper
 * (src/ptrace_backend.c: read_pc_ret / set_pc / set_hw_bp), reimplemented over
 * asmspy's own reads — the same file-extraction discipline as asmspy_graphsort.h
 * / asmspy_dataview.h (pure, `static inline`, unit-testable in cli/test_arch.c).
 *
 * Two independent pieces live here:
 *   1. Register access + single-step semantics (the arch seam, gated per-arch).
 *   2. The AArch64 DBGWCR/DBGWVR/BAS data-watchpoint control-word ENCODER —
 *      defined UNCONDITIONALLY (pure integer math) so cli/test_arch.c pins it on
 *      EVERY host, even x86-64 where no AArch64 watchpoint can ever fire.
 *
 * Contains NO engine logic (no loops, no attribution) — register access and
 * encoding only.
 */
#ifndef ASMSPY_ARCH_H
#define ASMSPY_ARCH_H

#include <elf.h> /* NT_PRSTATUS */
#include <stdint.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h> /* struct iovec — the GETREGSET regset transfer */
#include <sys/user.h> /* struct user_regs_struct (x86-64 GPRs; AArch64 user_pt_regs) */

#include "asmtest_trace.h" /* asmtest_arch_t — ASMTEST_ARCH_X86_64 / _ARM64 */

/* The tracee's architecture. asmspy traces a process on the SAME machine, so the
 * tracee's arch is the host arch — no per-target detection is needed (the i386
 * refusal at asmspy_elf_class already rejects the one same-machine mismatch that
 * matters). Feeds the disassembler / operand enumerator in every engine. */
#if defined(__aarch64__)
#define ASMSPY_HOST_ARCH ASMTEST_ARCH_ARM64
#else
#define ASMSPY_HOST_ARCH ASMTEST_ARCH_X86_64
#endif

/* ================================================================= */
/* Register access seam                                              */
/* ================================================================= */

/* Opaque register snapshot. Wraps `struct user_regs_struct`, which on x86-64 is
 * the GPR block PTRACE_GETREGS fills and on AArch64 is `user_pt_regs`
 * (regs[31]/sp/pc/pstate, 272 bytes) that PTRACE_GETREGSET(NT_PRSTATUS) fills.
 * The `.r` member is exposed so the syscalls engine can keep its `ts->entry`
 * snapshot as the raw struct its `scarg`/`ap_*` decoders read. */
typedef struct {
    struct user_regs_struct r;
} asmspy_regs_t;

/* Read the tracee's GPRs. x86-64 has PTRACE_GETREGS; AArch64 does not, so it
 * transfers the NT_PRSTATUS regset via PTRACE_GETREGSET + an iovec (exactly as
 * src/ptrace_backend.c's read_pc_ret does). Returns 0 on success, -1 on failure
 * — the same convention as the raw ptrace() calls it replaces. */
static inline int asmspy_regs_read(pid_t tid, asmspy_regs_t *g) {
#if defined(__aarch64__)
    struct iovec iov = {&g->r, sizeof g->r};
    return ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS,
                  &iov) == 0
               ? 0
               : -1;
#else
    return ptrace(PTRACE_GETREGS, tid, NULL, &g->r) == 0 ? 0 : -1;
#endif
}

/* Write the tracee's GPRs back (PTRACE_SETREGS / PTRACE_SETREGSET). Used only by
 * the x86 trap-flag clear and the region engine's breakpoint rewind. */
static inline int asmspy_regs_write(pid_t tid, const asmspy_regs_t *g) {
#if defined(__aarch64__)
    struct iovec iov = {(void *)&g->r, sizeof g->r};
    return ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS,
                  &iov) == 0
               ? 0
               : -1;
#else
    return ptrace(PTRACE_SETREGS, tid, NULL, (void *)&g->r) == 0 ? 0 : -1;
#endif
}

/* Program counter — the instruction about to retire.        (rip | pc)     */
static inline uint64_t asmspy_reg_pc(const asmspy_regs_t *g) {
#if defined(__aarch64__)
    return (uint64_t)g->r.pc;
#else
    return (uint64_t)g->r.rip;
#endif
}

/* Integer return register / syscall-return value.           (rax | x0)     */
static inline uint64_t asmspy_reg_ret(const asmspy_regs_t *g) {
#if defined(__aarch64__)
    return (uint64_t)g->r.regs[0];
#else
    return (uint64_t)g->r.rax;
#endif
}

/* Stack pointer.                                            (rsp | sp)     */
static inline uint64_t asmspy_reg_sp(const asmspy_regs_t *g) {
#if defined(__aarch64__)
    return (uint64_t)g->r.sp;
#else
    return (uint64_t)g->r.rsp;
#endif
}

/* Link register — the return address a `bl` wrote to x30. x86-64 has no link
 * register (the return address lives on the stack), so this is always 0 there.
 * On AArch64 the `--tree`/`--graph` call-frame check verifies a callee's entry
 * LR equals call_site + 4. (x30 | 0) */
static inline uint64_t asmspy_reg_lr(const asmspy_regs_t *g) {
#if defined(__aarch64__)
    return (uint64_t)g->r.regs[30];
#else
    (void)g;
    return 0;
#endif
}

/* Syscall number at a syscall-stop. x86-64 keeps the ORIGINAL number in the
 * shadow `orig_rax` (rax is overwritten by the return); AArch64 has no `orig_`
 * shadow — at the entry-stop the number is in x8. (orig_rax | x8) */
static inline uint64_t asmspy_reg_syscall_nr(const asmspy_regs_t *g) {
#if defined(__aarch64__)
    return (uint64_t)g->r.regs[8];
#else
    return (uint64_t)g->r.orig_rax;
#endif
}

/* Set the program counter (for the region engine's software-breakpoint rewind —
 * x86 only: `int3` leaves the stop-PC one past the byte). (rip= | pc=) */
static inline void asmspy_set_pc(asmspy_regs_t *g, uint64_t v) {
#if defined(__aarch64__)
    g->r.pc = v;
#else
    g->r.rip = v;
#endif
}

/* ================================================================= */
/* AArch64 NT_ARM_HW_WATCH control-word ENCODER (pure; every host)    */
/*                                                                   */
/* Defined unconditionally so cli/test_arch.c can pin the encoding    */
/* even on x86-64, where no AArch64 watchpoint can fire — the same    */
/* "pure module carries the burden, the live leg covers the wiring"   */
/* discipline asmspy_autoregion.h uses.                               */
/* ================================================================= */

/* DBGWCR fields (arch register low bits): E = bit 0, PAC (privilege) = bits 2:1,
 * LSC (load/store control) = bits 4:3, BAS (byte-address-select) = bits 12:5.
 *   LSC:  load = 0b01, store = 0b10, both = 0b11
 *   PAC:  EL0 (user) = 0b10   (the kernel recomputes PAC from the address, so the
 *         written value is effectively ignored; read-back shows EL0=2)
 *   BAS:  bit i selects byte i of the 8-byte-aligned DBGWVR window
 * (encode_ctrl_reg = (BAS<<5)|(LSC<<3)|(PAC<<1)|E). */
#define ASMSPY_DBGWCR_E         0x1u
#define ASMSPY_DBGWCR_PAC_EL0   0x2u /* privilege = EL0 (user) */
#define ASMSPY_DBGWCR_LSC_LOAD  0x1u
#define ASMSPY_DBGWCR_LSC_STORE 0x2u
#define ASMSPY_DBGWCR_LSC_BOTH  0x3u

/* Assemble a DBGWCR word from an explicit BAS and the direction flag. */
static inline uint32_t asmspy_dbgwcr_from_bas(unsigned bas, int rw_rdwr) {
    unsigned lsc = rw_rdwr ? ASMSPY_DBGWCR_LSC_BOTH : ASMSPY_DBGWCR_LSC_STORE;
    return ((bas & 0xffu) << 5) | (lsc << 3) | (ASMSPY_DBGWCR_PAC_EL0 << 1) |
           ASMSPY_DBGWCR_E;
}

/* DBGWCR for a full, 8-byte-aligned `len`-byte watch (offset 0): BAS = all `len`
 * low bytes selected. `watch(store, 8)` == 0x1FF5 = (0xff<<5)|(0b10<<3)|(0b10<<1)|1.
 * `rw_rdwr` selects read+write (LSC=0b11) instead of write-only (LSC=0b10). */
static inline uint32_t asmspy_dbgwcr_word(int rw_rdwr, int len) {
    unsigned bas;
    switch (len) {
    case 1:
        bas = 0x01u;
        break;
    case 2:
        bas = 0x03u;
        break;
    case 4:
        bas = 0x0fu;
        break;
    default:
        bas = 0xffu; /* 8 bytes */
        break;
    }
    return asmspy_dbgwcr_from_bas(bas, rw_rdwr);
}

/* A resolved AArch64 data watchpoint: the 8-byte-aligned DBGWVR base address, the
 * DBGWCR control word, and `ok` (0 = the request was REJECTED). */
typedef struct {
    uint64_t dbgwvr; /* 8-byte-aligned watch base (addr & ~7)               */
    uint32_t dbgwcr; /* control word (E/PAC/LSC/BAS)                        */
    int ok;          /* 1 = encodable, 0 = rejected (bad len / crosses 8B)  */
} asmspy_watch_enc_t;

/* Encode a `len`-byte data watch at `addr` in `--rw` (both) or write-only mode.
 *
 * DBGWVR holds the 8-byte-aligned address; BAS bit i selects byte DBGWVR+i. For a
 * len-byte watch at offset = addr & 7:  DBGWVR = addr & ~7,  BAS = ((1<<len)-1) <<
 * offset. The window MAY NOT cross the 8-byte boundary, so `offset + len > 8` is
 * rejected (ok = 0) — the AArch64 analog of x86's length-alignment reject. So a
 * 4-byte watch at base+4 is DBGWVR=base / BAS=0xf0; a 4-byte watch at base+6 is
 * rejected. `len` must be 1/2/4/8. */
static inline asmspy_watch_enc_t asmspy_watch_encode(uint64_t addr, int len,
                                                     int rw_rdwr) {
    asmspy_watch_enc_t e;
    e.dbgwvr = 0;
    e.dbgwcr = 0;
    e.ok = 0;
    if (len != 1 && len != 2 && len != 4 && len != 8)
        return e;
    unsigned offset = (unsigned)(addr & 7u);
    if (offset + (unsigned)len > 8u)
        return e; /* window would cross the 8-byte boundary */
    unsigned bas = (((1u << len) - 1u) & 0xffu) << offset;
    e.dbgwvr = addr & ~(uint64_t)7u;
    e.dbgwcr = asmspy_dbgwcr_from_bas(bas, rw_rdwr);
    e.ok = 1;
    return e;
}

#endif /* ASMSPY_ARCH_H */
