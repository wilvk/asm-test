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

/* Record a failure into *out (bytes already NULL/zero) and return false. */
static bool fail(asm_result_t *out, const char *msg) {
    out->ok = false;
    snprintf(out->err, sizeof out->err, "%s", msg ? msg : "assemble failed");
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
