/*
 * ffi.c — opaque-handle FFI convenience layer for dynamic-language bindings.
 *
 * The struct-layout path (mirror regs_t / emu_result_t from the asmtest_abi.json
 * manifest) is ideal for languages that can lay out C structs — C++, Rust, Zig.
 * Dynamic-FFI languages (Node, Ruby, Lua, …) are happier calling functions over
 * scalars and pointers than computing field offsets or marshalling C arrays. So
 * these helpers expose the same capability through the universal FFI subset:
 * allocate an opaque handle, call with scalar args, read fields by accessor.
 * They live only in the shared libraries (not the static lib / test binaries).
 *
 * Nothing here depends on Unicorn — the emu_result_t accessors only read struct
 * fields — so this object links into both libasmtest and libasmtest_emu. The one
 * helper that drives the emulator (asmtest_emu_call2) lives in emu.c.
 */
#include "asmtest.h"
#include "asmtest_emu.h"

#include <stdlib.h>
#include <string.h>

/* ---- regs_t opaque handle + accessors ---- */
regs_t *asmtest_regs_new(void) {
    return (regs_t *)calloc(1, sizeof(regs_t));
}
void asmtest_regs_free(regs_t *r) {
    free(r);
}
unsigned long asmtest_regs_ret(const regs_t *r) {
    return r->ret;
}
unsigned long asmtest_regs_flags(const regs_t *r) {
    return r->flags;
}
double asmtest_regs_fret(const regs_t *r) {
    return r->fret;
}

/* Read a single vector lane (float32) from a captured vector register. */
float asmtest_regs_vec_f32(const regs_t *r, int index, int lane) {
    if (index < 0 || lane < 0 || lane > 3)
        return 0.0f;
    return r->vec[index].f32[lane];
}

/* Condition flag by name, resolved to the host arch's mask — so a binding needs
 * no flag-mask constants. Returns 1 if set, 0 otherwise (or unknown name). */
int asmtest_regs_flag_set(const regs_t *r, const char *name) {
    unsigned long m = 0;
    if (!name)
        return 0;
#if defined(__x86_64__)
    if (!strcmp(name, "CF"))
        m = ASMTEST_CF;
    else if (!strcmp(name, "PF"))
        m = ASMTEST_PF;
    else if (!strcmp(name, "ZF"))
        m = ASMTEST_ZF;
    else if (!strcmp(name, "SF"))
        m = ASMTEST_SF;
    else if (!strcmp(name, "OF"))
        m = ASMTEST_OF;
#elif defined(__aarch64__)
    if (!strcmp(name, "VF"))
        m = ASMTEST_VF;
    else if (!strcmp(name, "CF"))
        m = ASMTEST_CF;
    else if (!strcmp(name, "ZF"))
        m = ASMTEST_ZF;
    else if (!strcmp(name, "NF"))
        m = ASMTEST_NF;
#endif
    return (m && (r->flags & m)) ? 1 : 0;
}

/* ---- scalar-arg capture wrappers (no array marshalling needed) ---- */
void asmtest_capture6(regs_t *out, void *fn, long a0, long a1, long a2, long a3,
                      long a4, long a5) {
    long args[6] = {a0, a1, a2, a3, a4, a5};
    asm_call_capture(out, fn, args);
}
void asmtest_capture_fp2(regs_t *out, void *fn, double f0, double f1) {
    long iargs[6] = {0, 0, 0, 0, 0, 0};
    double fargs[8] = {f0, f1, 0, 0, 0, 0, 0, 0};
    asm_call_capture_fp(out, fn, iargs, fargs);
}

/* Vector capture for the opaque-handle bindings, completing the trampoline
 * surface alongside capture6 (int) and capture_fp2 (fp). `lanes` is a flat array
 * of 4*nvec float32s — nvec vector args, each four lanes, packed contiguously and
 * little-endian — so a binding marshals only a scalar array, never a vec128_t
 * (the struct-layout bindings call asm_call_capture_vec directly instead). The
 * whole vector register file is captured into out->vec[]; the vector return is
 * out->vec[0], read back lane by lane with asmtest_regs_vec_f32. nvec is clamped
 * to [0, 8] (the number of vector argument registers). */
void asmtest_capture_vec_f32(regs_t *out, void *fn, const float *lanes,
                             int nvec) {
    long iargs[6] = {0, 0, 0, 0, 0, 0};
    vec128_t vargs[8];
    memset(vargs, 0, sizeof(vargs));
    if (nvec < 0)
        nvec = 0;
    if (nvec > 8)
        nvec = 8;
    for (int i = 0; i < nvec; i++)
        for (int lane = 0; lane < 4; lane++)
            vargs[i].f32[lane] = lanes[i * 4 + lane];
    asm_call_capture_vec(out, fn, iargs, vargs);
}

/* ---- emu_result_t opaque handle + accessors (no Unicorn dependency) ---- */
emu_result_t *asmtest_emu_result_new(void) {
    return (emu_result_t *)calloc(1, sizeof(emu_result_t));
}
void asmtest_emu_result_free(emu_result_t *r) {
    free(r);
}
int asmtest_emu_result_ok(const emu_result_t *r) {
    return r->ok ? 1 : 0;
}
int asmtest_emu_result_faulted(const emu_result_t *r) {
    return r->faulted ? 1 : 0;
}

/* Faulting guest address — only meaningful when asmtest_emu_result_faulted. */
unsigned long long asmtest_emu_result_fault_addr(const emu_result_t *r) {
    return (unsigned long long)r->fault_addr;
}

/* Kind of invalid access as an int (emu_fault_kind_t): 0 none, 1 read, 2 write,
 * 3 fetch — so a binding reports where and why a fault hit, not just that it did. */
int asmtest_emu_result_fault_kind(const emu_result_t *r) {
    return (int)r->fault_kind;
}

/* x86-64 guest register by name. The 16 GP fields plus rip/rflags are laid out
 * contiguously in emu_x86_regs_t, so the whole 64-bit file reads as one array —
 * a binding can pull the instruction pointer and flags, not only rax..r15. */
unsigned long long asmtest_emu_x86_reg(const emu_result_t *r, const char *name) {
    static const char *const n[18] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                      "rbp", "rsp", "r8",  "r9",  "r10", "r11",
                                      "r12", "r13", "r14", "r15", "rip", "rflags"};
    const uint64_t *p = &r->regs.rax;
    if (!name)
        return 0;
    for (int i = 0; i < 18; i++)
        if (!strcmp(name, n[i]))
            return (unsigned long long)p[i];
    return 0;
}

/* A guest XMM lane after the run — the FP/vector side of the register file, which
 * the GP-only asmtest_emu_x86_reg can't reach. A scalar double return lands in
 * xmm[0].f64[0]; a 128-bit vector return in xmm[0]. index selects the register
 * (0..15), lane the element. Out-of-range reads return 0. */
double asmtest_emu_x86_xmm_f64(const emu_result_t *r, int index, int lane) {
    if (index < 0 || index > 15 || lane < 0 || lane > 1)
        return 0.0;
    return r->regs.xmm[index].f64[lane];
}
float asmtest_emu_x86_xmm_f32(const emu_result_t *r, int index, int lane) {
    if (index < 0 || index > 15 || lane < 0 || lane > 3)
        return 0.0f;
    return r->regs.xmm[index].f32[lane];
}

/* The execution-trace opaque handle + accessors (asmtest_emu_trace_new/free and
 * the readers) were extracted into the engine-neutral src/trace.c so the native
 * and hardware-trace tiers reuse them without the Unicorn object. They stay
 * exported under the same names for binding ABI stability. */

/* ---- mid-execution guard result handles + accessors (Track F) ----
 * A binding allocates a handle, arms the guard with emu_watch_writes /
 * emu_guard_reg (both take pointers + scalars, so a binding calls them
 * directly), runs an emu_call, then reads the recorded violation by field. The
 * arming/driving lives in emu.c; these are plain struct-field reads. */
emu_watch_t *asmtest_emu_watch_new(void) {
    return (emu_watch_t *)calloc(1, sizeof(emu_watch_t));
}
void asmtest_emu_watch_free(emu_watch_t *w) { free(w); }
int asmtest_emu_watch_violated(const emu_watch_t *w) {
    return (w && w->violated) ? 1 : 0;
}
unsigned long long asmtest_emu_watch_addr(const emu_watch_t *w) {
    return w ? (unsigned long long)w->addr : 0;
}
unsigned asmtest_emu_watch_size(const emu_watch_t *w) {
    return w ? (unsigned)w->size : 0;
}
unsigned long long asmtest_emu_watch_rip_off(const emu_watch_t *w) {
    return w ? (unsigned long long)w->rip_off : 0;
}

emu_reg_guard_t *asmtest_emu_reg_guard_new(void) {
    return (emu_reg_guard_t *)calloc(1, sizeof(emu_reg_guard_t));
}
void asmtest_emu_reg_guard_free(emu_reg_guard_t *g) { free(g); }
int asmtest_emu_reg_guard_violated(const emu_reg_guard_t *g) {
    return (g && g->violated) ? 1 : 0;
}
unsigned long long asmtest_emu_reg_guard_got(const emu_reg_guard_t *g) {
    return g ? (unsigned long long)g->got : 0;
}
unsigned long long asmtest_emu_reg_guard_rip_off(const emu_reg_guard_t *g) {
    return g ? (unsigned long long)g->rip_off : 0;
}

/* ---- coverage-guided fuzzing + mutation stat handles (Track E) ----
 * The drivers (emu_fuzz_cover1 / emu_mutation_test1, in fuzz.o) fill a caller
 * stat struct; a binding allocates the handle, passes it to the driver (with a
 * trace handle for the coverage union), then reads the totals. */
emu_fuzz_stat_t *asmtest_emu_fuzz_stat_new(void) {
    return (emu_fuzz_stat_t *)calloc(1, sizeof(emu_fuzz_stat_t));
}
void asmtest_emu_fuzz_stat_free(emu_fuzz_stat_t *s) { free(s); }
unsigned long long asmtest_emu_fuzz_blocks_reached(const emu_fuzz_stat_t *s) {
    return s ? (unsigned long long)s->blocks_reached : 0;
}
unsigned long long asmtest_emu_fuzz_corpus_len(const emu_fuzz_stat_t *s) {
    return s ? (unsigned long long)s->corpus_len : 0;
}
unsigned long long asmtest_emu_fuzz_iterations(const emu_fuzz_stat_t *s) {
    return s ? (unsigned long long)s->iterations : 0;
}

emu_mutation_stat_t *asmtest_emu_mutation_stat_new(void) {
    return (emu_mutation_stat_t *)calloc(1, sizeof(emu_mutation_stat_t));
}
void asmtest_emu_mutation_stat_free(emu_mutation_stat_t *s) { free(s); }
unsigned long long asmtest_emu_mutation_mutants(const emu_mutation_stat_t *s) {
    return s ? (unsigned long long)s->mutants : 0;
}
unsigned long long asmtest_emu_mutation_killed(const emu_mutation_stat_t *s) {
    return s ? (unsigned long long)s->killed : 0;
}
unsigned long long asmtest_emu_mutation_survived(const emu_mutation_stat_t *s) {
    return s ? (unsigned long long)s->survived : 0;
}

/* ---- cross-arch emu result handles + register accessors ----
 * The AArch64 / RISC-V / ARM32 guests run raw machine-code bytes (emu_arm64_call,
 * emu_riscv_call, emu_arm_call) and write a per-arch result struct. Their leading
 * fields (ok/uc_err/faulted/fault_addr/fault_kind) are laid out identically to
 * emu_result_t, so the asmtest_emu_result_{ok,faulted,fault_addr,fault_kind}
 * accessors above read any of these via the opaque pointer; only the register
 * files differ, and those reads live here. */

/* AArch64: x0..x30, plus sp / pc / nzcv; the NEON file is v0..v31. */
emu_arm64_result_t *asmtest_emu_arm64_result_new(void) {
    return (emu_arm64_result_t *)calloc(1, sizeof(emu_arm64_result_t));
}
void asmtest_emu_arm64_result_free(emu_arm64_result_t *r) {
    free(r);
}
unsigned long long asmtest_emu_arm64_reg(const emu_arm64_result_t *r,
                                         const char *name) {
    if (!name)
        return 0;
    if (!strcmp(name, "sp"))
        return (unsigned long long)r->regs.sp;
    if (!strcmp(name, "pc"))
        return (unsigned long long)r->regs.pc;
    if (!strcmp(name, "nzcv"))
        return (unsigned long long)r->regs.nzcv;
    if (name[0] == 'x') {
        int i = atoi(name + 1);
        if (i >= 0 && i <= 30)
            return (unsigned long long)r->regs.x[i];
    }
    return 0;
}
double asmtest_emu_arm64_vec_f64(const emu_arm64_result_t *r, int index,
                                 int lane) {
    if (index < 0 || index > 31 || lane < 0 || lane > 1)
        return 0.0;
    return r->regs.v[index].f64[lane];
}
float asmtest_emu_arm64_vec_f32(const emu_arm64_result_t *r, int index,
                                int lane) {
    if (index < 0 || index > 31 || lane < 0 || lane > 3)
        return 0.0f;
    return r->regs.v[index].f32[lane];
}

/* RISC-V (RV64): x0..x31 and pc, with the a0..a7 / ra / sp ABI aliases; the FP
 * file (D extension) is f0..f31, fa0 = f[10]. */
emu_riscv_result_t *asmtest_emu_riscv_result_new(void) {
    return (emu_riscv_result_t *)calloc(1, sizeof(emu_riscv_result_t));
}
void asmtest_emu_riscv_result_free(emu_riscv_result_t *r) {
    free(r);
}
unsigned long long asmtest_emu_riscv_reg(const emu_riscv_result_t *r,
                                         const char *name) {
    if (!name)
        return 0;
    if (!strcmp(name, "pc"))
        return (unsigned long long)r->regs.pc;
    if (!strcmp(name, "ra"))
        return (unsigned long long)r->regs.x[1];
    if (!strcmp(name, "sp"))
        return (unsigned long long)r->regs.x[2];
    if (name[0] == 'a') { /* a0..a7 -> x10..x17 */
        int i = atoi(name + 1);
        if (i >= 0 && i <= 7)
            return (unsigned long long)r->regs.x[10 + i];
    }
    if (name[0] == 'x') {
        int i = atoi(name + 1);
        if (i >= 0 && i <= 31)
            return (unsigned long long)r->regs.x[i];
    }
    return 0;
}
double asmtest_emu_riscv_f_f64(const emu_riscv_result_t *r, int index,
                               int lane) {
    if (index < 0 || index > 31 || lane < 0 || lane > 1)
        return 0.0;
    return r->regs.f[index].f64[lane];
}

/* ARM32 (AArch32): r0..r15 (sp = r13, lr = r14, pc = r15) and the 32-bit cpsr;
 * the VFP/NEON file is q0..q15. */
emu_arm_result_t *asmtest_emu_arm_result_new(void) {
    return (emu_arm_result_t *)calloc(1, sizeof(emu_arm_result_t));
}
void asmtest_emu_arm_result_free(emu_arm_result_t *r) {
    free(r);
}
unsigned long long asmtest_emu_arm_reg(const emu_arm_result_t *r,
                                       const char *name) {
    if (!name)
        return 0;
    if (!strcmp(name, "cpsr"))
        return (unsigned long long)r->regs.cpsr;
    if (!strcmp(name, "sp"))
        return (unsigned long long)r->regs.r[13];
    if (!strcmp(name, "lr"))
        return (unsigned long long)r->regs.r[14];
    if (!strcmp(name, "pc"))
        return (unsigned long long)r->regs.r[15];
    if (name[0] == 'r') {
        int i = atoi(name + 1);
        if (i >= 0 && i <= 15)
            return (unsigned long long)r->regs.r[i];
    }
    return 0;
}
double asmtest_emu_arm_q_f64(const emu_arm_result_t *r, int index, int lane) {
    if (index < 0 || index > 15 || lane < 0 || lane > 1)
        return 0.0;
    return r->regs.q[index].f64[lane];
}
float asmtest_emu_arm_q_f32(const emu_arm_result_t *r, int index, int lane) {
    if (index < 0 || index > 15 || lane < 0 || lane > 3)
        return 0.0f;
    return r->regs.q[index].f32[lane];
}
