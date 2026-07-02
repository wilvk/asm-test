/*
 * emu.c — Unicorn-Engine-backed emulator tier (Phase 4+), with x86-64,
 * AArch64, RISC-V (RV64), and ARM32 guests.
 */
#include "asmtest_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* Guest memory layout (arbitrary, just needs to be mapped and non-overlapping).
 * EMU_CODE_BASE is published in asmtest_emu.h (the assembler shares it). */
#define EMU_CODE_SIZE  0x00010000UL /* 64 KiB of code space  */
#define EMU_STACK_BASE 0x00200000UL
#define EMU_STACK_SIZE 0x00010000UL /* 64 KiB stack          */
#define EMU_RET_MAGIC  0x00f00000UL /* return target; unmapped, only a stop addr */

/* A memory-write watchpoint armed on the handle (emu_watch_writes): subsequent
 * runs flag the first valid write that violates the [lo,hi) region per `mode`. */
typedef struct {
    bool armed;
    uint64_t lo, hi; /* the guarded region [lo, hi)                     */
    emu_watch_mode_t mode;
    emu_watch_t *out; /* caller's result (NULL == disarmed)             */
} watch_ctx_t;

/* A register invariant armed on the handle (emu_guard_reg): subsequent runs flag
 * the first basic-block entry at which `uc_reg` no longer holds `want`. */
typedef struct {
    bool armed;
    int uc_reg; /* UC_X86_REG_*                                          */
    uint64_t want;
    emu_reg_guard_t *out;
} reg_guard_ctx_t;

struct emu {
    uc_engine *uc;
    watch_ctx_t watch; /* mid-execution memory-write guard (emu_watch_*)  */
    reg_guard_ctx_t reg; /* mid-execution register invariant (emu_guard_*) */
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

/* UC_HOOK_CODE: fires once per executed instruction — the ordered trace.
 * The append/truncate logic is the engine-neutral trace_append_insn (trace.c),
 * shared with the DynamoRIO and hardware-trace backends. */
static void on_code(uc_engine *uc, uint64_t address, uint32_t size,
                    void *user) {
    (void)uc;
    (void)size;
    trace_ctx_t *c = (trace_ctx_t *)user;
    trace_append_insn(c->t, address - c->base);
}

/* UC_HOOK_BLOCK: fires at each basic-block entry — the coverage set. The
 * distinct-block dedup + truncate logic is the engine-neutral
 * trace_append_block (trace.c). */
static void on_block(uc_engine *uc, uint64_t address, uint32_t size,
                     void *user) {
    (void)uc;
    (void)size;
    trace_ctx_t *c = (trace_ctx_t *)user;
    trace_append_block(c->t, address - c->base);
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

/* UC_HOOK_MEM_WRITE: every VALID write. Flags the first one that violates the
 * armed watchpoint — a write touching a NEVER region, or escaping an ONLY
 * region — recording its data address, width, and the offending store's offset.
 * (A write to UNMAPPED memory faults instead, via on_invalid_mem.) */
static void on_mem_write(uc_engine *uc, uc_mem_type type, uint64_t addr,
                         int size, int64_t value, void *user) {
    (void)type;
    (void)value;
    watch_ctx_t *w = (watch_ctx_t *)user;
    if (w->out == NULL || w->out->violated)
        return;
    uint64_t end = addr + (uint64_t)size;
    bool touches = addr < w->hi && end > w->lo;       /* overlaps [lo,hi)  */
    bool contained = addr >= w->lo && end <= w->hi;   /* fully inside      */
    bool bad = (w->mode == EMU_WATCH_NEVER) ? touches : !contained;
    if (!bad)
        return;
    uint64_t rip = 0;
    uc_reg_read(uc, UC_X86_REG_RIP, &rip);
    w->out->violated = true;
    w->out->addr = addr;
    w->out->size = (uint32_t)size;
    w->out->rip_off = rip - EMU_CODE_BASE;
}

/* UC_HOOK_BLOCK companion: checks the armed register invariant at each block
 * entry, recording the first breach (the value seen + that block's offset). */
static void on_block_guard(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user) {
    (void)size;
    reg_guard_ctx_t *g = (reg_guard_ctx_t *)user;
    if (g->out == NULL || g->out->violated)
        return;
    uint64_t v = 0;
    uc_reg_read(uc, g->uc_reg, &v);
    if (v != g->want) {
        g->out->violated = true;
        g->out->got = v;
        g->out->rip_off = address - EMU_CODE_BASE;
    }
}

/* Load a routine's bytes at EMU_CODE_BASE and flush Unicorn's translation-block
 * cache for it. The flush matters when an emulator handle is reused for a
 * different routine at the same address: without it, Unicorn would re-run the
 * previously translated (now stale) code instead of the bytes just written. */
static bool load_code(uc_engine *uc, const void *code, size_t code_len) {
    if (uc_mem_write(uc, EMU_CODE_BASE, code, code_len) != UC_ERR_OK)
        return false;
    /* Drop any cached translation for the code region so a reused handle
     * re-decodes the freshly written bytes. uc_ctl_flush_tb is newer
     * (Unicorn >= 2.1); fall back to uc_ctl_remove_cache (since 2.0.0) so we
     * build against the older libunicorn shipped by some distros. */
#if defined(uc_ctl_flush_tb)
    uc_ctl_flush_tb(uc);
#elif defined(uc_ctl_remove_cache)
    uc_ctl_remove_cache(uc, EMU_CODE_BASE, EMU_CODE_BASE + EMU_CODE_SIZE);
#endif
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
/* Zero the GP + vector register file and reset RFLAGS before a call, so a routine
 * that reads a register/lane the caller did not set gets a deterministic 0 rather
 * than the previous call's state on a reused handle (all bindings hold one
 * long-lived handle across many calls). Guest MAPPED memory is intentionally
 * preserved — emu_map/emu_write preload data for the call — and the internal
 * stack is re-based per call by the setup. */
static void emu_x86_zero_regs(uc_engine *uc) {
    static const int gp[] = {
        UC_X86_REG_RAX, UC_X86_REG_RBX, UC_X86_REG_RCX, UC_X86_REG_RDX,
        UC_X86_REG_RSI, UC_X86_REG_RDI, UC_X86_REG_RBP, UC_X86_REG_R8,
        UC_X86_REG_R9,  UC_X86_REG_R10, UC_X86_REG_R11, UC_X86_REG_R12,
        UC_X86_REG_R13, UC_X86_REG_R14, UC_X86_REG_R15};
    uint64_t z = 0;
    for (size_t i = 0; i < sizeof gp / sizeof gp[0]; i++)
        uc_reg_write(uc, gp[i], &z);
    uint8_t zv[16] = {0};
    for (int i = 0; i < 16; i++)
        uc_reg_write(uc, UC_X86_REG_XMM0 + i, zv);
    uint64_t flags = 2; /* x86 EFLAGS reserved bit 1 is always set */
    uc_reg_write(uc, UC_X86_REG_EFLAGS, &flags);
}

static bool emu_x86_setup_sysv(uc_engine *uc, const void *fn, size_t code_len,
                               const long *iargs, int niargs) {
    static const int arg_regs[6] = {UC_X86_REG_RDI, UC_X86_REG_RSI,
                                    UC_X86_REG_RDX, UC_X86_REG_RCX,
                                    UC_X86_REG_R8,  UC_X86_REG_R9};
    if (!load_code(uc, fn, code_len))
        return false;
    emu_x86_zero_regs(uc); /* deterministic start: clear stale reg state */
    /* SysV places INTEGER args beyond the sixth on the stack starting at
     * [rsp+8] at entry (psABI §3.2.3). Reserve room above the return address for
     * them, rounding the reservation to 16 bytes so rsp stays ≡ 8 (mod 16) at
     * entry (16-aligned after the simulated call). */
    int nstack = niargs > 6 ? niargs - 6 : 0;
    uint64_t pad = ((uint64_t)nstack * 8 + 15) & ~(uint64_t)15;
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 8 - pad;
    uint64_t ret = EMU_RET_MAGIC;
    uc_mem_write(uc, sp, &ret, sizeof ret);
    for (int i = 6; i < niargs; i++) {
        uint64_t v = (uint64_t)iargs[i];
        uc_mem_write(uc, sp + 8 + 8 * (uint64_t)(i - 6), &v, sizeof v);
    }
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
static bool emu_x86_run(emu_t *e, uint64_t max_insns, emu_result_t *out,
                        emu_trace_t *trace) {
    uc_engine *uc = e->uc;
    fault_rec_t fr = {0};
    uc_hook hh;
    uc_hook_add(uc, &hh, UC_HOOK_MEM_INVALID, (void *)on_invalid_mem, &fr, 1,
                0);
    trace_ctx_t tc = {trace, EMU_CODE_BASE};
    uc_hook hcode, hblock;
    add_trace_hooks(uc, &tc, &hcode, &hblock);

    /* Mid-execution guards armed on the handle (emu_watch_writes/emu_guard_reg);
     * each resets its result for this run, then records the first violation. */
    uc_hook hwrite = 0, hguard = 0;
    if (e->watch.armed && e->watch.out != NULL) {
        e->watch.out->violated = false;
        uc_hook_add(uc, &hwrite, UC_HOOK_MEM_WRITE, (void *)on_mem_write,
                    &e->watch, 1, 0);
    }
    if (e->reg.armed && e->reg.out != NULL) {
        e->reg.out->violated = false;
        uc_hook_add(uc, &hguard, UC_HOOK_BLOCK, (void *)on_block_guard, &e->reg,
                    1, 0);
    }

    uc_err err =
        uc_emu_start(uc, EMU_CODE_BASE, EMU_RET_MAGIC, 0, (size_t)max_insns);
    uc_hook_del(uc, hh);
    if (trace != NULL) {
        uc_hook_del(uc, hcode);
        uc_hook_del(uc, hblock);
    }
    if (hwrite)
        uc_hook_del(uc, hwrite);
    if (hguard)
        uc_hook_del(uc, hguard);

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
    return emu_x86_run(e, max_insns, out, trace);
}

bool emu_call(emu_t *e, const void *fn, size_t code_len, const long *args,
              int nargs, uint64_t max_insns, emu_result_t *out) {
    return emu_call_traced(e, fn, code_len, args, nargs, max_insns, out, NULL);
}

/* Scalar-arg convenience for dynamic-FFI bindings (see ffi.c): run `fn` (a
 * 64-byte code window) with two integer args; returns 1 if it ran, 0 otherwise.
 * Faults land in *out (read via asmtest_emu_result_faulted / _x86_reg). */
int asmtest_emu_call2(emu_t *e, const void *fn, long a0, long a1,
                      emu_result_t *out) {
    long args[2] = {a0, a1};
    return emu_call(e, fn, 64, args, 2, 0, out) ? 1 : 0;
}

/* Like asmtest_emu_call2, but up to six integer args (nargs selects how many,
 * clamped to [0,6]) and an optional max_insns cap — so a binding can drive the
 * emulator with the full SysV integer-arg set, not just two. */
int asmtest_emu_call6(emu_t *e, const void *fn, long a0, long a1, long a2,
                      long a3, long a4, long a5, int nargs, uint64_t max_insns,
                      emu_result_t *out) {
    long args[6] = {a0, a1, a2, a3, a4, a5};
    if (nargs < 0)
        nargs = 0;
    if (nargs > 6)
        nargs = 6;
    return emu_call(e, fn, 64, args, nargs, max_insns, out) ? 1 : 0;
}

/* Two double args marshalled into xmm0/xmm1 (the SysV FP arg registers); the
 * scalar double return lands in xmm[0].f64[0] (read via asmtest_emu_x86_xmm_f64).
 * The FP analog of asmtest_emu_call2. */
int asmtest_emu_call_fp2(emu_t *e, const void *fn, double f0, double f1,
                         emu_result_t *out) {
    long iargs[6] = {0, 0, 0, 0, 0, 0};
    double fargs[2] = {f0, f1};
    return emu_call_fp(e, fn, 64, iargs, 0, fargs, 2, 0, out) ? 1 : 0;
}

/* nvec 128-bit vector args as a flat 4*nvec float32 array (mirrors
 * asmtest_capture_vec_f32) marshalled into xmm0..7; the whole XMM file is
 * captured into the result (a vector return is xmm[0]). nvec is clamped [0,8]. */
int asmtest_emu_call_vec_f32(emu_t *e, const void *fn, const float *lanes,
                             int nvec, emu_result_t *out) {
    long iargs[6] = {0, 0, 0, 0, 0, 0};
    emu_vec128_t vargs[8];
    memset(vargs, 0, sizeof vargs);
    if (nvec < 0)
        nvec = 0;
    if (nvec > 8)
        nvec = 8;
    for (int i = 0; i < nvec; i++)
        for (int lane = 0; lane < 4; lane++)
            vargs[i].f32[lane] = lanes[i * 4 + lane];
    return emu_call_vec(e, fn, 64, iargs, 0, vargs, nvec, 0, out) ? 1 : 0;
}

/* Run `fn` under the Microsoft x64 (Win64) convention with up to four integer
 * args in rcx, rdx, r8, r9 (nargs clamped [0,4]); the return is rax. Lets a
 * binding test a Win64 routine on a System V host. */
int asmtest_emu_call_win64_6(emu_t *e, const void *fn, long a0, long a1, long a2,
                             long a3, int nargs, uint64_t max_insns,
                             emu_result_t *out) {
    long args[4] = {a0, a1, a2, a3};
    if (nargs < 0)
        nargs = 0;
    if (nargs > 4)
        nargs = 4;
    return emu_call_win64(e, fn, 64, args, nargs, max_insns, out) ? 1 : 0;
}

/* Like asmtest_emu_call6, but records an execution trace / basic-block coverage
 * into `trace` (an opaque handle from asmtest_emu_trace_new; may be NULL to
 * behave exactly like asmtest_emu_call6). */
int asmtest_emu_call6_traced(emu_t *e, const void *fn, long a0, long a1, long a2,
                             long a3, long a4, long a5, int nargs,
                             uint64_t max_insns, emu_result_t *out,
                             emu_trace_t *trace) {
    long args[6] = {a0, a1, a2, a3, a4, a5};
    if (nargs < 0)
        nargs = 0;
    if (nargs > 6)
        nargs = 6;
    return emu_call_traced(e, fn, 64, args, nargs, max_insns, out, trace) ? 1 : 0;
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
    return emu_x86_run(e, max_insns, out, NULL);
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
    return emu_x86_run(e, max_insns, out, NULL);
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
    emu_x86_zero_regs(uc); /* deterministic start: clear stale reg state */

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

    return emu_x86_run(e, max_insns, out, trace);
}

bool emu_call_win64(emu_t *e, const void *fn, size_t code_len, const long *args,
                    int nargs, uint64_t max_insns, emu_result_t *out) {
    return emu_call_win64_traced(e, fn, code_len, args, nargs, max_insns, out,
                                 NULL);
}

/* ------------------------------------------------------------------ */
/* Mid-execution guards (x86-64 guest, Track F)                        */
/*                                                                     */
/* Watchpoints and register invariants are armed ON THE HANDLE and     */
/* persist across emu_call_* until cleared, so the same guard can span  */
/* a sweep of inputs. Each run resets its result, then records the      */
/* FIRST violation as data (no host crash) — the introspection no       */
/* ABI-boundary tool can do. Pair the recorded offset with             */
/* emu_watch_describe (disasm.c) for the offending instruction's text.  */
/* ------------------------------------------------------------------ */

/* Map an x86-64 integer register name ("rax".."r15", "rsp"/"rbp"/"rip") to its
 * UC_X86_REG_* id; -1 for an unknown name. */
static int x86_uc_reg(const char *name) {
    static const struct {
        const char *n;
        int r;
    } map[] = {
        {"rax", UC_X86_REG_RAX}, {"rbx", UC_X86_REG_RBX},
        {"rcx", UC_X86_REG_RCX}, {"rdx", UC_X86_REG_RDX},
        {"rsi", UC_X86_REG_RSI}, {"rdi", UC_X86_REG_RDI},
        {"rbp", UC_X86_REG_RBP}, {"rsp", UC_X86_REG_RSP},
        {"r8", UC_X86_REG_R8},   {"r9", UC_X86_REG_R9},
        {"r10", UC_X86_REG_R10}, {"r11", UC_X86_REG_R11},
        {"r12", UC_X86_REG_R12}, {"r13", UC_X86_REG_R13},
        {"r14", UC_X86_REG_R14}, {"r15", UC_X86_REG_R15},
        {"rip", UC_X86_REG_RIP},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(name, map[i].n) == 0)
            return map[i].r;
    return -1;
}

void emu_watch_writes(emu_t *e, uint64_t addr, size_t size,
                      emu_watch_mode_t mode, emu_watch_t *out) {
    if (e == NULL)
        return;
    e->watch.armed = true;
    e->watch.lo = addr;
    e->watch.hi = addr + size;
    e->watch.mode = mode;
    e->watch.out = out;
    if (out != NULL) {
        out->violated = false;
        out->addr = 0;
        out->size = 0;
        out->rip_off = 0;
    }
}

void emu_watch_clear(emu_t *e) {
    if (e == NULL)
        return;
    e->watch.armed = false;
    e->watch.out = NULL;
}

bool emu_guard_reg(emu_t *e, const char *regname, uint64_t want,
                   emu_reg_guard_t *out) {
    if (e == NULL || regname == NULL)
        return false;
    int r = x86_uc_reg(regname);
    if (r < 0)
        return false;
    e->reg.armed = true;
    e->reg.uc_reg = r;
    e->reg.want = want;
    e->reg.out = out;
    if (out != NULL) {
        out->violated = false;
        out->got = 0;
        out->rip_off = 0;
    }
    return true;
}

void emu_guard_reg_clear(emu_t *e) {
    if (e == NULL)
        return;
    e->reg.armed = false;
    e->reg.out = NULL;
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
 * in x0..x7 (AAPCS64 stage C rule C.9 assigns the first eight to r0..r7). Returns
 * false only on a code-write failure. */
/* AArch64 analog of emu_x86_zero_regs: clear x0..x28, the frame pointer, NZCV, and
 * the whole v0..v31 file before a call so a reused handle starts deterministic. */
static void emu_arm64_zero_regs(uc_engine *uc) {
    uint64_t z = 0;
    for (int r = UC_ARM64_REG_X0; r <= UC_ARM64_REG_X28; r++)
        uc_reg_write(uc, r, &z);
    uc_reg_write(uc, UC_ARM64_REG_X29, &z); /* FP; x30/SP set by the setup */
    uc_reg_write(uc, UC_ARM64_REG_NZCV, &z);
    uint8_t zv[16] = {0};
    for (int i = 0; i < 32; i++)
        uc_reg_write(uc, UC_ARM64_REG_V0 + i, zv);
}

static bool emu_arm64_setup(uc_engine *uc, const void *code, size_t code_len,
                            const long *iargs, int niargs) {
    static const int arg_regs[8] = {UC_ARM64_REG_X0, UC_ARM64_REG_X1,
                                    UC_ARM64_REG_X2, UC_ARM64_REG_X3,
                                    UC_ARM64_REG_X4, UC_ARM64_REG_X5,
                                    UC_ARM64_REG_X6, UC_ARM64_REG_X7};
    if (!load_code(uc, code, code_len))
        return false;
    emu_arm64_zero_regs(uc); /* deterministic start: clear stale reg state */
    uint64_t sp = EMU_STACK_BASE + EMU_STACK_SIZE - 16; /* 16-aligned */
    uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
    uint64_t lr = EMU_RET_MAGIC; /* `ret` branches to lr */
    uc_reg_write(uc, UC_ARM64_REG_X30, &lr);
    for (int i = 0; i < niargs && i < 8; i++) {
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

bool emu_arm_call_vec(emu_arm_t *e, const void *code, size_t code_len,
                      const long *iargs, int niargs,
                      const emu_vec128_t *vargs, int nvargs,
                      uint64_t max_insns, emu_arm_result_t *out) {
    /* AAPCS-VFP: 128-bit vector args go in q0..q3 (q0..q15 are consecutive
     * Unicorn register ids). The whole q file is captured by read_all_regs_arm,
     * which reads d0..d31 back into q[0..15]. */
    uc_engine *uc = e->uc;
    memset(out, 0, sizeof *out);
    if (!emu_arm_setup(uc, code, code_len, iargs, niargs)) {
        out->uc_err = -1;
        return false;
    }
    for (int i = 0; i < nvargs && i < 4; i++)
        uc_reg_write(uc, UC_ARM_REG_Q0 + i, (void *)vargs[i].u8);
    return emu_arm_run(uc, max_insns, out, NULL);
}

/* RISC-V has no emu_riscv_call_vec: the RISC-V "V" (vector) extension would be
 * the analogue, but Unicorn's RISC-V guest exposes no vector registers
 * (UC_RISCV_REG_V0.. do not exist) and no vtype/vl CSRs, so there is no
 * register interface to marshal vector args into or capture them from. Vector
 * marshalling is therefore implemented for the x86-64 (xmm), AArch64 (NEON v),
 * and ARM32 (NEON q) guests only; RISC-V stays scalar-FP (emu_riscv_call_fp).
 * See docs/emulator.md and docs/plans/expansion-plan.md (Track C). */

/* ------------------------------------------------------------------ */
/* Coverage reporting & source-line mapping (Track C)                  */
/*                                                                     */
/* emu_trace_covered, emu_trace_report, emu_coverage_uncovered,        */
/* emu_trace_lcov, emu_line_lookup, emu_trace_source_report, and       */
/* emu_trace_lcov_source were extracted into the engine-neutral         */
/* src/trace.c (declared in asmtest_trace.h) so the DynamoRIO and       */
/* hardware-trace backends reuse them without linking the Unicorn       */
/* emulator object. They operate purely on the recorded trace.          */
/* ------------------------------------------------------------------ */
