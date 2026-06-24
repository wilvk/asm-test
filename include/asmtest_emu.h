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
/* Tracing is opt-in and APPENDS: zero the struct, point insns/blocks  */
/* at caller-owned buffers (either may stay NULL to skip that          */
/* dimension), then run via emu_call_traced. Re-running with the same  */
/* struct accumulates, so the union of basic blocks across many inputs */
/* answers "did the tests reach every block?". Addresses are recorded  */
/* as byte offsets from the start of the routine (offset 0 = entry).   */
/* ------------------------------------------------------------------ */
typedef struct {
    /* Ordered instruction trace (UC_HOOK_CODE): each executed instruction's
     * offset, in execution order, up to insns_cap entries. */
    uint64_t *insns;
    size_t insns_cap;
    size_t insns_len;      /* entries written to insns[] (<= insns_cap)      */
    uint64_t insns_total;  /* instructions executed (counts past insns_cap)  */

    /* Basic-block coverage (UC_HOOK_BLOCK): the DISTINCT block-start offsets
     * entered, de-duplicated, up to blocks_cap entries. */
    uint64_t *blocks;
    size_t blocks_cap;
    size_t blocks_len;     /* distinct blocks recorded (<= blocks_cap)       */
    uint64_t blocks_total; /* block entries; a loop counts each pass         */

    bool truncated;        /* a buffer filled and at least one entry dropped */
} emu_trace_t;

/* Like emu_call, but also records an execution trace / coverage into *trace
 * (which may be NULL to behave exactly like emu_call). See emu_trace_t. */
bool emu_call_traced(emu_t *e, const void *fn, size_t code_len,
                     const long *args, int nargs, uint64_t max_insns,
                     emu_result_t *out, emu_trace_t *trace);

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

/* Like emu_arm64_call, but also records a trace / coverage into *trace (which
 * may be NULL). Offsets are byte offsets from the start of the code; AArch64
 * instructions are 4 bytes, so they fall on 0, 4, 8, ... See emu_trace_t. */
bool emu_arm64_call_traced(emu_arm64_t *e, const void *code, size_t code_len,
                           const long *args, int nargs, uint64_t max_insns,
                           emu_arm64_result_t *out, emu_trace_t *trace);

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
    uint64_t x[32]; /* x0..x31; x0 = zero, x1 = ra, x2 = sp, x10..x17 = a0..a7 */
    uint64_t pc;
} emu_riscv_regs_t;

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
} emu_arm_regs_t;

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

/* ------------------------------------------------------------------ */
/* Coverage reporting (Track C)                                        */
/*                                                                     */
/* Helpers over an accumulated emu_trace_t (see emu_call_traced). All  */
/* are arch-neutral: a trace records block byte-offsets from the       */
/* routine entry, whatever the guest.                                  */
/* ------------------------------------------------------------------ */

/* True if basic-block offset `off` appears in the trace's distinct block set. */
bool emu_trace_covered(const emu_trace_t *t, uint64_t off);

/* Print a human-readable coverage summary for `t` to `out`: distinct blocks,
 * total block entries (loops re-count), instruction count, and the sorted
 * covered offsets. */
void emu_trace_report(const emu_trace_t *t, FILE *out);

/* Print the block offsets present in `universe` (e.g. a trace accumulated over
 * all inputs) but absent from `covered`, plus an "X/Y covered" line, to `out`.
 * Returns the number of uncovered blocks (0 = full coverage of the universe). */
size_t emu_coverage_uncovered(const emu_trace_t *covered,
                              const emu_trace_t *universe, FILE *out);

/* Emit an lcov-style .info record for `t` to `out`. Without debug info the
 * framework has no source lines, so block byte-offsets stand in for line
 * numbers (offset-level coverage); `name` fills the SF: source-file field. */
void emu_trace_lcov(const emu_trace_t *t, const char *name, FILE *out);

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

/* The run completed without an invalid memory access (and the engine was OK). */
#define ASSERT_NO_FAULT(res)                                                  \
    do {                                                                      \
        const emu_result_t *asmtest_r_ = (res);                              \
        if (asmtest_r_->faulted)                                             \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_NO_FAULT: faulted at 0x%llx (kind %d)",      \
                         (unsigned long long)asmtest_r_->fault_addr,          \
                         (int)asmtest_r_->fault_kind);                        \
        else if (asmtest_r_->uc_err != 0)                                    \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_NO_FAULT: engine error %d",                  \
                         asmtest_r_->uc_err);                                 \
    } while (0)

/* The run hit an invalid memory access. */
#define ASSERT_FAULT(res)                                                     \
    do {                                                                      \
        if (!(res)->faulted)                                                 \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_FAULT: expected a fault, none occurred");    \
    } while (0)

/* The run faulted with a specific kind (EMU_FAULT_READ/WRITE/FETCH) at addr. */
#define ASSERT_FAULT_AT(res, want_kind, want_addr)                            \
    do {                                                                      \
        const emu_result_t *asmtest_r_ = (res);                              \
        if (!asmtest_r_->faulted)                                            \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_FAULT_AT: expected a fault, none occurred"); \
        else if (asmtest_r_->fault_kind != (want_kind) ||                    \
                 asmtest_r_->fault_addr != (uint64_t)(want_addr))            \
            asmtest_fail(                                                     \
                __FILE__, __LINE__,                                          \
                "ASSERT_FAULT_AT: got kind %d at 0x%llx, want kind %d at "    \
                "0x%llx",                                                     \
                (int)asmtest_r_->fault_kind,                                 \
                (unsigned long long)asmtest_r_->fault_addr, (int)(want_kind), \
                (unsigned long long)(uint64_t)(want_addr));                  \
    } while (0)

/* Compare a captured register field unsigned: ASSERT_EMU_REG_EQ(&r, rax, 42)
 * (x86) or ASSERT_EMU_REG_EQ(&r, x[0], 42) (other guests' result structs). */
#define ASSERT_EMU_REG_EQ(res, field, val)                                    \
    do {                                                                      \
        unsigned long long asmtest_g_ =                                       \
            (unsigned long long)(res)->regs.field;                            \
        unsigned long long asmtest_w_ = (unsigned long long)(val);            \
        if (asmtest_g_ != asmtest_w_)                                        \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_EMU_REG_EQ(%s): 0x%llx vs 0x%llx", #field,   \
                         asmtest_g_, asmtest_w_);                             \
    } while (0)

/* The scalar double return (xmm[0].f64[0]) equals `expected` exactly. */
#define ASSERT_EMU_FP_EQ(res, expected)                                       \
    do {                                                                      \
        double asmtest_g_ = (res)->regs.xmm[0].f64[0];                       \
        double asmtest_w_ = (expected);                                      \
        if (!(asmtest_g_ == asmtest_w_))                                     \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_EMU_FP_EQ: %.17g vs %.17g", asmtest_g_,      \
                         asmtest_w_);                                         \
    } while (0)

/* Bytewise-compare a captured XMM register to 16 expected bytes. */
#define ASSERT_EMU_VEC_EQ(res, idx, expect_ptr)                               \
    do {                                                                      \
        const unsigned char *asmtest_a_ = (res)->regs.xmm[idx].u8;           \
        const unsigned char *asmtest_e_ =                                    \
            (const unsigned char *)(expect_ptr);                             \
        for (int asmtest_i_ = 0; asmtest_i_ < 16; asmtest_i_++)              \
            if (asmtest_a_[asmtest_i_] != asmtest_e_[asmtest_i_]) {          \
                asmtest_fail(__FILE__, __LINE__,                              \
                             "ASSERT_EMU_VEC_EQ(xmm[%s]): first diff at "     \
                             "byte %d (0x%02x != 0x%02x)",                    \
                             #idx, asmtest_i_, asmtest_e_[asmtest_i_],        \
                             asmtest_a_[asmtest_i_]);                         \
                break;                                                        \
            }                                                                 \
    } while (0)

/* Basic-block offset `off` was covered by the (accumulated) trace. */
#define ASSERT_BLOCK_COVERED(trace, off)                                      \
    do {                                                                      \
        if (!emu_trace_covered((trace), (uint64_t)(off)))                    \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_BLOCK_COVERED: block offset 0x%llx not "     \
                         "covered",                                           \
                         (unsigned long long)(uint64_t)(off));               \
    } while (0)

/* At least `n` distinct basic blocks were covered. */
#define ASSERT_BLOCKS_AT_LEAST(trace, n)                                      \
    do {                                                                      \
        size_t asmtest_bl_ = (trace)->blocks_len;                            \
        if (asmtest_bl_ < (size_t)(n))                                       \
            asmtest_fail(__FILE__, __LINE__,                                  \
                         "ASSERT_BLOCKS_AT_LEAST: %zu < %zu", asmtest_bl_,    \
                         (size_t)(n));                                        \
    } while (0)

#endif /* ASMTEST_EMU_H */
