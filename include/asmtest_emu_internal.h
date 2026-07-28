/*
 * asmtest_emu_internal.h — INTERNAL cross-TU seam for the emulator tier.
 *
 * Not a public API and never installed: this exposes the raw Unicorn handle
 * behind an opaque emu_t so the sibling value-producer tier (src/dataflow_emu.c
 * / src/dataflow_resume.c, extension roadmap R3) can seed registers, attach its
 * value-capture hooks, and drive uc_emu_start on the SAME uc_engine that carries
 * emu_snapshot / emu_restore and the per-step register ring. That is what lets
 * the L0 value producer inherit checkpoint/resume without opening its own
 * engine. It is deliberately kept out of the public asmtest_emu.h: handing out
 * the uc handle bypasses the emu_t abstraction, so only in-tree producer tiers
 * use it (the blockstep/descent tiers carry the same *_internal.h convention).
 */
#ifndef ASMTEST_EMU_INTERNAL_H
#define ASMTEST_EMU_INTERNAL_H

#include "asmtest_emu.h" /* emu_t */

#ifdef __cplusplus
extern "C" {
#endif

/* struct uc_struct is Unicorn's `typedef struct uc_struct uc_engine;`. Forward-
 * declaring it lets this header expose the accessor without pulling all of
 * <unicorn/unicorn.h> in; a TU that also includes unicorn.h sees the identical
 * type, so `uc_engine *u = emu_uc(e);` needs no cast. */
struct uc_struct;

/* The Unicorn engine backing `e`, or NULL for a NULL handle. INTERNAL. */
struct uc_struct *emu_uc(emu_t *e);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ASMTEST_EMU_INTERNAL_H */
