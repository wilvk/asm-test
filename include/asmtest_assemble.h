/*
 * asmtest_assemble.h — optional in-line assembler (text -> machine code),
 * backed by the Keystone Engine.
 *
 * Where the emulator tier (asmtest_emu.h) runs a routine supplied as raw
 * machine-code bytes, this turns assembly *source text* into those bytes for
 * any guest the emulator runs. It is the assembler counterpart to the Unicorn
 * disassembler the emu tier uses, and pairs naturally with it: assemble a
 * string here, then run it through emu_call (see emu_call_asm below).
 *
 * Keystone is optional and pkg-config gated (-lkeystone); the core framework
 * and the emulator tier have no dependency on this. Build the assemble object
 * + -lkeystone to use it.
 */
#ifndef ASMTEST_ASSEMBLE_H
#define ASMTEST_ASSEMBLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asmtest_emu.h" /* emu_* handles/results for the bridge wrappers */

#ifdef __cplusplus
extern "C" {
#endif

/* Target architecture for the assembler. Mirrors the emulator's guest set:
 * x86-64, AArch64, RISC-V (RV64), and ARM32. RISC-V depends on the linked
 * Keystone build supporting it (released Keystone does not yet); assembling
 * ASM_RISCV64 against a build without it fails with a clear message rather
 * than at link time. */
typedef enum {
    ASM_X86_64,
    ASM_ARM64,
    ASM_RISCV64,
    ASM_ARM32,
} asm_arch_t;

/* Input assembly syntax. Only meaningful for x86 (ASM_X86_64); ignored for the
 * other guests. In-line strings default to Intel (the Keystone/kstool
 * convention); ASM_SYNTAX_ATT selects the GAS/AT&T syntax the repo's .s corpus
 * is written in. ASM_SYNTAX_NASM and ASM_SYNTAX_MASM are the further
 * Intel-family dialects, and ASM_SYNTAX_GAS the AT&T-family GNU-assembler
 * dialect; each maps to the matching Keystone x86 option. The enum values are
 * part of the binding ABI (passed as ints across the FFI), so only append.
 *
 * Note: NASM uses ';' for end-of-line comments, so a multi-statement NASM
 * source must separate its statements with newlines, not ';' — the Intel /
 * AT&T / MASM / GAS dialects accept ';' as a separator as before. */
typedef enum {
    ASM_SYNTAX_INTEL,
    ASM_SYNTAX_ATT,
    ASM_SYNTAX_NASM,
    ASM_SYNTAX_MASM,
    ASM_SYNTAX_GAS,
} asm_syntax_t;

/* Result of one assemble. On success, `bytes` holds `len` bytes of machine code
 * (owned by the caller; release with asmtest_asm_free). On failure, `ok` is
 * false, `bytes` is NULL, and `err` carries the Keystone diagnostic. */
typedef struct {
    uint8_t *bytes;      /* malloc'd machine code, or NULL on failure     */
    size_t   len;        /* byte length of `bytes`                        */
    size_t   stat_count; /* assembly statements successfully processed    */
    bool     ok;         /* true iff the source assembled cleanly         */
    char     err[160];   /* human-readable error on failure ("" on ok)    */
} asm_result_t;

/*
 * Assemble `source` (statements separated by ';' or newline) for `arch` at load
 * address `addr` — pass the address the bytes will run at (e.g. EMU_CODE_BASE)
 * so PC-relative and branch targets resolve correctly. `syntax` applies to
 * x86 only. Fills *out and returns out->ok. Always release *out (whether it
 * succeeded or not) with asmtest_asm_free.
 */
bool asmtest_assemble(asm_arch_t arch, asm_syntax_t syntax, const char *source,
                      uint64_t addr, asm_result_t *out);

/* Free the machine code owned by *r and reset it (safe to call on a zeroed or
 * already-freed result, and on a failed one). */
void asmtest_asm_free(asm_result_t *r);

/* ---- Bridge: assemble + run through the emulator tier (Phase 2) ---------- */
/* Each assembles `src` for its guest at the emulator's load base, then runs it
 * through the matching emu_*_call. Returns false (with the Keystone message in
 * the result's error channel) if the source fails to assemble. Declared here;
 * defined alongside asmtest_assemble so a single object bridges both libs. */

bool emu_call_asm(emu_t *e, const char *src, const long *args, int nargs,
                  uint64_t max_insns, emu_result_t *out);

bool emu_arm64_call_asm(emu_arm64_t *e, const char *src, const long *args,
                        int nargs, uint64_t max_insns, emu_arm64_result_t *out);

bool emu_riscv_call_asm(emu_riscv_t *e, const char *src, const long *args,
                        int nargs, uint64_t max_insns, emu_riscv_result_t *out);

bool emu_arm_call_asm(emu_arm_t *e, const char *src, const long *args, int nargs,
                      uint64_t max_insns, emu_arm_result_t *out);

/* Opaque-handle FFI shim for dynamic-language bindings (mirrors
 * asmtest_emu_call2 in asmtest_emu.h): assemble x86-64 `src` and run it with
 * two integer args, results in the *out handle (read via the asmtest_emu_*
 * accessors). Returns 1 if it assembled and ran, 0 otherwise. Thin Intel-only,
 * two-arg wrapper over asmtest_emu_call_asm6 — kept for source compatibility;
 * new bindings should call the widened shim below. */
int asmtest_emu_call_asm(emu_t *e, const char *src, long a0, long a1,
                         emu_result_t *out);

/* Widened opaque-handle FFI shim. Assemble x86-64 `src` in `syntax`
 * (ASM_SYNTAX_INTEL or ASM_SYNTAX_ATT, as the int 0/1) and run it with the
 * first `nargs` (0..6) of the scalar args, stopping after `max_insns`
 * instructions (0 = run to `ret`). Results land in *out (read via the
 * asmtest_emu_* accessors). Returns 1 if it assembled and ran, 0 otherwise; on
 * failure the Keystone diagnostic is available from asmtest_asm_last_error().
 * Mirrors asmtest_capture6's six-scalar shape so dynamic FFIs marshal no
 * array. */
int asmtest_emu_call_asm6(emu_t *e, const char *src, int syntax, long a0,
                          long a1, long a2, long a3, long a4, long a5, int nargs,
                          uint64_t max_insns, emu_result_t *out);

/* The Keystone diagnostic from the most recent assemble on the calling thread
 * ("" when the last one succeeded, or none has run). Lets a binding report
 * *why* a string failed to assemble without a by-value struct return. The
 * storage is thread-local; copy it if you need it past the next assemble. */
const char *asmtest_asm_last_error(void);

/* Assemble-only FFI shim (no emulator): assemble `src` for `arch`
 * (asm_arch_t as the int 0..3) in `syntax` (0/1) at load address `addr`,
 * copying up to `cap` bytes of machine code into `buf`. Returns the full byte
 * length (which may exceed `cap`, so a caller can size a buffer and retry), or
 * 0 on failure (diagnostic via asmtest_asm_last_error). Unlike the run shims
 * this is multi-arch — a binding can assemble AArch64/RISC-V/ARM32 even though
 * its emulator handle is the x86-64 guest. */
int asmtest_asm_bytes(int arch, int syntax, const char *src, uint64_t addr,
                      uint8_t *buf, int cap);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ASMTEST_ASSEMBLE_H */
