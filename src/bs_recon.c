/*
 * bs_recon.c — block-step terminator scanner + branch classifier. See bs_recon.h.
 *
 * Pure decode logic over asmtest_disas* (the shared Capstone length-decoder): no
 * platform #if guard needed, the disas layer already self-stubs off its supported
 * hosts. Extracted verbatim from src/ptrace_backend.c (inproc-btf-block-step.md T1) —
 * classify_branch is exported (non-static): window_scan_terminator, which stays in
 * ptrace_backend.c, calls it directly across the TU boundary.
 */
#include "bs_recon.h"

#include "asmtest_trace.h"

br_kind_t classify_branch(asmtest_arch_t arch, const uint8_t *code,
                          size_t code_len, uint64_t base_addr, uint64_t off,
                          uint64_t next_pc, size_t *len_out) {
    int is_call = 0, is_ret = 0;
    size_t l =
        asmtest_disas_probe(arch, code, code_len, off, &is_call, &is_ret);
    *len_out = l;
    if (l == 0)
        return BR_NONE; /* caller checks *len_out */
    if (!asmtest_disas_is_branch(arch, code, code_len, off))
        return BR_NONE;
    if (is_ret)
        return BR_HARD_UNKNOWN;
    uint64_t target = 0;
    if (!asmtest_disas_branch_target(arch, code, code_len, base_addr, off,
                                     &target))
        return BR_HARD_UNKNOWN; /* indirect jmp/call: always taken, no static target */
    /* A direct `call rel` and a direct `jmp rel` are unconditional — always taken. */
    if (is_call || asmtest_disas_is_uncond_jump(arch, code, code_len, off))
        return target == next_pc ? BR_HARD_HIT : BR_HARD_MISS;
    return target == next_pc ? BR_COND_HIT : BR_COND_MISS;
}

int asmtest_bs_scan_terminator(asmtest_arch_t arch, const uint8_t *code,
                               size_t len, uint64_t base_ip, uint64_t from_off,
                               uint64_t next_pc, uint64_t *term_out) {
    uint64_t walk = from_off;
    uint64_t first_cand = 0;
    unsigned ncand = 0;
    for (size_t guard = 0; guard <= len; guard++) {
        if (walk >= len)
            break; /* ran off the region */
        size_t l = 0;
        br_kind_t k =
            classify_branch(arch, code, len, base_ip, walk, next_pc, &l);
        if (l == 0)
            break; /* undecodable */
        if (k == BR_COND_HIT || k == BR_HARD_HIT) {
            if (ncand++ == 0)
                first_cand = walk;
            if (k == BR_HARD_HIT)
                goto decided; /* always taken: nothing past it runs */
            walk += l;
            continue; /* conditional: keep scanning to prove uniqueness */
        }
        if (k == BR_HARD_UNKNOWN) {
            if (ncand == 0) {
                *term_out = walk; /* ret/indirect: always taken, must be it */
                return ASMTEST_BS_OK;
            }
            goto decided;
        }
        if (k == BR_HARD_MISS)
            goto decided; /* always taken, but elsewhere: the run ended earlier */
        walk += l;        /* BR_NONE / BR_COND_MISS: the run continues */
    }
    /* Fell out: off the region, undecodable, or the ceiling. */
    if (ncand == 0)
        return ASMTEST_BS_FAIL; /* no terminator reaches next_pc: desynced */
    *term_out = first_cand;
    return ASMTEST_BS_AMBIGUOUS; /* a candidate, but uniqueness unproven: do not guess */
decided:
    if (ncand == 0)
        return ASMTEST_BS_FAIL;
    *term_out = first_cand;
    return ncand == 1 ? ASMTEST_BS_OK : ASMTEST_BS_AMBIGUOUS;
}
