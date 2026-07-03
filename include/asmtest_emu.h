/*
 * asmtest_emu.h — optional emulator tier (Phase 4), backed by Unicorn Engine.
 *
 * Where ASM_CALLn (capture.s) inspects a routine at the ABI boundary on real
 * hardware, the emulator runs a routine inside a fully isolated virtual CPU:
 * preload arbitrary registers and memory, run to `ret` or single-step N
 * instructions, then read back the COMPLETE register file — and turn any
 * invalid memory access into a precise, reported fault instead of a crash.
 *
 * Guests: x86-64, AArch64, RISC-V (RV64), and ARM32 — the emulated target, not
 * the host; Unicorn emulates each regardless of host arch. Build with the emu
 * objects + -lunicorn; the core framework has no dependency on this.
 */
#ifndef ASMTEST_EMU_H
#define ASMTEST_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The execution-trace model (asmtest_trace_t), the source line-map types, the
 * disassembler arch enum, and the engine-neutral coverage/report helpers were
 * extracted into asmtest_trace.h so every trace backend (Unicorn here, plus the
 * optional DynamoRIO and Intel PT / ARM CoreSight tiers) shares one sink. This
 * header re-exports the historical emu_* / EMU_ARCH_* spellings as aliases. */
#include "asmtest_trace.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Portable compile-time assert (this header is standalone — it may be included
 * without asmtest.h, and from C++). The #ifndef lets both headers define it. */
#ifndef ASMTEST_STATIC_ASSERT
#ifdef __cplusplus
#define ASMTEST_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define ASMTEST_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif

/* Guest load base: every guest maps its code region here and starts execution
 * at this address. Exposed so the in-line assembler (asmtest_assemble.h) can
 * assemble at the same address the bytes run at, keeping PC-relative and branch
 * targets correct. The size/stack/sentinel constants stay private to emu.c. */
#define EMU_CODE_BASE 0x00100000UL

/* One 128-bit vector (XMM) register, viewable as several lane layouts. Mirrors
 * vec128_t from asmtest.h, but kept independent so this header stands alone. */
typedef union {
    uint8_t u8[16];
    uint32_t u32[4];
    uint64_t u64[2];
    float f32[4];
    double f64[2];
} emu_vec128_t;

/* Full x86-64 general-purpose register file plus rip/rflags and the XMM file.
 * xmm[] is filled on every run; for a routine returning a double the result is
 * xmm[0].f64[0], and a 128-bit vector return is xmm[0]. */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags;
    emu_vec128_t xmm[16];
} emu_x86_regs_t;

/* Layout contract (Track 0): bindings mirror these register structs, and the
 * generated manifest (scripts/gen-manifest.c) reports their offsets. The
 * asserts guarantee the header, the manifest, and any binding agree. */
ASMTEST_STATIC_ASSERT(offsetof(emu_x86_regs_t, xmm) == 144,
                      "emu_x86_regs_t.xmm @144");
ASMTEST_STATIC_ASSERT(sizeof(emu_x86_regs_t) == 400, "emu_x86_regs_t size");

/* Kind of invalid access, mirrors Unicorn's uc_mem_type for faults. */
typedef enum {
    EMU_FAULT_NONE = 0,
    EMU_FAULT_READ,
    EMU_FAULT_WRITE,
    EMU_FAULT_FETCH,
} emu_fault_kind_t;

typedef struct {
    bool ok;             /* ran to `ret` (or hit the step limit) cleanly */
    int uc_err;          /* raw Unicorn error code (0 == UC_ERR_OK)       */
    bool faulted;        /* an invalid memory access occurred             */
    uint64_t fault_addr; /* faulting guest address, if faulted            */
    emu_fault_kind_t fault_kind;
    emu_x86_regs_t regs; /* full register file after the run              */
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

/* Opaque-handle FFI helpers for dynamic-language bindings (see src/ffi.c and
 * emu.c): an emu_result_t handle, scalar-arg call, and field accessors, so a
 * binding needs no struct layout. asmtest_emu_call2 runs `fn` (64-byte window)
 * with two integer args; faults are read via the accessors. */
emu_result_t *asmtest_emu_result_new(void);
void asmtest_emu_result_free(emu_result_t *r);
int asmtest_emu_result_ok(const emu_result_t *r);
int asmtest_emu_result_faulted(const emu_result_t *r);
unsigned long long asmtest_emu_result_fault_addr(const emu_result_t *r);
int asmtest_emu_result_fault_kind(const emu_result_t *r);
unsigned long long asmtest_emu_x86_reg(const emu_result_t *r, const char *name);
double asmtest_emu_x86_xmm_f64(const emu_result_t *r, int index, int lane);
float asmtest_emu_x86_xmm_f32(const emu_result_t *r, int index, int lane);
int asmtest_emu_call2(emu_t *e, const void *fn, long a0, long a1,
                      emu_result_t *out);

/* Wider/typed scalar-arg emu wrappers for dynamic-FFI bindings (see emu.c).
 * asmtest_emu_call6 takes up to six integer args (nargs in [0,6]) and a
 * max_insns cap; _call_fp2 marshals two doubles into xmm0/xmm1 (return in
 * xmm[0].f64[0]); _call_vec_f32 marshals nvec 128-bit vectors from a flat
 * 4*nvec float32 array; _call_win64_6 uses the Win64 convention (rcx,rdx,r8,r9).
 * Each runs `fn` as a 64-byte code window and returns 1 if it ran, 0 otherwise. */
int asmtest_emu_call6(emu_t *e, const void *fn, long a0, long a1, long a2,
                      long a3, long a4, long a5, int nargs, uint64_t max_insns,
                      emu_result_t *out);
int asmtest_emu_call_fp2(emu_t *e, const void *fn, double f0, double f1,
                         emu_result_t *out);
int asmtest_emu_call_vec_f32(emu_t *e, const void *fn, const float *lanes,
                             int nvec, emu_result_t *out);
int asmtest_emu_call_win64_6(emu_t *e, const void *fn, long a0, long a1,
                             long a2, long a3, int nargs, uint64_t max_insns,
                             emu_result_t *out);

/* Like emu_call, but also marshals `nfargs` double args into the SysV FP
 * argument registers (xmm0..7) alongside the `niargs` integer args. The scalar
 * double return is out->regs.xmm[0].f64[0]. (x86-64 guest.) */
bool emu_call_fp(emu_t *e, const void *fn, size_t code_len, const long *iargs,
                 int niargs, const double *fargs, int nfargs,
                 uint64_t max_insns, emu_result_t *out);

/* Like emu_call, but marshals `nvargs` full 128-bit vector args into the vector
 * argument registers (xmm0..7) alongside the `niargs` integer args. The whole
 * XMM file is captured into out->regs.xmm[]; a vector return is xmm[0].
 * (x86-64 guest.) */
bool emu_call_vec(emu_t *e, const void *fn, size_t code_len, const long *iargs,
                  int niargs, const emu_vec128_t *vargs, int nvargs,
                  uint64_t max_insns, emu_result_t *out);

/* ------------------------------------------------------------------ */
/* Execution trace & basic-block coverage (Phase 10)                   */
/*                                                                     */
/* The trace record (ordered instruction offsets + distinct basic-block */
/* offsets, totals, truncation bit) is the engine-neutral asmtest_trace_t */
/* defined in asmtest_trace.h. emu_trace_t is the historical name kept   */
/* for the emulator-tier signatures and the bindings; it is the same     */
/* type. Tracing is opt-in and APPENDS: zero the struct, point insns/    */
/* blocks at caller-owned buffers (either may stay NULL), then run via    */
/* emu_call_traced. Re-running with the same struct accumulates.          */
/* ------------------------------------------------------------------ */
typedef asmtest_trace_t emu_trace_t;

/* Like emu_call, but also records an execution trace / coverage into *trace
 * (which may be NULL to behave exactly like emu_call). See emu_trace_t. */
bool emu_call_traced(emu_t *e, const void *fn, size_t code_len,
                     const long *args, int nargs, uint64_t max_insns,
                     emu_result_t *out, emu_trace_t *trace);

/* The opaque execution-trace handle for dynamic-FFI bindings (asmtest_emu_trace_*)
 * and its accessors are declared in asmtest_trace.h (backend-neutral). Pass the
 * handle to asmtest_emu_call6_traced to record into it; read coverage back via
 * the accessors. */

/* Like asmtest_emu_call6, but records an execution trace / coverage into the
 * opaque `trace` handle (may be NULL). */
int asmtest_emu_call6_traced(emu_t *e, const void *fn, long a0, long a1,
                             long a2, long a3, long a4, long a5, int nargs,
                             uint64_t max_insns, emu_result_t *out,
                             emu_trace_t *trace);

/* Run x86-64 code under the Microsoft x64 ("Win64") calling convention instead
 * of System V, on the same emulator engine: integer args go in rcx, rdx, r8,
 * r9 then on the stack above 32 bytes of caller-reserved shadow space, the
 * return value is in rax, and rsi/rdi are nonvolatile (callee-saved) rather
 * than argument registers. Lets Win64 routines be tested on a System V host.
 * The _traced form records a trace / coverage just like emu_call_traced. */
bool emu_call_win64(emu_t *e, const void *fn, size_t code_len, const long *args,
                    int nargs, uint64_t max_insns, emu_result_t *out);
bool emu_call_win64_traced(emu_t *e, const void *fn, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_result_t *out, emu_trace_t *trace);

/* ------------------------------------------------------------------ */
/* AArch64 guest (emulated regardless of host architecture)            */
/*                                                                     */
/* Cross-arch emulation can't copy bytes from a host-native routine, so */
/* emu_arm64_call takes raw AArch64 machine code directly.              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t x[31]; /* x0..x30 (x29 = fp, x30 = lr) */
    uint64_t sp, pc, nzcv;
    emu_vec128_t v[32]; /* NEON v0..v31; a double return is v[0].f64[0] */
} emu_arm64_regs_t;

ASMTEST_STATIC_ASSERT(offsetof(emu_arm64_regs_t, v) == 272,
                      "emu_arm64_regs_t.v @272");
ASMTEST_STATIC_ASSERT(sizeof(emu_arm64_regs_t) == 784, "emu_arm64_regs_t size");

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

/* Run raw AArch64 machine code with integer args in x0..x7 (AAPCS64 assigns the
 * first eight integer args to x0..x7). Stops when the routine's `ret` branches
 * to the sentinel LR, or after max_insns. */
bool emu_arm64_call(emu_arm64_t *e, const void *code, size_t code_len,
                    const long *args, int nargs, uint64_t max_insns,
                    emu_arm64_result_t *out);

/* Like emu_arm64_call, but also records a trace / coverage into *trace (which
 * may be NULL). Offsets are byte offsets from the start of the code; AArch64
 * instructions are 4 bytes, so they fall on 0, 4, 8, ... See emu_trace_t. */
bool emu_arm64_call_traced(emu_arm64_t *e, const void *code, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_arm64_result_t *out, emu_trace_t *trace);

/* Like emu_arm64_call, but also marshals `nfargs` double args into d0..d7 (the
 * AAPCS64 FP arg registers) alongside the integer args; the double return is
 * out->regs.v[0].f64[0]. emu_arm64_call_vec marshals `nvargs` 128-bit vectors
 * into v0..v7 and captures the whole v0..v31 file. */
bool emu_arm64_call_fp(emu_arm64_t *e, const void *code, size_t code_len,
                       const long *iargs, int niargs, const double *fargs,
                       int nfargs, uint64_t max_insns, emu_arm64_result_t *out);
bool emu_arm64_call_vec(emu_arm64_t *e, const void *code, size_t code_len,
                        const long *iargs, int niargs,
                        const emu_vec128_t *vargs, int nvargs,
                        uint64_t max_insns, emu_arm64_result_t *out);

/* ------------------------------------------------------------------ */
/* RISC-V (RV64) guest (emulated regardless of host architecture)      */
/*                                                                     */
/* Like the AArch64 guest, this runs raw RV64 machine code directly,   */
/* so RISC-V routines emulate on an x86-64 or AArch64 host. Integer    */
/* args go in a0..a7 (x10..x17); the return value lands in a0/a1; ra   */
/* (x1) holds the sentinel return address and sp (x2) the stack. There */
/* is no flags register — RISC-V folds comparisons into its branches.  */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t
        x[32]; /* x0..x31; x0 = zero, x1 = ra, x2 = sp, x10..x17 = a0..a7 */
    uint64_t pc;
    emu_vec128_t f[32]; /* F0..F31 (D ext, 64-bit in f64[0]); fa0 = f[10] */
} emu_riscv_regs_t;

ASMTEST_STATIC_ASSERT(offsetof(emu_riscv_regs_t, f) == 264,
                      "emu_riscv_regs_t.f @264");
ASMTEST_STATIC_ASSERT(sizeof(emu_riscv_regs_t) == 776, "emu_riscv_regs_t size");

typedef struct {
    bool ok;
    int uc_err;
    bool faulted;
    uint64_t fault_addr;
    emu_fault_kind_t fault_kind;
    emu_riscv_regs_t regs;
} emu_riscv_result_t;

typedef struct emu_riscv emu_riscv_t;

emu_riscv_t *emu_riscv_open(void);
void emu_riscv_close(emu_riscv_t *e);
bool emu_riscv_map(emu_riscv_t *e, uint64_t addr, size_t size);
bool emu_riscv_write(emu_riscv_t *e, uint64_t addr, const void *data,
                     size_t len);
bool emu_riscv_read(emu_riscv_t *e, uint64_t addr, void *data, size_t len);

/* Run raw RV64 machine code with integer args in a0..a7. Stops when the
 * routine's `ret` (jalr x0, 0(ra)) branches to the sentinel ra, or after
 * max_insns. */
bool emu_riscv_call(emu_riscv_t *e, const void *code, size_t code_len,
                    const long *args, int nargs, uint64_t max_insns,
                    emu_riscv_result_t *out);

/* Like emu_riscv_call, but also records a trace / coverage into *trace (which
 * may be NULL). Offsets are byte offsets from the start of the code; base RV64
 * instructions are 4 bytes, so they fall on 0, 4, 8, ... See emu_trace_t. */
bool emu_riscv_call_traced(emu_riscv_t *e, const void *code, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_riscv_result_t *out, emu_trace_t *trace);

/* Like emu_riscv_call, but also marshals `nfargs` double args into fa0..fa7
 * (f10..f17, the FP arg registers of the D extension) alongside the integer
 * args; the double return is out->regs.f[10].f64[0] (fa0). The FP unit is
 * enabled at emu_riscv_open (mstatus.FS). */
bool emu_riscv_call_fp(emu_riscv_t *e, const void *code, size_t code_len,
                       const long *iargs, int niargs, const double *fargs,
                       int nfargs, uint64_t max_insns, emu_riscv_result_t *out);

/* ------------------------------------------------------------------ */
/* ARM32 (AArch32 / A32) guest (emulated regardless of host arch)      */
/*                                                                     */
/* Like the AArch64 and RISC-V guests, this runs raw A32 machine code  */
/* directly, so ARM32 routines emulate on an x86-64 host. Per the      */
/* AAPCS, integer args go in r0..r3; the return value lands in r0      */
/* (r0:r1 for 64-bit); lr (r14) holds the sentinel return address and  */
/* sp (r13) the stack; the routine returns with `bx lr`. Registers are */
/* 32-bit; cpsr carries the condition flags (N/Z/C/V).                 */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t r[16]; /* r0..r15; r13 = sp, r14 = lr, r15 = pc */
    uint32_t cpsr;  /* condition flags live in the top bits  */
    emu_vec128_t
        q[16]; /* VFP/NEON q0..q15; d(2k)=q[k].f64[0], d(2k+1)=.f64[1] */
} emu_arm_regs_t;

ASMTEST_STATIC_ASSERT(offsetof(emu_arm_regs_t, q) == 72,
                      "emu_arm_regs_t.q @72");
ASMTEST_STATIC_ASSERT(sizeof(emu_arm_regs_t) == 328, "emu_arm_regs_t size");

typedef struct {
    bool ok;
    int uc_err;
    bool faulted;
    uint64_t fault_addr;
    emu_fault_kind_t fault_kind;
    emu_arm_regs_t regs;
} emu_arm_result_t;

typedef struct emu_arm emu_arm_t;

emu_arm_t *emu_arm_open(void);
void emu_arm_close(emu_arm_t *e);
bool emu_arm_map(emu_arm_t *e, uint64_t addr, size_t size);
bool emu_arm_write(emu_arm_t *e, uint64_t addr, const void *data, size_t len);
bool emu_arm_read(emu_arm_t *e, uint64_t addr, void *data, size_t len);

/* Run raw A32 machine code with integer args in r0..r3. Stops when the
 * routine's `bx lr` branches to the sentinel lr, or after max_insns. */
bool emu_arm_call(emu_arm_t *e, const void *code, size_t code_len,
                  const long *args, int nargs, uint64_t max_insns,
                  emu_arm_result_t *out);

/* Like emu_arm_call, but also records a trace / coverage into *trace (which
 * may be NULL). Offsets are byte offsets from the start of the code; A32
 * instructions are 4 bytes, so they fall on 0, 4, 8, ... See emu_trace_t. */
bool emu_arm_call_traced(emu_arm_t *e, const void *code, size_t code_len,
                         const long *args, int nargs, uint64_t max_insns,
                         emu_arm_result_t *out, emu_trace_t *trace);

/* Like emu_arm_call, but also marshals `nfargs` double args into d0..d7 (the
 * AAPCS-VFP FP arg registers) alongside the integer args; the double return is
 * out->regs.q[0].f64[0] (d0). The VFP unit is enabled at emu_arm_open
 * (CPACR + FPEXC). */
bool emu_arm_call_fp(emu_arm_t *e, const void *code, size_t code_len,
                     const long *iargs, int niargs, const double *fargs,
                     int nfargs, uint64_t max_insns, emu_arm_result_t *out);

/* Like emu_arm_call_fp, but marshals `nvargs` 128-bit NEON vectors into q0..q3
 * (the AAPCS-VFP vector arg registers) alongside the integer args, and captures
 * the whole q0..q15 file. A vector return is out->regs.q[0]. (RISC-V has no
 * counterpart: Unicorn exposes no vector registers for its RISC-V guest, so the
 * V extension cannot be marshalled — see the note in src/emu.c.) */
bool emu_arm_call_vec(emu_arm_t *e, const void *code, size_t code_len,
                      const long *iargs, int niargs, const emu_vec128_t *vargs,
                      int nvargs, uint64_t max_insns, emu_arm_result_t *out);

/* ------------------------------------------------------------------ */
/* Cross-arch emu result handles + register accessors (dynamic-FFI)    */
/*                                                                     */
/* The AArch64 / RISC-V / ARM32 guests run raw machine code (emu_<arch>_call,    */
/* which already takes pointers + scalars, so a binding calls it directly) and   */
/* write a per-arch result struct. These opaque handles + register accessors let */
/* a dynamic-FFI binding read that struct without mirroring its layout. The      */
/* leading fields match emu_result_t, so the asmtest_emu_result_{ok,faulted,     */
/* fault_addr,fault_kind} accessors read any of these via the opaque pointer.    */
/* ------------------------------------------------------------------ */
emu_arm64_result_t *asmtest_emu_arm64_result_new(void);
void asmtest_emu_arm64_result_free(emu_arm64_result_t *r);
unsigned long long asmtest_emu_arm64_reg(const emu_arm64_result_t *r,
                                         const char *name);
double asmtest_emu_arm64_vec_f64(const emu_arm64_result_t *r, int index,
                                 int lane);
float asmtest_emu_arm64_vec_f32(const emu_arm64_result_t *r, int index,
                                int lane);

emu_riscv_result_t *asmtest_emu_riscv_result_new(void);
void asmtest_emu_riscv_result_free(emu_riscv_result_t *r);
unsigned long long asmtest_emu_riscv_reg(const emu_riscv_result_t *r,
                                         const char *name);
double asmtest_emu_riscv_f_f64(const emu_riscv_result_t *r, int index,
                               int lane);

emu_arm_result_t *asmtest_emu_arm_result_new(void);
void asmtest_emu_arm_result_free(emu_arm_result_t *r);
unsigned long long asmtest_emu_arm_reg(const emu_arm_result_t *r,
                                       const char *name);
double asmtest_emu_arm_q_f64(const emu_arm_result_t *r, int index, int lane);
float asmtest_emu_arm_q_f32(const emu_arm_result_t *r, int index, int lane);

/* ------------------------------------------------------------------ */
/* Coverage reporting (Track C)                                        */
/*                                                                     */
/* Helpers over an accumulated emu_trace_t (see emu_call_traced). All  */
/* are arch-neutral: a trace records block byte-offsets from the       */
/* routine entry, whatever the guest.                                  */
/* ------------------------------------------------------------------ */

/* The coverage/report helpers over an accumulated trace — emu_trace_covered,
 * emu_trace_report, emu_coverage_uncovered, emu_trace_lcov — are engine-neutral
 * and declared in asmtest_trace.h (new code can use the canonical asmtest_trace_*
 * spellings). They are listed here for the emulator tier's historical surface. */

/* ------------------------------------------------------------------ */
/* Disassembly in diagnostics (optional, Capstone)                     */
/*                                                                     */
/* Faults, traces, and coverage are recorded as byte offsets from the  */
/* routine entry. When the build links Capstone — the disassembler     */
/* counterpart to the Keystone assembler in src/assemble.c — these      */
/* helpers annotate an offset with the instruction at it, turning       */
/* `uncovered: 0x2f` into `uncovered: 0x2f  cmp rax, 0`. Optional and   */
/* self-skipping: built without Capstone, emu_disas_available() is      */
/* false and every helper degrades to the plain offset form, so the     */
/* same call works either way and the core build (emu.o,                */
/* libasmtest_emu) stays Capstone-free.                                 */
/* ------------------------------------------------------------------ */

/* Guest architecture for the disassembler, mirroring the emulator's guest set
 * (and asm_arch_t's ordering): pass the arch of the guest whose code you ran
 * (the x86-64 guest -> EMU_ARCH_X86_64, the AArch64 guest -> EMU_ARCH_ARM64).
 * The canonical type is asmtest_arch_t (asmtest_trace.h); emu_arch_t and the
 * EMU_ARCH_* names are compatibility aliases for it. */
typedef asmtest_arch_t emu_arch_t;
#define EMU_ARCH_X86_64  ASMTEST_ARCH_X86_64
#define EMU_ARCH_ARM64   ASMTEST_ARCH_ARM64
#define EMU_ARCH_RISCV64 ASMTEST_ARCH_RISCV64
#define EMU_ARCH_ARM32   ASMTEST_ARCH_ARM32

/* True iff this build links Capstone, so the helpers below produce instruction
 * text rather than degrading to bare offsets. (RISC-V additionally needs a
 * Capstone >= 5 build; emu_disas self-skips that guest on older Capstone.) */
bool emu_disas_available(void);

/* Disassemble the single instruction at code[off] for `arch`, formatting
 * "<mnemonic> <operands>" into buf (always NUL-terminated, truncated to
 * buflen). `base_addr` is the address the bytes run at (EMU_CODE_BASE), so
 * PC-relative operands resolve to absolute targets. Returns the instruction's
 * byte length, or 0 when Capstone is absent or the bytes do not decode — in
 * which case buf is set to "" so the caller can fall back to the offset. */
size_t emu_disas(emu_arch_t arch, const uint8_t *code, size_t code_len,
                 uint64_t base_addr, uint64_t off, char *buf, size_t buflen);

/* Print the ordered instruction trace (emu_trace_t.insns, in execution order)
 * with each entry disassembled — a readable execution listing, complementing
 * the coverage summary from emu_trace_report_disasm. Without Capstone (or with
 * code NULL) prints the bare offsets. */
void emu_trace_disasm(const emu_trace_t *t, emu_arch_t arch,
                      const uint8_t *code, size_t code_len, FILE *out);

/* Disassembling counterparts to emu_trace_report / emu_coverage_uncovered:
 * the same summary line, but each listed block offset is annotated with its
 * first instruction when Capstone is present and `code`/`code_len` (the routine
 * bytes passed to emu_*_call_traced) are supplied for guest `arch`. Without
 * Capstone, or with code NULL, they fall through to the offset-only form (and
 * emu_coverage_uncovered_disasm returns the same uncovered count). */
void emu_trace_report_disasm(const emu_trace_t *t, emu_arch_t arch,
                             const uint8_t *code, size_t code_len, FILE *out);
size_t emu_coverage_uncovered_disasm(const emu_trace_t *covered,
                                     const emu_trace_t *universe,
                                     emu_arch_t arch, const uint8_t *code,
                                     size_t code_len, FILE *out);

/* Format a one-line description of `r`'s fault into buf (always NUL-terminated):
 *   "write fault accessing 0xdead0000: mov [rbx], rax  (@0x12)"   (Capstone)
 *   "write fault accessing 0xdead0000  (@0x12)"                   (no Capstone)
 * The faulting instruction is the one at rip; `code`/`code_len` are its bytes
 * and `base_addr` the address they run at (EMU_CODE_BASE), so @0xNN is the
 * faulting instruction's offset. Writes "no fault" for a clean result. x86-64
 * guest (reads emu_result_t.regs.rip); pairs with ASSERT_NO_FAULT for a richer
 * failure line. */
void emu_fault_describe(const emu_result_t *r, emu_arch_t arch,
                        const uint8_t *code, size_t code_len,
                        uint64_t base_addr, char *buf, size_t buflen);

/* ------------------------------------------------------------------ */
/* Mid-execution guards (x86-64 guest, Track F)                        */
/*                                                                     */
/* Assert properties WHILE a routine runs, not just on its result —    */
/* the introspection no ABI-boundary tool can do. A guard is armed on   */
/* the emu handle and persists across emu_call_* until cleared (so it   */
/* can span a sweep of inputs); each run resets the result, then        */
/* records the FIRST violation as data instead of crashing the host.    */
/* Step-bounded assertions (#3) need no new API: run with max_insns=N   */
/* (the single-step path) and inspect out->regs with ASSERT_EMU_REG_EQ. */
/* ------------------------------------------------------------------ */

/* What counts as a write-watchpoint violation over a region [addr,addr+size):
 * NEVER — a write that touches the region (a forbidden zone, e.g. a guard band
 * past a buffer); ONLY — a write that escapes it (confinement: the routine must
 * only scribble inside its declared output buffer). */
typedef enum {
    EMU_WATCH_NEVER,
    EMU_WATCH_ONLY,
} emu_watch_mode_t;

/* First write that violated the armed watchpoint. `rip_off` is the offending
 * store's offset from EMU_CODE_BASE (feed it to emu_disas / emu_watch_describe
 * for the instruction text). `violated` is false when the watchpoint held. */
typedef struct {
    bool violated;
    uint64_t addr;    /* data address of the offending write   */
    uint32_t size;    /* its width in bytes                    */
    uint64_t rip_off; /* offset of the store that did it       */
} emu_watch_t;

/* First basic-block entry at which a guarded register broke its invariant. */
typedef struct {
    bool violated;
    uint64_t got;     /* the register's value at the breach    */
    uint64_t rip_off; /* that block's offset from EMU_CODE_BASE */
} emu_reg_guard_t;

/* Arm a memory-write watchpoint over [addr, addr+size) for subsequent
 * emu_call_* on this handle; the first violating write lands in *out (pass
 * out=NULL, or emu_watch_clear, to disarm). x86-64 guest. */
void emu_watch_writes(emu_t *e, uint64_t addr, size_t size,
                      emu_watch_mode_t mode, emu_watch_t *out);
void emu_watch_clear(emu_t *e);

/* Arm a register invariant: assert x86 GP register `regname` ("rbx", "rsp", …)
 * holds `want` at every basic-block entry — a callee-saved or stack-pointer
 * guard that catches mid-routine corruption even when the value is restored by
 * return (which ABI capture cannot see). Returns false for an unknown register
 * name. The first breach lands in *out. */
bool emu_guard_reg(emu_t *e, const char *regname, uint64_t want,
                   emu_reg_guard_t *out);
void emu_guard_reg_clear(emu_t *e);

/* Format a one-line description of a watchpoint violation into buf, including
 * the offending store's disassembly when Capstone is present (pairs with the
 * disassembly tier; in src/disasm.c). "no violation" when *w did not fire. */
void emu_watch_describe(const emu_watch_t *w, emu_arch_t arch,
                        const uint8_t *code, size_t code_len,
                        uint64_t base_addr, char *buf, size_t buflen);

/* Opaque-handle FFI for the guard results (a dynamic binding can't lay out the
 * structs): allocate a handle, arm the guard with emu_watch_writes /
 * emu_guard_reg pointing at it, run an emu_call, then read the recorded
 * violation by field. The handle is a heap emu_watch_t / emu_reg_guard_t. */
emu_watch_t *asmtest_emu_watch_new(void);
void asmtest_emu_watch_free(emu_watch_t *w);
int asmtest_emu_watch_violated(const emu_watch_t *w);
unsigned long long asmtest_emu_watch_addr(const emu_watch_t *w);
unsigned asmtest_emu_watch_size(const emu_watch_t *w);
unsigned long long asmtest_emu_watch_rip_off(const emu_watch_t *w);

emu_reg_guard_t *asmtest_emu_reg_guard_new(void);
void asmtest_emu_reg_guard_free(emu_reg_guard_t *g);
int asmtest_emu_reg_guard_violated(const emu_reg_guard_t *g);
unsigned long long asmtest_emu_reg_guard_got(const emu_reg_guard_t *g);
unsigned long long asmtest_emu_reg_guard_rip_off(const emu_reg_guard_t *g);

/* ------------------------------------------------------------------ */
/* Coverage-guided fuzzing & mutation testing (x86-64 guest, Track E)  */
/*                                                                     */
/* Close the loop between the emulator's basic-block coverage and       */
/* input generation, and prove a test's input set actually catches a    */
/* perturbed routine. Both run the routine INSIDE the emulator, so a     */
/* pathological input or a broken mutant (an infinite loop, a wild       */
/* branch) is contained by the instruction cap and fault hooks rather    */
/* than crashing the host. Inputs are one integer arg (the canonical     */
/* property-test shape); the seedable splitmix64 RNG makes runs           */
/* reproducible. Defined in src/fuzz.c (no extra dependency).            */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t blocks_reached; /* distinct blocks in the accumulated union     */
    uint64_t corpus_len;     /* inputs kept, each of which grew coverage      */
    uint64_t iterations;     /* candidate inputs tried                        */
} emu_fuzz_stat_t;

/* Coverage-guided search for a 1-int-arg x86-64 routine: draw up to `iters`
 * candidate inputs in [lo,hi] (seeded by `seed`) — fresh, or by mutating a
 * corpus member (the feedback that makes it "guided") — running each through
 * emu_call_traced so coverage ACCUMULATES into `uni` (zero the struct and point
 * uni->blocks at a buffer first). An input that enters a new block is kept in
 * the corpus. Fills *stat; reaches blocks a handful of fixed vectors miss. */
bool emu_fuzz_cover1(emu_t *e, const void *code, size_t code_len, long lo,
                     long hi, uint64_t iters, uint64_t seed, emu_trace_t *uni,
                     emu_fuzz_stat_t *stat);

typedef struct {
    size_t mutants;  /* mutants generated and run                            */
    size_t killed;   /* an input distinguished the mutant from the original  */
    size_t survived; /* NO input did — a test-gap (or an equivalent mutant)  */
} emu_mutation_stat_t;

/* Mutation-test a 1-int-arg x86-64 routine against an input set: flip bits of
 * `code` (every single-bit flip when max_mutants==0, else `max_mutants` sampled
 * by `seed`), run the mutant and the original on each input, and classify the
 * mutant as killed (some input distinguishes it — the suite would catch the
 * change) or survived (none does). The original routine is the oracle; the
 * emulator contains a broken mutant. Survivors are the signal — a stronger
 * input set kills more. Returns the survivor count; fills *stat. The routine's
 * result must be a function of its argument (the differential-testing premise). */
size_t emu_mutation_test1(emu_t *e, const void *code, size_t code_len,
                          const long *inputs, size_t ninputs,
                          uint64_t max_mutants, uint64_t seed,
                          emu_mutation_stat_t *stat);

/* Opaque-handle FFI for the fuzz/mutation stat structs (a binding allocates the
 * handle, passes it to emu_fuzz_cover1 / emu_mutation_test1, then reads the
 * totals). The drivers themselves take plain pointers/scalars, so a binding
 * calls them directly with this handle + a trace handle. */
emu_fuzz_stat_t *asmtest_emu_fuzz_stat_new(void);
void asmtest_emu_fuzz_stat_free(emu_fuzz_stat_t *s);
unsigned long long asmtest_emu_fuzz_blocks_reached(const emu_fuzz_stat_t *s);
unsigned long long asmtest_emu_fuzz_corpus_len(const emu_fuzz_stat_t *s);
unsigned long long asmtest_emu_fuzz_iterations(const emu_fuzz_stat_t *s);

emu_mutation_stat_t *asmtest_emu_mutation_stat_new(void);
void asmtest_emu_mutation_stat_free(emu_mutation_stat_t *s);
unsigned long long asmtest_emu_mutation_mutants(const emu_mutation_stat_t *s);
unsigned long long asmtest_emu_mutation_killed(const emu_mutation_stat_t *s);
unsigned long long asmtest_emu_mutation_survived(const emu_mutation_stat_t *s);

/* ------------------------------------------------------------------ */
/* Source-line coverage mapping (Track C)                              */
/*                                                                     */
/* The trace records basic-block byte-offsets from the routine entry.  */
/* To report coverage against SOURCE LINES instead of raw offsets,     */
/* supply a line map: an ascending table of (offset, line) rows — the  */
/* shape of a DWARF line program or an assembler listing, produced     */
/* out-of-band (objdump/readelf/--listing). The framework deliberately */
/* does not parse DWARF itself, keeping its no-extra-dependency stance; */
/* it just consumes the normalized map. Row i covers the byte-offsets   */
/* [entries[i].offset, entries[i+1].offset); the last row extends to    */
/* the routine's end. A source line counts as covered when a covered    */
/* basic block begins within its offset range.                          */
/* ------------------------------------------------------------------ */
/* The source line-map types (emu_line_entry_t / emu_line_map_t) and the
 * resolving helpers (emu_line_lookup, emu_trace_source_report,
 * emu_trace_lcov_source) are engine-neutral and declared in asmtest_trace.h. */

/* ------------------------------------------------------------------ */
/* Emulator assertions (Track C)                                       */
/*                                                                     */
/* These build on the core runner's asmtest_fail (declared in          */
/* asmtest.h), so include "asmtest.h" before using them — as the emu   */
/* test suite does. They turn the raw emu_result_t / emu_trace_t       */
/* structs into one-line assertions, matching the native tier's        */
/* ASSERT_* ergonomics.                                                */
/* ------------------------------------------------------------------ */

void asmtest_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* The run completed without an invalid memory access (and the engine was OK).
 * Field-access only (no typed temporary), so these work for every guest's
 * result struct — emu_result_t, emu_arm64_result_t, etc. all share the names. */
#define ASSERT_NO_FAULT(res)                                                   \
    do {                                                                       \
        if ((res)->faulted)                                                    \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_NO_FAULT: faulted at 0x%llx (kind %d)",       \
                         (unsigned long long)(res)->fault_addr,                \
                         (int)(res)->fault_kind);                              \
        else if ((res)->uc_err != 0)                                           \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_NO_FAULT: engine error %d", (res)->uc_err);   \
    } while (0)

/* The run hit an invalid memory access. */
#define ASSERT_FAULT(res)                                                      \
    do {                                                                       \
        if (!(res)->faulted)                                                   \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_FAULT: expected a fault, none occurred");     \
    } while (0)

/* The run faulted with a specific kind (EMU_FAULT_READ/WRITE/FETCH) at addr. */
#define ASSERT_FAULT_AT(res, want_kind, want_addr)                             \
    do {                                                                       \
        if (!(res)->faulted)                                                   \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_FAULT_AT: expected a fault, none occurred");  \
        else if ((res)->fault_kind != (want_kind) ||                           \
                 (res)->fault_addr != (uint64_t)(want_addr))                   \
            asmtest_fail(                                                      \
                __FILE__, __LINE__,                                            \
                "ASSERT_FAULT_AT: got kind %d at 0x%llx, want kind %d at "     \
                "0x%llx",                                                      \
                (int)(res)->fault_kind, (unsigned long long)(res)->fault_addr, \
                (int)(want_kind), (unsigned long long)(uint64_t)(want_addr));  \
    } while (0)

/* Compare a captured register field unsigned: ASSERT_EMU_REG_EQ(&r, rax, 42)
 * (x86) or ASSERT_EMU_REG_EQ(&r, x[0], 42) (other guests' result structs). */
#define ASSERT_EMU_REG_EQ(res, field, val)                                     \
    do {                                                                       \
        unsigned long long asmtest_g_ = (unsigned long long)(res)->regs.field; \
        unsigned long long asmtest_w_ = (unsigned long long)(val);             \
        if (asmtest_g_ != asmtest_w_)                                          \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_EMU_REG_EQ(%s): 0x%llx vs 0x%llx", #field,    \
                         asmtest_g_, asmtest_w_);                              \
    } while (0)

/* The scalar double return (xmm[0].f64[0]) equals `expected` exactly. */
#define ASSERT_EMU_FP_EQ(res, expected)                                        \
    do {                                                                       \
        double asmtest_g_ = (res)->regs.xmm[0].f64[0];                         \
        double asmtest_w_ = (expected);                                        \
        if (!(asmtest_g_ == asmtest_w_))                                       \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_EMU_FP_EQ: %.17g vs %.17g", asmtest_g_,       \
                         asmtest_w_);                                          \
    } while (0)

/* Bytewise-compare a captured 128-bit vector (an emu_vec128_t *, from any
 * guest's register file) to 16 expected bytes. */
#define ASSERT_EMU_VEC128_EQ(vec_ptr, expect_ptr)                              \
    do {                                                                       \
        const unsigned char *asmtest_a_ = (vec_ptr)->u8;                       \
        const unsigned char *asmtest_e_ = (const unsigned char *)(expect_ptr); \
        for (int asmtest_i_ = 0; asmtest_i_ < 16; asmtest_i_++)                \
            if (asmtest_a_[asmtest_i_] != asmtest_e_[asmtest_i_]) {            \
                asmtest_fail(__FILE__, __LINE__,                               \
                             "ASSERT_EMU_VEC128_EQ: first diff at byte %d "    \
                             "(0x%02x != 0x%02x)",                             \
                             asmtest_i_, asmtest_e_[asmtest_i_],               \
                             asmtest_a_[asmtest_i_]);                          \
                break;                                                         \
            }                                                                  \
    } while (0)

/* Bytewise-compare a captured x86 XMM register to 16 expected bytes. */
#define ASSERT_EMU_VEC_EQ(res, idx, expect_ptr)                                \
    ASSERT_EMU_VEC128_EQ(&(res)->regs.xmm[idx], (expect_ptr))

/* Basic-block offset `off` was covered by the (accumulated) trace. */
#define ASSERT_BLOCK_COVERED(trace, off)                                       \
    do {                                                                       \
        if (!emu_trace_covered((trace), (uint64_t)(off)))                      \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_BLOCK_COVERED: block offset 0x%llx not "      \
                         "covered",                                            \
                         (unsigned long long)(uint64_t)(off));                 \
    } while (0)

/* At least `n` distinct basic blocks were covered. */
#define ASSERT_BLOCKS_AT_LEAST(trace, n)                                       \
    do {                                                                       \
        size_t asmtest_bl_ = (trace)->blocks_len;                              \
        if (asmtest_bl_ < (size_t)(n))                                         \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_BLOCKS_AT_LEAST: %zu < %zu", asmtest_bl_,     \
                         (size_t)(n));                                         \
    } while (0)

/* Mid-execution guard assertions (Track F): check the emu_watch_t /            */
/* emu_reg_guard_t a guarded run filled. The failure names the offending        */
/* store's / block's offset; pair with emu_watch_describe for the instruction.  */

/* No guarded write violated the armed watchpoint (emu_watch_writes). */
#define ASSERT_NO_WRITE_VIOLATION(w)                                           \
    do {                                                                       \
        if ((w)->violated)                                                     \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_NO_WRITE_VIOLATION: write to 0x%llx (%u "     \
                         "bytes) at insn @0x%llx",                             \
                         (unsigned long long)(w)->addr, (unsigned)(w)->size,   \
                         (unsigned long long)(w)->rip_off);                    \
    } while (0)

/* A guarded write DID violate the watchpoint (expect a scribble). */
#define ASSERT_WRITE_VIOLATION(w)                                              \
    do {                                                                       \
        if (!(w)->violated)                                                    \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_WRITE_VIOLATION: expected a guarded write, "  \
                         "none occurred");                                     \
    } while (0)

/* The register invariant (emu_guard_reg) held at every basic-block entry. */
#define ASSERT_REG_INVARIANT(g)                                                \
    do {                                                                       \
        if ((g)->violated)                                                     \
            asmtest_fail(__FILE__, __LINE__,                                   \
                         "ASSERT_REG_INVARIANT: broke invariant (got 0x%llx) " \
                         "at block @0x%llx",                                   \
                         (unsigned long long)(g)->got,                         \
                         (unsigned long long)(g)->rip_off);                    \
    } while (0)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ASMTEST_EMU_H */
