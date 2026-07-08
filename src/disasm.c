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

/* True if `insn` (decoded for Capstone arch `a`) is a call. Capstone 5 tags AArch64
 * `bl`/`blr` with CS_GRP_CALL, but Capstone 4 — the distro libcapstone-dev the decode
 * lane installs — files them under CS_GRP_JUMP instead, so match those two opcodes by id
 * as well. This keeps call detection identical on the apt (4.x) and from-source (5.x)
 * builds. Needs CS_OPT_DETAIL on for the group query. */
static int insn_is_call(csh h, cs_insn *insn, cs_arch a) {
    if (cs_insn_group(h, insn, CS_GRP_CALL))
        return 1;
    if (a == CS_ARCH_ARM64 &&
        (insn->id == ARM64_INS_BL || insn->id == ARM64_INS_BLR))
        return 1;
    return 0;
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

/* 1 if the instruction at `code+off` is a CALL (x86 `call`; AArch64 `bl`/`blr`) — the
 * Capstone CS_GRP_CALL group, which the out-of-process ptrace stepper uses to tell a
 * call-out to a runtime helper (step over it, resume after) from the routine's own
 * return (stop). 0 otherwise, or always when built without Capstone (callers then fall
 * back to leaf-only tracing — the first region exit is treated as the return). */
int asmtest_disas_is_call(asmtest_arch_t arch, const uint8_t *code,
                          size_t code_len, uint64_t off) {
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
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON); /* groups[] needs detail mode */
    cs_insn *insn = NULL;
    size_t count =
        cs_disasm(h, code + off, code_len - (size_t)off, off, 1, &insn);
    int is_call = 0;
    if (count > 0) {
        is_call = insn_is_call(h, &insn[0], a);
        cs_free(insn, count);
    }
    cs_close(&h);
    return is_call;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)off;
    return 0;
#endif
}

/* Decode the instruction at `code+off` ONCE and report its byte length (return value; 0 if
 * undecodable / no Capstone) plus, via the out-params, whether it is a call and/or a return.
 * The call-descent step loop needs all three per single-stepped instruction; doing them in
 * one cs_open + cs_disasm avoids the 3x engine open/decode of calling is_call + is_ret +
 * asmtest_disas separately on a hot path. `is_call`/`is_ret` may be NULL. Not part of the
 * bindings-parity tier surface. */
size_t asmtest_disas_probe(asmtest_arch_t arch, const uint8_t *code,
                           size_t code_len, uint64_t off, int *is_call,
                           int *is_ret) {
    if (is_call != NULL)
        *is_call = 0;
    if (is_ret != NULL)
        *is_ret = 0;
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
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON); /* groups[] needs detail mode */
    cs_insn *insn = NULL;
    size_t count =
        cs_disasm(h, code + off, code_len - (size_t)off, off, 1, &insn);
    size_t len = 0;
    if (count > 0) {
        if (is_call != NULL)
            *is_call = insn_is_call(h, &insn[0], a);
        if (is_ret != NULL)
            *is_ret = cs_insn_group(h, &insn[0], CS_GRP_RET) ? 1 : 0;
        len = insn[0].size;
        cs_free(insn, count);
    }
    cs_close(&h);
    return len;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)off;
    return 0;
#endif
}

/* 1 if the instruction at `code+off` is any control-transfer instruction — a
 * (conditional or unconditional) jump, a call, or a return — matching the block
 * partition the PT/DR/Unicorn backends produce (a block ends after every CTI).
 * 0 otherwise, or always when built without Capstone. */
int asmtest_disas_is_branch(asmtest_arch_t arch, const uint8_t *code,
                            size_t code_len, uint64_t off) {
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
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON); /* groups[] needs detail mode */
    cs_insn *insn = NULL;
    size_t count =
        cs_disasm(h, code + off, code_len - (size_t)off, off, 1, &insn);
    int is_branch = 0;
    if (count > 0) {
        is_branch = (cs_insn_group(h, &insn[0], CS_GRP_JUMP) ||
                     cs_insn_group(h, &insn[0], CS_GRP_CALL) ||
                     cs_insn_group(h, &insn[0], CS_GRP_RET) ||
                     cs_insn_group(h, &insn[0], CS_GRP_IRET))
                        ? 1
                        : 0;
        cs_free(insn, count);
    }
    cs_close(&h);
    return is_branch;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)off;
    return 0;
#endif
}

/* 1 if the instruction at `code+off` is a RETURN (x86 `ret`/`retf`; AArch64 `ret`) — the
 * Capstone CS_GRP_RET group, the third term of the call-descent pop predicate. 0
 * otherwise, or always when built without Capstone. Mirrors asmtest_disas_is_call. */
int asmtest_disas_is_ret(asmtest_arch_t arch, const uint8_t *code,
                         size_t code_len, uint64_t off) {
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
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON); /* groups[] needs detail mode */
    cs_insn *insn = NULL;
    size_t count =
        cs_disasm(h, code + off, code_len - (size_t)off, off, 1, &insn);
    int is_ret = 0;
    if (count > 0) {
        is_ret = cs_insn_group(h, &insn[0], CS_GRP_RET) ? 1 : 0;
        cs_free(insn, count);
    }
    cs_close(&h);
    return is_ret;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)off;
    return 0;
#endif
}

/* 1 if the instruction at `code+off` is an UNCONDITIONAL jump (x86 `jmp`, id
 * X86_INS_JMP within CS_GRP_JUMP — direct rel or indirect r/m; NOT a conditional jcc,
 * which shares the JUMP group under a different id, and NOT ret/iret). 0 otherwise,
 * on a non-x86 arch (no in-tree caller needs unconditional-jump detection there — the
 * AMD reduced-filter replay is x86-only), or without Capstone. Mirrors
 * asmtest_disas_is_ret; the AMD replay pairs it with asmtest_disas_branch_target() to
 * isolate a DIRECT unconditional jmp (target immediate present) it must follow. */
int asmtest_disas_is_uncond_jump(asmtest_arch_t arch, const uint8_t *code,
                                 size_t code_len, uint64_t off) {
#ifdef ASMTEST_HAVE_CAPSTONE
    if (code == NULL || off >= code_len)
        return 0;
    cs_arch a;
    cs_mode m;
    if (!cs_target(arch, &a, &m))
        return 0;
    if (a != CS_ARCH_X86)
        return 0; /* only the x86 unconditional-jmp id is consulted below */
    csh h;
    if (cs_open(a, m, &h) != CS_ERR_OK)
        return 0;
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON); /* groups[] needs detail mode */
    cs_insn *insn = NULL;
    size_t count =
        cs_disasm(h, code + off, code_len - (size_t)off, off, 1, &insn);
    int is_uncond = 0;
    if (count > 0) {
        is_uncond = (cs_insn_group(h, &insn[0], CS_GRP_JUMP) &&
                     insn[0].id == X86_INS_JMP)
                        ? 1
                        : 0;
        cs_free(insn, count);
    }
    cs_close(&h);
    return is_uncond;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)off;
    return 0;
#endif
}

/* Resolve the DIRECT-call target at `code+off` into *target (absolute = base_addr +
 * displacement). x86 `E8` rel32 and AArch64 `bl` imm26 are direct; Capstone exposes the
 * resolved absolute target as an immediate operand (CS_OP_IMM). Returns 1 (direct call,
 * *target set) or 0 (indirect call / non-call / undecodable / no Capstone), in which case
 * the descent loop reads the live post-step PC as the target instead. */
int asmtest_disas_call_target(asmtest_arch_t arch, const uint8_t *code,
                              size_t code_len, uint64_t base_addr, uint64_t off,
                              uint64_t *target) {
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
    cs_option(h, CS_OPT_DETAIL,
              CS_OPT_ON); /* operands + groups need detail mode */
    cs_insn *insn = NULL;
    /* Decode at base_addr+off so Capstone resolves the rel32/imm26 to its ABSOLUTE
     * target immediate directly (no manual displacement arithmetic). */
    size_t count = cs_disasm(h, code + off, code_len - (size_t)off,
                             base_addr + off, 1, &insn);
    int ok = 0;
    if (count > 0) {
        if (insn_is_call(h, &insn[0], a)) {
            const cs_detail *d = insn[0].detail;
            if (d != NULL) {
                /* Both operand-enum sets are always available (capstone.h pulls in
                 * x86.h and arm64.h), and the disasm layer is cross-arch — x86 bytes
                 * may be decoded on an AArch64 host and vice versa — so neither branch
                 * is host-arch gated; `a` (the decode target) selects the union arm. */
                if (a == CS_ARCH_X86) {
                    for (int i = 0; i < d->x86.op_count; i++)
                        if (d->x86.operands[i].type == X86_OP_IMM) {
                            if (target != NULL)
                                *target = (uint64_t)d->x86.operands[i].imm;
                            ok = 1;
                            break;
                        }
                } else if (a == CS_ARCH_ARM64) {
                    for (int i = 0; i < d->arm64.op_count; i++)
                        if (d->arm64.operands[i].type == ARM64_OP_IMM) {
                            if (target != NULL)
                                *target = (uint64_t)d->arm64.operands[i].imm;
                            ok = 1;
                            break;
                        }
                }
            }
        }
        cs_free(insn, count);
    }
    cs_close(&h);
    return ok;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)base_addr;
    (void)off;
    (void)target;
    return 0;
#endif
}

/* Resolve the DIRECT-branch target at `code+off` into *target (absolute = base_addr +
 * displacement) for ANY direct control transfer — an unconditional `jmp rel`, a
 * conditional `jcc rel`, or a `call rel` (x86); `b`/`b.cond`/`bl` (AArch64). Like
 * asmtest_disas_call_target but for the whole JUMP/CALL class: the block-step
 * reconstructor uses it to tell a TAKEN direct branch (target == the observed next
 * block start) from a NOT-TAKEN conditional (target != next start, execution fell
 * through). Returns 1 (direct branch, *target set) or 0 (indirect branch — `jmp
 * r/m`, `ret`, `br`/`blr` — a non-branch, undecodable bytes, or no Capstone), in
 * which case an indirect branch is treated as unconditionally taken (its live next
 * stop is the target). */
int asmtest_disas_branch_target(asmtest_arch_t arch, const uint8_t *code,
                                size_t code_len, uint64_t base_addr,
                                uint64_t off, uint64_t *target) {
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
    cs_option(h, CS_OPT_DETAIL, CS_OPT_ON);
    cs_insn *insn = NULL;
    /* Decode at base_addr+off so Capstone resolves rel8/rel32/imm to the ABSOLUTE
     * target immediate directly (mirrors asmtest_disas_call_target). */
    size_t count = cs_disasm(h, code + off, code_len - (size_t)off,
                             base_addr + off, 1, &insn);
    int ok = 0;
    if (count > 0) {
        /* Only JUMP/CALL-class transfers carry a direct target immediate; RET and
         * IRET are indirect (target on the stack). */
        int is_xfer = cs_insn_group(h, &insn[0], CS_GRP_JUMP) ||
                      cs_insn_group(h, &insn[0], CS_GRP_CALL);
        const cs_detail *d = insn[0].detail;
        if (is_xfer && d != NULL) {
            if (a == CS_ARCH_X86) {
                for (int i = 0; i < d->x86.op_count; i++)
                    if (d->x86.operands[i].type == X86_OP_IMM) {
                        if (target != NULL)
                            *target = (uint64_t)d->x86.operands[i].imm;
                        ok = 1;
                        break;
                    }
            } else if (a == CS_ARCH_ARM64) {
                for (int i = 0; i < d->arm64.op_count; i++)
                    if (d->arm64.operands[i].type == ARM64_OP_IMM) {
                        if (target != NULL)
                            *target = (uint64_t)d->arm64.operands[i].imm;
                        ok = 1;
                        break;
                    }
            }
        }
        cs_free(insn, count);
    }
    cs_close(&h);
    return ok;
#else
    (void)arch;
    (void)code;
    (void)code_len;
    (void)base_addr;
    (void)off;
    (void)target;
    return 0;
#endif
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
static void print_block_line(FILE *out, asmtest_arch_t arch,
                             const uint8_t *code, size_t code_len,
                             uint64_t base_addr, uint64_t off) {
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

void emu_trace_disasm(const emu_trace_t *t, emu_arch_t arch,
                      const uint8_t *code, size_t code_len, FILE *out) {
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
    const char *kind =
        (r->fault_kind >= EMU_FAULT_NONE && r->fault_kind <= EMU_FAULT_FETCH)
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
