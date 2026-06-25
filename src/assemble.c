/*
 * assemble.c — in-line assembler (text -> machine code), backed by Keystone.
 *
 * Phase 1 (this file, for now): the assembler core — asmtest_assemble /
 * asmtest_asm_free. The Phase 2 emu bridge wrappers (emu_call_asm, ...)
 * declared in asmtest_assemble.h are added here later; they call the Unicorn-
 * side emu_*_call but this object only needs the Keystone headers, so the emu
 * tier stays Keystone-free and only the final binary links both libs.
 */
#include "asmtest_assemble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <keystone/keystone.h>

/* Map an asm_arch_t to Keystone's (arch, mode). Returns false for a guest this
 * Keystone build can't assemble — notably RISC-V, which released Keystone does
 * not support (the #ifdef lights it up automatically if a future build adds
 * KS_ARCH_RISCV). On failure, *why points at a short reason for the caller. */
static bool ks_target(asm_arch_t arch, ks_arch *ks_a, int *ks_mode,
                      const char **why) {
    switch (arch) {
    case ASM_X86_64:
        *ks_a = KS_ARCH_X86;
        *ks_mode = KS_MODE_64;
        return true;
    case ASM_ARM64:
        *ks_a = KS_ARCH_ARM64;
        *ks_mode = KS_MODE_LITTLE_ENDIAN;
        return true;
    case ASM_ARM32:
        *ks_a = KS_ARCH_ARM;
        *ks_mode = KS_MODE_ARM | KS_MODE_LITTLE_ENDIAN;
        return true;
    case ASM_RISCV64:
#ifdef KS_ARCH_RISCV
        *ks_a = KS_ARCH_RISCV;
        *ks_mode = KS_MODE_RISCV64 | KS_MODE_LITTLE_ENDIAN;
        return true;
#else
        *why = "RISC-V is not supported by this Keystone build";
        return false;
#endif
    }
    *why = "unknown architecture";
    return false;
}

/* Last assemble diagnostic for the calling thread, surfaced to bindings through
 * asmtest_asm_last_error(). Thread-local so a multithreaded host's concurrent
 * assembles don't clobber each other's error. */
#if defined(_MSC_VER)
#define ASM_TLS __declspec(thread)
#else
#define ASM_TLS __thread
#endif
static ASM_TLS char g_asm_err[sizeof(((asm_result_t *)0)->err)];

static void set_last_err(const char *msg) {
    snprintf(g_asm_err, sizeof g_asm_err, "%s", msg ? msg : "");
}

const char *asmtest_asm_last_error(void) {
    return g_asm_err;
}

/* Record a failure into *out (bytes already NULL/zero) and return false. */
static bool fail(asm_result_t *out, const char *msg) {
    out->ok = false;
    snprintf(out->err, sizeof out->err, "%s", msg ? msg : "assemble failed");
    set_last_err(out->err);
    return false;
}

bool asmtest_assemble(asm_arch_t arch, asm_syntax_t syntax, const char *source,
                      uint64_t addr, asm_result_t *out) {
    memset(out, 0, sizeof *out);
    if (source == NULL)
        return fail(out, "null assembly source");

    ks_arch ks_a;
    int ks_mode;
    const char *why = NULL;
    if (!ks_target(arch, &ks_a, &ks_mode, &why))
        return fail(out, why);

    ks_engine *ks = NULL;
    if (ks_open(ks_a, ks_mode, &ks) != KS_ERR_OK)
        return fail(out, "ks_open failed (unsupported arch/mode)");

    /* Syntax is an x86-only knob; Keystone defaults x86 to Intel. */
    if (arch == ASM_X86_64 && syntax == ASM_SYNTAX_ATT)
        ks_option(ks, KS_OPT_SYNTAX, KS_OPT_SYNTAX_ATT);

    unsigned char *enc = NULL;
    size_t enc_size = 0, stat_count = 0;
    if (ks_asm(ks, source, addr, &enc, &enc_size, &stat_count) != 0) {
        /* ks_errno carries why; report it with how far we got. */
        snprintf(out->err, sizeof out->err, "%s (after %zu statement%s)",
                 ks_strerror(ks_errno(ks)), stat_count,
                 stat_count == 1 ? "" : "s");
        out->ok = false;
        set_last_err(out->err);
        ks_free(enc); /* NULL-safe; nothing allocated on the error path */
        ks_close(ks);
        return false;
    }

    /* Copy out of Keystone's allocator into a plain malloc buffer so the caller
     * frees with asmtest_asm_free() and never needs to know about ks_free(). */
    out->bytes = (uint8_t *)malloc(enc_size ? enc_size : 1);
    if (out->bytes == NULL) {
        ks_free(enc);
        ks_close(ks);
        return fail(out, "out of memory");
    }
    memcpy(out->bytes, enc, enc_size);
    out->len = enc_size;
    out->stat_count = stat_count;
    out->ok = true;
    set_last_err(""); /* clear any stale diagnostic on success */

    ks_free(enc);
    ks_close(ks);
    return true;
}

void asmtest_asm_free(asm_result_t *r) {
    if (r == NULL)
        return;
    free(r->bytes);
    memset(r, 0, sizeof *r);
}

/* ---- Emu bridge: assemble + run (Phase 2) -------------------------------- */
/* These call the Unicorn-side emu_*_call (declared in asmtest_emu.h) but this
 * translation unit links only Keystone; the final binary links both. Each
 * assembles at EMU_CODE_BASE — the address the emulator loads and runs the
 * bytes at — so PC-relative and branch targets resolve correctly. */

/* Assemble `src` for `arch`/`syntax` at the guest load base. On failure prints
 * the Keystone diagnostic to stderr (the array-API bridges below echo failures
 * there) and returns false; the result, already cleared by asmtest_assemble, is
 * still safe to asmtest_asm_free. The thread-local last-error is set either way,
 * so a quiet caller can read it via asmtest_asm_last_error() instead. */
static bool assemble_at_base(asm_arch_t arch, asm_syntax_t syntax,
                             const char *src, asm_result_t *r) {
    if (asmtest_assemble(arch, syntax, src, EMU_CODE_BASE, r))
        return true;
    fprintf(stderr, "asm-test: in-line assembly failed: %s\n", r->err);
    return false;
}

bool emu_call_asm(emu_t *e, const char *src, const long *args, int nargs,
                  uint64_t max_insns, emu_result_t *out) {
    memset(out, 0, sizeof *out);
    asm_result_t r;
    if (!assemble_at_base(ASM_X86_64, ASM_SYNTAX_INTEL, src, &r)) {
        out->uc_err = -1;
        asmtest_asm_free(&r);
        return false;
    }
    bool ok = emu_call(e, r.bytes, r.len, args, nargs, max_insns, out);
    asmtest_asm_free(&r);
    return ok;
}

bool emu_arm64_call_asm(emu_arm64_t *e, const char *src, const long *args,
                        int nargs, uint64_t max_insns,
                        emu_arm64_result_t *out) {
    memset(out, 0, sizeof *out);
    asm_result_t r;
    if (!assemble_at_base(ASM_ARM64, ASM_SYNTAX_INTEL, src, &r)) {
        out->uc_err = -1;
        asmtest_asm_free(&r);
        return false;
    }
    bool ok = emu_arm64_call(e, r.bytes, r.len, args, nargs, max_insns, out);
    asmtest_asm_free(&r);
    return ok;
}

bool emu_riscv_call_asm(emu_riscv_t *e, const char *src, const long *args,
                        int nargs, uint64_t max_insns,
                        emu_riscv_result_t *out) {
    memset(out, 0, sizeof *out);
    asm_result_t r;
    if (!assemble_at_base(ASM_RISCV64, ASM_SYNTAX_INTEL, src, &r)) {
        out->uc_err = -1;
        asmtest_asm_free(&r);
        return false;
    }
    bool ok = emu_riscv_call(e, r.bytes, r.len, args, nargs, max_insns, out);
    asmtest_asm_free(&r);
    return ok;
}

bool emu_arm_call_asm(emu_arm_t *e, const char *src, const long *args, int nargs,
                      uint64_t max_insns, emu_arm_result_t *out) {
    memset(out, 0, sizeof *out);
    asm_result_t r;
    if (!assemble_at_base(ASM_ARM32, ASM_SYNTAX_INTEL, src, &r)) {
        out->uc_err = -1;
        asmtest_asm_free(&r);
        return false;
    }
    bool ok = emu_arm_call(e, r.bytes, r.len, args, nargs, max_insns, out);
    asmtest_asm_free(&r);
    return ok;
}

/* Widened opaque-handle FFI shim (see asmtest_assemble.h): assemble x86-64 `src`
 * in `syntax` and run it with the first `nargs` scalar args, capped at
 * `max_insns`. Six scalars (like asmtest_capture6) so dynamic FFIs marshal no
 * array. Quiet on failure — the binding reports asmtest_asm_last_error(). */
int asmtest_emu_call_asm6(emu_t *e, const char *src, int syntax, long a0,
                          long a1, long a2, long a3, long a4, long a5, int nargs,
                          uint64_t max_insns, emu_result_t *out) {
    long args[6] = {a0, a1, a2, a3, a4, a5};
    asm_syntax_t syn = syntax == ASM_SYNTAX_ATT ? ASM_SYNTAX_ATT
                                                : ASM_SYNTAX_INTEL;
    if (nargs < 0)
        nargs = 0;
    if (nargs > 6)
        nargs = 6;
    memset(out, 0, sizeof *out);
    asm_result_t r;
    if (!asmtest_assemble(ASM_X86_64, syn, src, EMU_CODE_BASE, &r)) {
        out->uc_err = -1;
        asmtest_asm_free(&r);
        return 0;
    }
    bool ok = emu_call(e, r.bytes, r.len, args, nargs, max_insns, out);
    asmtest_asm_free(&r);
    return ok ? 1 : 0;
}

/* Intel-only, two-arg compatibility wrapper over the widened shim. */
int asmtest_emu_call_asm(emu_t *e, const char *src, long a0, long a1,
                         emu_result_t *out) {
    return asmtest_emu_call_asm6(e, src, ASM_SYNTAX_INTEL, a0, a1, 0, 0, 0, 0, 2,
                                 0, out);
}

/* Assemble-only FFI shim (see asmtest_assemble.h): multi-arch text -> bytes for
 * bindings, returning the machine-code length and copying up to `cap` bytes. */
int asmtest_asm_bytes(int arch, int syntax, const char *src, uint64_t addr,
                      uint8_t *buf, int cap) {
    if (arch < ASM_X86_64 || arch > ASM_ARM32) {
        set_last_err("unknown architecture");
        return 0;
    }
    asm_syntax_t syn = syntax == ASM_SYNTAX_ATT ? ASM_SYNTAX_ATT
                                                : ASM_SYNTAX_INTEL;
    asm_result_t r;
    if (!asmtest_assemble((asm_arch_t)arch, syn, src, addr, &r)) {
        asmtest_asm_free(&r);
        return 0;
    }
    int len = (int)r.len;
    if (buf != NULL && cap > 0) {
        int n = len < cap ? len : cap;
        memcpy(buf, r.bytes, (size_t)n);
    }
    asmtest_asm_free(&r);
    return len;
}
