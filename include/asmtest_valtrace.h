/*
 * asmtest_valtrace.h — the shared data-flow substrate (L0 value trace, L1 def-use,
 * L2 slice), the analysis counterpart to asmtest_trace.h.
 *
 * Where asmtest_trace_t records *which instructions ran* (ordered offsets + basic
 * blocks), this header records *how data moved*: a per-step VALUE TRACE (L0) whose
 * caller-owned buffers follow the exact append / truncate discipline of
 * asmtest_trace_t (trace.c), a tier-neutral last-writer DEF-USE graph (L1), and
 * forward / backward SLICES (L2) built once over any L0 trace. Every tier — the
 * Unicorn emulator producer (src/dataflow_emu.c), and later the scoped ptrace
 * stepper and the DynamoRIO taint client — fills the SAME asmtest_valtrace_t, so
 * the analysis layers are written once and shared, exactly as the trace sink is.
 *
 * Address-space normalization contract: a value trace mixes an effective MEMORY
 * address (absolute, or a routine offset in region mode) with instruction OFFSETS,
 * so every operand record carries its own space tag (at_loc_kind_t) and the whole
 * capture declares its mem_space, which the L1 linker normalizes against.
 *
 * Two dependency tiers, deliberately split across translation units:
 *   - the sink (append / new / free) + L1 + L2 are PURE C (src/dataflow.c): no
 *     Capstone, no Unicorn — they compile and unit-test on every host.
 *   - the operand read/write-set enumerator (src/dataflow_operands.c) needs
 *     Capstone detail mode; it degrades to a no-op (returns 0) without it.
 * See docs/internal/plans/data-flow-tracing-plan.md (Phases 0-2).
 */
#ifndef ASMTEST_VALTRACE_H
#define ASMTEST_VALTRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asmtest_trace.h" /* asmtest_arch_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* L0 — the shared value-trace sink                                    */
/* ------------------------------------------------------------------ */

/* The address space a location lives in — the normalization tag every operand
 * record carries so a value trace can mix an absolute effective address (MEM_ABS,
 * the ptrace / emulator whole-window mode) with a routine-relative one (MEM_OFF,
 * region mode) and still be linked correctly by L1. Registers are space-neutral. */
typedef enum {
    AT_LOC_REG = 0, /* a register (reg = Capstone reg id)                    */
    AT_LOC_MEM_ABS =
        1, /* memory at an absolute effective address               */
    AT_LOC_MEM_OFF =
        2, /* memory at a routine-relative offset (region mode)     */
} at_loc_kind_t;

/* One operand read/write record. For a register only {kind, reg, is_write} and the
 * captured value matter; for a memory operand the addressing terms
 * (base/index/scale/disp/segment) are filled by the operand enumerator at DECODE
 * time and `addr` is resolved to the effective address by a PRODUCER at RUN time
 * (0 until then). Values <= 8 bytes live inline in `value`; wider values (XMM/YMM,
 * up to 64 bytes) spill to the valtrace's `wide[]` side buffer at `wide_off`. */
typedef struct at_val_rec {
    at_loc_kind_t kind;
    uint32_t
        reg; /* AT_LOC_REG: Capstone reg id. MEM: segment reg id (0 none) */
    uint32_t
        base; /* MEM: base reg id (0 = none)                               */
    uint32_t
        index; /* MEM: index reg id (0 = none)                              */
    int32_t
        scale; /* MEM: index scale (1/2/4/8; 0 = no index)                  */
    int64_t
        disp; /* MEM: displacement                                         */
    uint64_t
        addr; /* MEM: resolved effective address (producer-filled)         */
    uint16_t
        size; /* width in bytes (0 = unknown; up to 64 for AVX-512)        */
    bool
        is_write; /* read-set vs write-set (from Capstone .access)             */
    bool
        value_valid; /* value / wide bytes are populated                        */
    bool wide; /* value spilled to wide[] (size > 8)                      */
    uint32_t
        wide_off;   /* byte offset into asmtest_valtrace_t.wide when `wide`   */
    uint64_t value; /* inline value for size <= 8                             */
    uint32_t step;  /* index into insn_off[]; stamped by _append             */
} at_val_rec_t;

/* The L0 sink. Zero it (or use asmtest_valtrace_new) and point the three arrays at
 * caller-owned buffers; each fills append-only and flips `truncated` when a buffer
 * overflows — the same honest-overflow contract as asmtest_trace_t. `insn_off`
 * parallels asmtest_trace_t.insns (one entry per executed step); `recs` is the
 * flattened operand stream, every record stamped with its `step`. */
typedef struct asmtest_valtrace {
    uint64_t
        *insn_off; /* per-step instruction offset (parallels the trace sink) */
    size_t steps_cap;
    size_t steps_len;
    uint64_t
        steps_total; /* steps seen (counts past steps_cap)                   */

    at_val_rec_t
        *recs; /* flattened operand records, caller-owned                */
    size_t recs_cap;
    size_t recs_len;
    uint64_t
        recs_total; /* records seen (counts past recs_cap)                   */

    uint8_t
        *wide; /* side buffer for values > 8 bytes                            */
    size_t wide_cap;
    size_t wide_len;

    bool truncated; /* a buffer filled and at least one entry dropped    */
    at_loc_kind_t
        mem_space; /* the memory normalization space for this capture   */
} asmtest_valtrace_t;

/* Allocate the sink plus its three caller-owned buffers in one handle (any cap may
 * be 0 to skip that dimension). mem_space defaults to AT_LOC_MEM_ABS; a producer
 * may overwrite it. */
asmtest_valtrace_t *asmtest_valtrace_new(size_t steps_cap, size_t recs_cap,
                                         size_t wide_cap);
void asmtest_valtrace_free(asmtest_valtrace_t *v);

/* Append one executed step at instruction offset `off`, copying its `n` operand
 * records and stamping each with the new step index. Append-only + truncate: when
 * a buffer is full the entry is dropped and `truncated` is set, but the *_total
 * counters still advance (so overflow is honest). A NULL sink is a no-op. */
void asmtest_valtrace_append(asmtest_valtrace_t *v, uint64_t off,
                             const at_val_rec_t *recs, size_t n);

/* Copy `n` bytes of a wide (>8B) value into the wide[] side buffer and return its
 * byte offset (set a record's wide_off to it, and wide = true). Returns SIZE_MAX
 * and sets `truncated` if wide[] cannot hold it. */
size_t asmtest_valtrace_stash_wide(asmtest_valtrace_t *v, const void *bytes,
                                   size_t n);

/* Convenience readers (steps / records actually stored). */
size_t asmtest_valtrace_steps(const asmtest_valtrace_t *v);
size_t asmtest_valtrace_recs(const asmtest_valtrace_t *v);

/* ------------------------------------------------------------------ */
/* Operand read/write-set enumerator (Capstone, detail mode)           */
/* ------------------------------------------------------------------ */

/* True iff this build links Capstone, so asmtest_operands produces real read/write
 * sets rather than degrading to 0. */
bool asmtest_operands_available(void);

/* Enumerate the READ-set and WRITE-set of the single instruction at code[off] for
 * `arch`, using one PERSISTENT Capstone handle (detail on) — never a per-call
 * cs_open/cs_close. Register accesses (explicit AND implicit: eflags, rsp on
 * push/pop/call/ret, string counters) come from cs_regs_access; memory operands
 * carry their base/index/scale/disp/segment terms (addr is left 0 for a producer
 * to resolve at run time). `*nreads` / `*nwrites` are in/out: capacity on input,
 * count written on output (either buffer/count pointer may be NULL to skip that
 * direction). Returns the instruction byte length, or 0 when the bytes do not
 * decode, the arch is stubbed (ARM32 / RISCV64), or Capstone is absent. */
size_t asmtest_operands(asmtest_arch_t arch, const uint8_t *code, size_t len,
                        uint64_t off, at_val_rec_t *reads, size_t *nreads,
                        at_val_rec_t *writes, size_t *nwrites);

/* ------------------------------------------------------------------ */
/* L1 — last-writer def-use graph (tier-neutral, pure)                 */
/* ------------------------------------------------------------------ */

/* A def-use edge: the value written at from_step is read at to_step through `loc`
 * (the consumer's read record). Register def-use is exact; memory def-use keys on
 * the normalized address space per byte and is "GC-uncanonicalized" (a raw address
 * collision aliases) until the managed layer lands — a documented limitation. */
typedef struct asmtest_defuse_edge {
    uint32_t
        from_step; /* the producing step (last writer)                       */
    uint32_t
        to_step; /* the consuming step (the reader)                        */
    at_val_rec_t
        loc; /* the dependence-carrying location (the read)            */
} asmtest_defuse_edge_t;

typedef struct asmtest_defuse {
    asmtest_defuse_edge_t *edges;
    size_t n; /* number of edges                                             */
    size_t
        nsteps; /* step count the graph spans (slice bound)                    */
} asmtest_defuse_t;

/* Build the last-writer def-use graph over an L0 trace: at each step, every read
 * location resolves to the step that last wrote it (an edge), then the writes
 * update the last-writer map. Caller frees with asmtest_defuse_free. */
asmtest_defuse_t *asmtest_defuse_build(const asmtest_valtrace_t *v);
void asmtest_defuse_free(asmtest_defuse_t *g);

/* ------------------------------------------------------------------ */
/* L2 — forward / backward slices (tier-neutral, pure)                 */
/* ------------------------------------------------------------------ */

/* A slice: the ascending, de-duplicated set of step indices reached (the origin
 * step included). */
typedef struct asmtest_slice {
    uint32_t *steps;
    size_t n;
} asmtest_slice_t;

/* Forward slice ("what does the value at seed.step influence?"): BFS along def-use
 * edges from seed.step in the producer->consumer direction. */
asmtest_slice_t *asmtest_slice_forward(const asmtest_defuse_t *g,
                                       at_val_rec_t seed);

/* Backward slice ("what produced the value at sink.step?"): BFS along def-use edges
 * from sink.step in the consumer->producer direction. */
asmtest_slice_t *asmtest_slice_backward(const asmtest_defuse_t *g,
                                        at_val_rec_t sink);

void asmtest_slice_free(asmtest_slice_t *s);
/* 1 if `step` is in the slice, else 0. */
int asmtest_slice_contains(const asmtest_slice_t *s, uint32_t step);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_VALTRACE_H */
