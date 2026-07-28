/*
 * dataflow_resume.c — the emu_t-HOSTED emulator L0 value producer (extension
 * roadmap R3, docs/internal/gui/30-resume-from-state-and-reweave.md).
 *
 * The self-contained producer (src/dataflow_emu.c) opens its own uc_engine per
 * call, so it can never reach the emu_t checkpoint/restore keystone. This TU runs
 * the SAME value trace on a caller-owned emu_t's engine (via the emu_uc internal
 * seam), so the producer inherits emu_snapshot / emu_restore. It reuses the shared
 * seed/capture seam (asmtest_dataflow_internal.h: asmtest_df_seed +
 * asmtest_df_capture, driven by R5's df_guest descriptor) verbatim, so a hosted
 * run on a fresh emu_open() handle is byte-identical to the standalone run —
 * proved record-for-record by cli/test_reweave.c (T1).
 *
 * On top of the hosted run it adds the checkpoint/resume pair (T2): snapshot the
 * guest at a chosen execution step, then restore and re-run forward under a fresh
 * value trace that aligns to the original by absolute step. That is the seam the
 * Reweave counterfactual (T3) and the Scrubber's "synthesize register history"
 * fallback (T4) build on.
 *
 * x86-64 guest only (the R3 non-goal: a resumed run inherits the value producer's
 * guest scope until a per-guest resume lands). Split out of dataflow_emu.c
 * deliberately: only THIS object references emu_*, so only the links that want the
 * hosted path (test_reweave) pull in emu.o. Every other consumer of
 * dataflow_emu.o stays emu-free.
 *
 * Requires Unicorn (engine) + the emulator tier (emu.o). Built under the same gate
 * as dataflow_emu.o (mk/dataflow.mk).
 */
#include "asmtest_valtrace.h"

#include "asmtest_dataflow_internal.h" /* DF_CODE_BASE + df_guest_for + seed/capture */
#include "asmtest_emu.h"               /* emu_t, emu_snapshot_t + snapshot/restore  */
#include "asmtest_emu_internal.h"      /* emu_uc — the raw handle behind emu_t      */
#include <stddef.h>
#include <string.h>
#include <unicorn/unicorn.h>

/* The resume producer is x86-64-only for now (see the file banner). */
static const struct df_guest *x86_guest(void) {
    return df_guest_for(ASMTEST_ARCH_X86_64);
}

/*
 * Run `code` on the caller-owned emu_t `e` and fill *vt with its value trace,
 * exactly like asmtest_dataflow_emu_run but on e's engine — so e's
 * emu_snapshot / emu_restore and per-step ring see the run. `e` must be a fresh
 * emu_open() handle (its CODE + STACK regions are already mapped at the guest
 * addresses the seam expects); on such a handle the trace is byte-identical to the
 * standalone producer's. Returns 0 clean / 1 guest-fault / -1 setup error.
 */
int asmtest_dataflow_emu_run_hosted(emu_t *e, const uint8_t *code,
                                    size_t code_len, const long *args,
                                    int nargs, uint64_t max_insns,
                                    asmtest_valtrace_t *vt) {
    if (e == NULL || vt == NULL || code == NULL)
        return -1;
    struct uc_struct *uc = emu_uc(e);
    const struct df_guest *g = x86_guest();
    if (uc == NULL || g == NULL)
        return -1;
    vt->mem_space = AT_LOC_MEM_ABS;
    asmtest_df_seed(g, uc, code, code_len, args, nargs, NULL, 0);
    return asmtest_df_capture(g, uc, code, code_len, DF_CODE_BASE, DF_CODE_BASE,
                              max_insns, vt, NULL, NULL);
}

/* Extra UC_HOOK_CODE for the checkpoint run: fires pre-instruction alongside the
 * value-capture hook, and takes an emu_snapshot at the pre-instruction state of
 * absolute step `k` (0-based) — the exact register + memory state that will feed
 * step k. Counting per firing stays in lockstep with the df step numbering (both
 * are whole-space UC_HOOK_CODE, one firing per executed instruction). */
typedef struct {
    emu_t *e;
    uint64_t k;
    uint64_t count;
    emu_snapshot_t *snap;
} df_ckpt_ctx;

static void df_snap_at_k(struct uc_struct *uc, uint64_t address, uint32_t size,
                         void *user) {
    (void)uc;
    (void)address;
    (void)size;
    df_ckpt_ctx *cc = (df_ckpt_ctx *)user;
    if (cc->count == cc->k && cc->snap == NULL)
        cc->snap = emu_snapshot(cc->e); /* read-only query; safe in a code hook */
    cc->count++;
}

/*
 * Run `code` on `e` producing the full value trace in *vt AND capturing an
 * emu_snapshot at the pre-instruction state of absolute step `k`. The snapshot is
 * returned via *snap_out (owned by the caller — free with emu_snapshot_free), or
 * NULL if the run executed <= k steps. Returns like _run_hosted. This is a
 * superset of _run_hosted: with no reachable k it produces the same trace.
 */
int asmtest_dataflow_emu_checkpoint(emu_t *e, const uint8_t *code,
                                    size_t code_len, const long *args, int nargs,
                                    uint64_t max_insns, uint64_t k,
                                    asmtest_valtrace_t *vt,
                                    emu_snapshot_t **snap_out) {
    if (e == NULL || vt == NULL || code == NULL || snap_out == NULL)
        return -1;
    *snap_out = NULL;
    struct uc_struct *uc = emu_uc(e);
    const struct df_guest *g = x86_guest();
    if (uc == NULL || g == NULL)
        return -1;
    vt->mem_space = AT_LOC_MEM_ABS;
    asmtest_df_seed(g, uc, code, code_len, args, nargs, NULL, 0);
    df_ckpt_ctx cc = {e, k, 0, NULL};
    int rc = asmtest_df_capture(g, uc, code, code_len, DF_CODE_BASE, DF_CODE_BASE,
                                max_insns, vt, df_snap_at_k, &cc);
    *snap_out = cc.snap;
    return rc;
}

/*
 * Run forward from `e`'s CURRENT guest state (wherever rip points) to the
 * sentinel, filling *vt with a fresh value trace. Used after emu_restore to
 * resume a checkpoint — and, with an edit applied first, to run a counterfactual.
 * Offsets stay relative to DF_CODE_BASE so the trace aligns with the original's.
 * `code`/`code_len` drive the operand enumerator; the guest bytes are already in
 * the engine. Returns like _run_hosted.
 */
int asmtest_dataflow_emu_run_from_current(emu_t *e, const uint8_t *code,
                                          size_t code_len,
                                          asmtest_valtrace_t *vt) {
    if (e == NULL || vt == NULL || code == NULL)
        return -1;
    struct uc_struct *uc = emu_uc(e);
    const struct df_guest *g = x86_guest();
    if (uc == NULL || g == NULL)
        return -1;
    vt->mem_space = AT_LOC_MEM_ABS;
    uint64_t rip = 0;
    uc_reg_read((uc_engine *)uc, UC_X86_REG_RIP, &rip);
    return asmtest_df_capture(g, uc, code, code_len, DF_CODE_BASE, rip, 0, vt,
                              NULL, NULL);
}

/*
 * Resume from a checkpoint `snap` (from asmtest_dataflow_emu_checkpoint on the
 * SAME `e`): restore the guest to the pre-step-k state, then re-run forward to the
 * sentinel under a FRESH value trace in *vt. The resumed trace is the TAIL of the
 * original, aligned by absolute step — resumed step j reproduces the original's
 * step k+j byte-for-byte, because the restored register + memory state is the
 * original's pre-step-k state exactly. For a counterfactual, split this:
 * emu_restore(e, snap), then asmtest_dataflow_emu_edit_gp(...), then
 * asmtest_dataflow_emu_run_from_current — the edit diverges the tail only where it
 * reaches. Returns like _run_hosted; -1 if the restore fails.
 */
int asmtest_dataflow_emu_resume(emu_t *e, const emu_snapshot_t *snap,
                                const uint8_t *code, size_t code_len,
                                asmtest_valtrace_t *vt) {
    if (e == NULL || snap == NULL || vt == NULL || code == NULL)
        return -1;
    if (!emu_restore(e, snap))
        return -1;
    return asmtest_dataflow_emu_run_from_current(e, code, code_len, vt);
}

/* Poke an x86-64 GP register (by SysV/x86 name: "rax".."r15", "rip", "rflags") on
 * `e`'s CURRENT guest state — the edit-at-step-K seam for the Reweave
 * counterfactual (applied between emu_restore and _run_from_current). Returns
 * false for an unknown name. Keeps the raw Unicorn register ids inside this TU so
 * a caller (test / desktop fork engine) needs no <unicorn/unicorn.h>. */
bool asmtest_dataflow_emu_edit_gp(emu_t *e, const char *name, uint64_t val) {
    if (e == NULL || name == NULL)
        return false;
    static const struct {
        const char *name;
        int uc_reg;
    } tab[] = {
        {"rax", UC_X86_REG_RAX}, {"rbx", UC_X86_REG_RBX},
        {"rcx", UC_X86_REG_RCX}, {"rdx", UC_X86_REG_RDX},
        {"rsi", UC_X86_REG_RSI}, {"rdi", UC_X86_REG_RDI},
        {"rbp", UC_X86_REG_RBP}, {"rsp", UC_X86_REG_RSP},
        {"r8", UC_X86_REG_R8},   {"r9", UC_X86_REG_R9},
        {"r10", UC_X86_REG_R10}, {"r11", UC_X86_REG_R11},
        {"r12", UC_X86_REG_R12}, {"r13", UC_X86_REG_R13},
        {"r14", UC_X86_REG_R14}, {"r15", UC_X86_REG_R15},
        {"rip", UC_X86_REG_RIP}, {"rflags", UC_X86_REG_EFLAGS},
    };
    struct uc_struct *uc = emu_uc(e);
    if (uc == NULL)
        return false;
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (strcmp(name, tab[i].name) == 0)
            return uc_reg_write((uc_engine *)uc, tab[i].uc_reg, &val) ==
                   UC_ERR_OK;
    }
    return false;
}
