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

/* Map an asm_syntax_t to Keystone's KS_OPT_SYNTAX value (x86 only). Keystone
 * already defaults to Intel, so the caller only sets an option for the others:
 * NASM/MASM are Intel-family dialects, GAS the AT&T-family GNU-assembler one. */
static int ks_syntax_opt(asm_syntax_t syntax) {
    switch (syntax) {
    case ASM_SYNTAX_ATT:
        return KS_OPT_SYNTAX_ATT;
    case ASM_SYNTAX_NASM:
        return KS_OPT_SYNTAX_NASM;
    case ASM_SYNTAX_MASM:
        return KS_OPT_SYNTAX_MASM;
    case ASM_SYNTAX_GAS:
        return KS_OPT_SYNTAX_GAS;
    case ASM_SYNTAX_INTEL:
    default:
        return KS_OPT_SYNTAX_INTEL;
    }
}

/* Normalize a binding-supplied syntax int (passed across the FFI as a plain
 * int) to an asm_syntax_t, defaulting any out-of-range value to Intel. */
static asm_syntax_t syntax_from_int(int syntax) {
    if (syntax >= ASM_SYNTAX_INTEL && syntax <= ASM_SYNTAX_GAS)
        return (asm_syntax_t)syntax;
    return ASM_SYNTAX_INTEL;
}

/* Statement-counting rules for one (arch, syntax) pair. Keystone parses each
 * guest with the matching LLVM assembly dialect, and the three knobs below are
 * the only ones a statement counter needs. All three were measured against the
 * pinned Keystone (0.9.2) — see docs/internal/implementations/
 * assemble-silent-statement-drop.md; re-measure them across a Keystone bump.
 *
 *   ';'  separates statements everywhere EXCEPT x86/NASM, where it introduces
 *        an end-of-line comment (so a NASM source separates with newlines).
 *   '#'  introduces a comment on x86 (Intel/MASM/AT&T/GAS) but is the IMMEDIATE
 *        prefix on ARM64/ARM32 — truncating an ARM line there would swallow the
 *        rest of `mov x0, #1; mov x1, #2` and lose the count. On x86/NASM '#'
 *        is neither (it fails to parse), so the truncation is moot there.
 *   '//' and slash-star comments are LLVM-wide and apply to every dialect. */
typedef struct {
    bool semi_is_comment; /* ';' starts a comment rather than separating */
    bool hash_is_comment; /* '#' starts a comment rather than being data */
} stmt_rules_t;

static stmt_rules_t stmt_rules(asm_arch_t arch, asm_syntax_t syntax) {
    stmt_rules_t r = {false, true};
    if (arch == ASM_ARM64 || arch == ASM_ARM32) {
        r.hash_is_comment = false; /* '#' is an immediate: mov x0, #1 */
    } else if (arch == ASM_X86_64 && syntax == ASM_SYNTAX_NASM) {
        r.semi_is_comment = true; /* NASM: "mov rax, 1 ; note" */
        r.hash_is_comment = false;
    }
    return r;
}

/* A LOWER BOUND on the statements `source` contains, for comparison against the
 * stat_count Keystone reports. Deliberately conservative in one direction only:
 * it may under-count (a construct it walks past costs a detection), but it must
 * never over-count, because an over-count rejects valid code — the failure mode
 * that matters here (see the doc's "Constraints & gates").
 *
 * Comment-only lines, labels and directives are NOT special-cased: Keystone
 * counts each of them as a statement, so leaving them out of this count only
 * ever makes it smaller. String and character literals ARE tracked, because a
 * ';' inside one (`.ascii "a;b"`) is a single statement to Keystone and would
 * otherwise be counted as two — a measured false rejection. */
static size_t count_statements(const char *src, asm_arch_t arch,
                               asm_syntax_t syntax) {
    const stmt_rules_t rules = stmt_rules(arch, syntax);
    size_t n = 0;
    bool pending = false; /* chunk so far holds a non-blank code character */

    for (const char *p = src; *p != '\0'; p++) {
        char c = *p;

        /* Literals: separators and comment introducers inside one are ordinary
         * characters. An unterminated literal swallows the rest of the source,
         * which only lowers the count — the safe direction. */
        if (c == '"' || c == '\'') {
            pending = true;
            for (p++; *p != '\0' && *p != c; p++)
                if (*p == '\\' && p[1] != '\0')
                    p++; /* \" does not close the literal */
            if (*p == '\0')
                break;
            continue;
        }

        /* Block comment; may span newlines, which then do not separate. */
        if (c == '/' && p[1] == '*') {
            for (p += 2; *p != '\0' && !(*p == '*' && p[1] == '/'); p++)
                ;
            if (*p == '\0')
                break;
            p++; /* land on '/', the loop's p++ steps past it */
            continue;
        }

        if ((c == '/' && p[1] == '/') || (c == '#' && rules.hash_is_comment) ||
            (c == ';' && rules.semi_is_comment)) {
            while (*p != '\0' && *p != '\n')
                p++;
            if (*p == '\0')
                break;
            c = '\n'; /* the newline that ended the comment still separates */
        }

        if (c == '\n' || (c == ';' && !rules.semi_is_comment)) {
            if (pending)
                n++;
            pending = false;
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\f' && c != '\v')
            pending = true;
    }
    return pending ? n + 1 : n;
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

const char *asmtest_asm_last_error(void) { return g_asm_err; }

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

    /* Syntax is an x86-only knob; Keystone defaults x86 to Intel, so only the
     * non-Intel dialects need an explicit option. */
    if (arch == ASM_X86_64 && syntax != ASM_SYNTAX_INTEL)
        ks_option(ks, KS_OPT_SYNTAX, ks_syntax_opt(syntax));

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

    /* ks_asm returning 0 is NOT enough: on a recoverable per-statement error
     * (bad operand, truncated operand, wrong-dialect operand) Keystone drops
     * that statement, leaves ks_errno at KS_ERR_OK, and still returns success —
     * so the caller would get machine code missing an instruction they wrote,
     * with ok == true. stat_count is the only witness. Fail the whole assemble
     * rather than hand back a quiet wrong answer. */
    size_t expected = count_statements(source, arch, syntax);
    if (stat_count < expected) {
        snprintf(out->err, sizeof out->err,
                 "assembler skipped %zu of %zu statements (check the syntax "
                 "argument)",
                 expected - stat_count, expected);
        out->ok = false;
        set_last_err(out->err);
        ks_free(enc);
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

bool emu_arm_call_asm(emu_arm_t *e, const char *src, const long *args,
                      int nargs, uint64_t max_insns, emu_arm_result_t *out) {
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
                          long a1, long a2, long a3, long a4, long a5,
                          int nargs, uint64_t max_insns, emu_result_t *out) {
    long args[6] = {a0, a1, a2, a3, a4, a5};
    asm_syntax_t syn = syntax_from_int(syntax);
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
    return asmtest_emu_call_asm6(e, src, ASM_SYNTAX_INTEL, a0, a1, 0, 0, 0, 0,
                                 2, 0, out);
}

/* Assemble-only FFI shim (see asmtest_assemble.h): multi-arch text -> bytes for
 * bindings, returning the machine-code length and copying up to `cap` bytes. */
int asmtest_asm_bytes(int arch, int syntax, const char *src, uint64_t addr,
                      uint8_t *buf, int cap) {
    if (arch < ASM_X86_64 || arch > ASM_ARM32) {
        set_last_err("unknown architecture");
        return 0;
    }
    asm_syntax_t syn = syntax_from_int(syntax);
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
