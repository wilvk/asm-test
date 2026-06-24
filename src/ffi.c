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

/* x86-64 guest register by name (rax..r15). The 16 GP fields are contiguous. */
unsigned long long asmtest_emu_x86_reg(const emu_result_t *r, const char *name) {
    static const char *const n[16] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi",
                                      "rbp", "rsp", "r8",  "r9",  "r10", "r11",
                                      "r12", "r13", "r14", "r15"};
    const uint64_t *p = &r->regs.rax;
    if (!name)
        return 0;
    for (int i = 0; i < 16; i++)
        if (!strcmp(name, n[i]))
            return (unsigned long long)p[i];
    return 0;
}
