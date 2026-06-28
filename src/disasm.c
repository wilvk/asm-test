/*
 * disasm.c — instruction-level disassembly for trace diagnostics, backed by
 * Capstone (the disassembler counterpart to the Keystone assembler in
 * assemble.c). Turns the byte offsets a trace backend records for faults,
 * traces, and coverage into readable instructions: `uncovered: 0x2f` becomes
 * `uncovered: 0x2f  cmp rax, 0`.
 *
 * Offset-based and backend-neutral: the same annotation layer renders offsets
 * recorded by the Unicorn emulator, the DynamoRIO native tier, and the Intel PT /
 * ARM CoreSight decoders. The canonical entry points (asmtest_*) take an explicit
 * base_addr so native/hardware regions at a runtime base resolve PC-relative
 * operands correctly; the historical emu_* spellings default base_addr to
 * EMU_CODE_BASE (the emulator load address).
 *
 * Optional and self-contained, exactly like assemble.o: built with
 * -DASMTEST_HAVE_CAPSTONE (set by the Makefile when pkg-config finds capstone)
 * it links Capstone; without it every entry point compiles to a graceful
 * fallback that degrades to bare offsets. Kept in its own translation unit so
 * the core emulator build (emu.o, libasmtest_emu) never depends on Capstone —
 * only a binary that wants disassembly links it.
 *
 * The report variants reuse emu_trace_report / emu_coverage_uncovered (in
 * trace.o) verbatim for the no-Capstone / no-code path, so the offset-only output
 * stays identical whether or not this file does anything.
 */
#include "asmtest_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ASMTEST_HAVE_CAPSTONE
#include <capstone/capstone.h>

/* Map an asmtest_arch_t to Capstone's (arch, mode). Returns false for a guest
 * this Capstone build can't disassemble — notably RISC-V, which needs Capstone
 * >= 5 (the CS_ARCH_RISCV enumerator does not exist before then, so it is guarded
 * by the API-version macro rather than referenced unconditionally). */
static bool cs_target(asmtest_arch_t arch, cs_arch *a, cs_mode *m) {
    switch (arch) {
    case ASMTEST_ARCH_X86_64:
        *a = CS_ARCH_X86;
        *m = CS_MODE_64;
        return true;
    case ASMTEST_ARCH_ARM64:
        *a = CS_ARCH_ARM64;
        *m = CS_MODE_LITTLE_ENDIAN;
        return true;
    case ASMTEST_ARCH_ARM32:
        *a = CS_ARCH_ARM;
        *m = (cs_mode)(CS_MODE_ARM | CS_MODE_LITTLE_ENDIAN);
        return true;
    case ASMTEST_ARCH_RISCV64:
#if defined(CS_API_MAJOR) && (CS_API_MAJOR >= 5)
        *a = CS_ARCH_RISCV;
        *m = (cs_mode)(CS_MODE_RISCV64 | CS_MODE_RISCVC);
        return true;
#else
        return false; /* RISC-V disassembly needs Capstone >= 5 */
#endif
    }
    return false;
}
#endif /* ASMTEST_HAVE_CAPSTONE */

bool asmtest_disas_available(void) {
#ifdef ASMTEST_HAVE_CAPSTONE
    return true;
#else
    return false;
#endif
}
bool emu_disas_available(void) { return asmtest_disas_available(); }

size_t asmtest_disas(asmtest_arch_t arch, const uint8_t *code, size_t code_len,
                     uint64_t base_addr, uint64_t off, char *buf,
                     size_t buflen) {
    if (buf != NULL && buflen > 0)
        buf[0] = '\0';
#ifdef ASMTEST_HAVE_CAPSTONE
    if (code == NULL || off >= code_len)
        return 0;
    cs_arch a;
    cs_mode m;
    if (!cs_target(arch, &a, &m))
        return 0;
    csh h;
    if (cs_open(a, m, &h) != CS_ERR_OK)
        return 0;
    cs_insn *insn = NULL;
    size_t count = cs_disasm(h, code + off, code_len - (size_t)off,
                             base_addr + off, 1, &insn);
    size_t len = 0;
    if (count > 0) {
        if (buf != NULL && buflen > 0) {
            if (insn[0].op_str[0] != '\0')
                snprintf(buf, buflen, "%s %s", insn[0].mnemonic,
                         insn[0].op_str);
            else
                snprintf(buf, buflen, "%s", insn[0].mnemonic);
        }
        len = insn[0].size;
        cs_free(insn, count);
    }
    cs_close(&h);
    return len;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)base_addr;
    (void)off;
    return 0;
#endif
}

/* Compatibility spelling: disassemble at the emulator load base by default. */
size_t emu_disas(emu_arch_t arch, const uint8_t *code, size_t code_len,
                 uint64_t base_addr, uint64_t off, char *buf, size_t buflen) {
    return asmtest_disas(arch, code, code_len, base_addr, off, buf, buflen);
}

/* Copy `n` block offsets into a freshly malloc'd ascending array (caller frees);
 * *out_n receives the count. NULL on empty/oom. Mirrors trace.c's sorted_blocks
 * so the disassembling reports list blocks in the same order as the plain ones. */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t *dup_sorted(const uint64_t *src, size_t n, size_t *out_n) {
    *out_n = src != NULL ? n : 0;
    if (*out_n == 0)
        return NULL;
    uint64_t *s = (uint64_t *)malloc(*out_n * sizeof *s);
    if (s == NULL) {
        *out_n = 0;
        return NULL;
    }
    memcpy(s, src, *out_n * sizeof *s);
    qsort(s, *out_n, sizeof *s, cmp_u64);
    return s;
}

/* "    0x2f  cmp rax, 0\n" — one annotated block line; the instruction text is
 * elided when the bytes do not decode (e.g. the offset is past the routine). */
static void print_block_line(FILE *out, asmtest_arch_t arch, const uint8_t *code,
                             size_t code_len, uint64_t base_addr, uint64_t off) {
    char text[128];
    asmtest_disas(arch, code, code_len, base_addr, off, text, sizeof text);
    if (text[0] != '\0')
        fprintf(out, "    0x%llx  %s\n", (unsigned long long)off, text);
    else
        fprintf(out, "    0x%llx\n", (unsigned long long)off);
}

void asmtest_trace_disasm(const asmtest_trace_t *t, asmtest_arch_t arch,
                          const uint8_t *code, size_t code_len,
                          uint64_t base_addr, FILE *out) {
    if (t == NULL || out == NULL)
        return;
    fprintf(out, "trace: %llu instructions%s\n",
            (unsigned long long)t->insns_total,
            t->truncated ? " (truncated)" : "");
    if (t->insns == NULL)
        return;
    bool disasm = asmtest_disas_available() && code != NULL;
    for (size_t i = 0; i < t->insns_len; i++)
        if (disasm)
            print_block_line(out, arch, code, code_len, base_addr, t->insns[i]);
        else
            fprintf(out, "    0x%llx\n", (unsigned long long)t->insns[i]);
}

void emu_trace_disasm(const emu_trace_t *t, emu_arch_t arch, const uint8_t *code,
                      size_t code_len, FILE *out) {
    asmtest_trace_disasm(t, arch, code, code_len, EMU_CODE_BASE, out);
}

void asmtest_trace_report_disasm(const asmtest_trace_t *t, asmtest_arch_t arch,
                                 const uint8_t *code, size_t code_len,
                                 uint64_t base_addr, FILE *out) {
    if (!asmtest_disas_available() || code == NULL) {
        emu_trace_report(t, out);
        return;
    }
    if (t == NULL || out == NULL)
        return;
    fprintf(out,
            "coverage: %zu distinct blocks, %llu block entries, "
            "%llu instructions%s\n",
            t->blocks_len, (unsigned long long)t->blocks_total,
            (unsigned long long)t->insns_total,
            t->truncated ? " (truncated)" : "");
    size_t n;
    uint64_t *s = dup_sorted(t->blocks, t->blocks_len, &n);
    if (s == NULL)
        return;
    fprintf(out, "  blocks:\n");
    for (size_t i = 0; i < n; i++)
        print_block_line(out, arch, code, code_len, base_addr, s[i]);
    free(s);
}

void emu_trace_report_disasm(const emu_trace_t *t, emu_arch_t arch,
                             const uint8_t *code, size_t code_len, FILE *out) {
    asmtest_trace_report_disasm(t, arch, code, code_len, EMU_CODE_BASE, out);
}

size_t asmtest_trace_coverage_uncovered_disasm(const asmtest_trace_t *covered,
                                               const asmtest_trace_t *universe,
                                               asmtest_arch_t arch,
                                               const uint8_t *code,
                                               size_t code_len,
                                               uint64_t base_addr, FILE *out) {
    if (!asmtest_disas_available() || code == NULL)
        return emu_coverage_uncovered(covered, universe, out);
    if (universe == NULL || universe->blocks == NULL)
        return 0;
    size_t total = universe->blocks_len;
    uint64_t *miss = (uint64_t *)malloc((total ? total : 1) * sizeof *miss);
    if (miss == NULL)
        return 0;
    size_t nmiss = 0;
    for (size_t i = 0; i < total; i++)
        if (!emu_trace_covered(covered, universe->blocks[i]))
            miss[nmiss++] = universe->blocks[i];
    qsort(miss, nmiss, sizeof *miss, cmp_u64);
    if (out != NULL) {
        fprintf(out, "coverage: %zu/%zu blocks covered\n", total - nmiss,
                total);
        if (nmiss > 0) {
            fprintf(out, "  uncovered:\n");
            for (size_t i = 0; i < nmiss; i++)
                print_block_line(out, arch, code, code_len, base_addr, miss[i]);
        }
    }
    free(miss);
    return nmiss;
}

size_t emu_coverage_uncovered_disasm(const emu_trace_t *covered,
                                     const emu_trace_t *universe,
                                     emu_arch_t arch, const uint8_t *code,
                                     size_t code_len, FILE *out) {
    return asmtest_trace_coverage_uncovered_disasm(
        covered, universe, arch, code, code_len, EMU_CODE_BASE, out);
}

void emu_fault_describe(const emu_result_t *r, emu_arch_t arch,
                        const uint8_t *code, size_t code_len,
                        uint64_t base_addr, char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    buf[0] = '\0';
    if (r == NULL) {
        snprintf(buf, buflen, "no result");
        return;
    }
    if (!r->faulted) {
        snprintf(buf, buflen, "no fault");
        return;
    }
    static const char *const kinds[] = {"no", "read", "write", "fetch"};
    const char *kind = (r->fault_kind >= EMU_FAULT_NONE &&
                        r->fault_kind <= EMU_FAULT_FETCH)
                           ? kinds[r->fault_kind]
                           : "?";
    /* On a fault Unicorn leaves rip at the offending instruction; its offset
     * from the load base is what we disassemble (the routine's own bytes). */
    uint64_t rip = r->regs.rip;
    uint64_t off = rip >= base_addr ? rip - base_addr : rip;
    char text[128];
    asmtest_disas(arch, code, code_len, base_addr, off, text, sizeof text);
    if (text[0] != '\0')
        snprintf(buf, buflen, "%s fault accessing 0x%llx: %s  (@0x%llx)", kind,
                 (unsigned long long)r->fault_addr, text,
                 (unsigned long long)off);
    else
        snprintf(buf, buflen, "%s fault accessing 0x%llx  (@0x%llx)", kind,
                 (unsigned long long)r->fault_addr, (unsigned long long)off);
}

void emu_watch_describe(const emu_watch_t *w, emu_arch_t arch,
                        const uint8_t *code, size_t code_len,
                        uint64_t base_addr, char *buf, size_t buflen) {
    if (buf == NULL || buflen == 0)
        return;
    buf[0] = '\0';
    if (w == NULL) {
        snprintf(buf, buflen, "no watch");
        return;
    }
    if (!w->violated) {
        snprintf(buf, buflen, "no violation");
        return;
    }
    char text[128];
    asmtest_disas(arch, code, code_len, base_addr, w->rip_off, text,
                  sizeof text);
    if (text[0] != '\0')
        snprintf(buf, buflen, "write to 0x%llx (%u bytes): %s  (@0x%llx)",
                 (unsigned long long)w->addr, (unsigned)w->size, text,
                 (unsigned long long)w->rip_off);
    else
        snprintf(buf, buflen, "write to 0x%llx (%u bytes)  (@0x%llx)",
                 (unsigned long long)w->addr, (unsigned)w->size,
                 (unsigned long long)w->rip_off);
}
