/*
 * asmtest_dataflow_internal.h — INTERNAL shared engine for the emulator L0 value
 * producer, so the standalone run (src/dataflow_emu.c) and the emu_t-hosted /
 * resume runs (src/dataflow_resume.c, extension roadmap R3) drive ONE hook
 * machinery through ONE per-guest descriptor. Sharing it is what makes the hosted
 * run byte-identical to the standalone by construction — same seed, same
 * value-capture timing, same finalize; only the engine ownership differs.
 *
 * Composes with R5's per-guest descriptor (src/dataflow_emu.c: df_guest): the
 * seed/capture seam takes the same opaque df_guest the standalone run selects, so
 * the hosted path is guest-neutral too (x86-64 today; a future guest is one more
 * df_guest, not a second copy of this engine).
 *
 * Not a public API and never installed. Both users are src/ TUs already gated on
 * Unicorn + Capstone (mk/dataflow.mk).
 */
#ifndef ASMTEST_DATAFLOW_INTERNAL_H
#define ASMTEST_DATAFLOW_INTERNAL_H

#include "asmtest_valtrace.h" /* asmtest_valtrace_t + asmtest_arch_t (via trace.h) */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Guest sandbox layout — shared by every guest (Unicorn maps whatever address it
 * is told, so the same fixed bases host an x86-64 or AArch64 routine equally). The
 * standalone producer (src/dataflow_emu.c) and the hosted producer
 * (src/dataflow_resume.c, on an emu_t whose emu_open mapped the same regions) both
 * key on these. */
#define DF_CODE_BASE  0x00100000UL
#define DF_CODE_SIZE  0x00010000UL
#define DF_STACK_BASE 0x00200000UL
#define DF_STACK_SIZE 0x00010000UL
#define DF_RET_MAGIC  0x00f00000UL

/* struct uc_struct is Unicorn's uc_engine; struct df_guest is R5's per-guest
 * descriptor — both forward-declared to keep this header free of
 * <unicorn/unicorn.h> and of df_guest's definition (both live in the .c). */
struct uc_struct;
struct df_guest;

/* A Unicorn UC_HOOK_CODE callback: pre-instruction, per step. */
typedef void (*df_code_hook_t)(struct uc_struct *uc, uint64_t address,
                               uint32_t size, void *user);

/* The per-guest descriptor for `arch` (R5), or NULL if no guest is armed for it. */
const struct df_guest *df_guest_for(asmtest_arch_t arch);

/* Seed a fresh entry state for guest `g` on `uc`, whose CODE region (at
 * DF_CODE_BASE) and STACK region (at DF_STACK_BASE) are ALREADY mapped: write the
 * routine bytes (no trap-fill — a fresh Unicorn map is zero-filled), run the
 * guest's deterministic register init + call/return setup, and marshal up to the
 * guest's `nargs` integer args (plus `nfargs` SSE-class FP args where the guest
 * supports them). Byte-for-byte the standalone df_run_impl setup, now callable on
 * an emu_t's engine too. */
void asmtest_df_seed(const struct df_guest *g, struct uc_struct *uc,
                     const uint8_t *code, size_t code_len, const long *iargs,
                     int niargs, const double *fargs, int nfargs);

/* Attach the value-capture hooks (UC_HOOK_CODE + the two memory hooks) to `uc`,
 * run guest `g` from `start_addr` to the DF_RET_MAGIC sentinel (or `max_insns`
 * steps, 0 = unbounded), finalize the last step, and detach — filling *vt with the
 * value trace whose step offsets are relative to `base`. `code`/`code_len` drive
 * the operand enumerator. If `extra_hook` is non-NULL it is added as one more
 * UC_HOOK_CODE with `extra_user` (used by the checkpoint path to snapshot at a
 * chosen step). Returns 0 on a clean run to the sentinel, 1 on a guest
 * fault/error (a partial trace is still produced). Does NOT map, seed, open, or
 * close the engine. */
int asmtest_df_capture(const struct df_guest *g, struct uc_struct *uc,
                       const uint8_t *code, size_t code_len, uint64_t base,
                       uint64_t start_addr, uint64_t max_insns,
                       asmtest_valtrace_t *vt, df_code_hook_t extra_hook,
                       void *extra_user);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ASMTEST_DATAFLOW_INTERNAL_H */
