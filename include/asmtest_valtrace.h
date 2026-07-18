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
 * See docs/internal/archive/plans/data-flow-tracing-plan.md (Phases 0-2).
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

/* By-pointer seed variants of asmtest_slice_forward / _backward. Only seed->step
 * is read (as by-value today), but a pointer argument crosses every FFI — the
 * by-value at_val_rec_t is SysV MEMORY-class, which Ruby Fiddle and other dynamic
 * FFIs cannot express as a value argument. A NULL seed is treated as step 0. */
asmtest_slice_t *asmtest_slice_forward_seed(const asmtest_defuse_t *g,
                                            const at_val_rec_t *seed);
asmtest_slice_t *asmtest_slice_backward_seed(const asmtest_defuse_t *g,
                                             const at_val_rec_t *seed);

void asmtest_slice_free(asmtest_slice_t *s);
/* 1 if `step` is in the slice, else 0. */
int asmtest_slice_contains(const asmtest_slice_t *s, uint32_t step);

/* ------------------------------------------------------------------ */
/* Phase 4 (increment 1) — PC -> method identity + version             */
/*                                                                     */
/* The managed-taint PREREQUISITE: attribute each executed step of an   */
/* L0 value trace to the METHOD + VERSION whose JIT-compiled body owns   */
/* its instruction PC. The method-map is the SAME shape the asmspy       */
/* jitdump reader (code_addr / code_size / name / code_index) and the    */
/* text perf-map (addr / size / name, version 0) both produce, and the   */
/* §D3 addr-channel (asmtest_addr_channel.h) publishes (base / len /     */
/* version). Pure address math — no Capstone, no Unicorn — so it runs on  */
/* every host, exactly like the L1 / L2 passes above.                    */
/*                                                                       */
/* Versioning mirrors the code-image recorder's concept (asmtest_code-    */
/* image.h): a monotonic version stamps each compilation. Tiered re-JIT   */
/* is handled two ways, both by the version field: an IN-PLACE recompile  */
/* at a REUSED address leaves both records live and the GREATEST version   */
/* wins for that address; a re-JIT to a NEW address is simply a new        */
/* record carrying a higher version, so its PCs attribute to the new       */
/* version while the old body's PCs still resolve to the old one. GC-move  */
/* object-identity canonicalization is DEFERRED to a later increment.      */
/* ------------------------------------------------------------------ */

/* One method-map entry. [addr, addr+size) bounds the emitted body (half-open);
 * size == 0 means unknown extent — a point match on `addr` only. `name` is the
 * method identity (borrowed; the caller's map owns its strings). `version` is
 * the code_index / re-JIT counter described above. */
typedef struct asmtest_method {
    uint64_t addr;
    uint64_t size;
    const char *name;
    uint64_t version;
} asmtest_method_t;

/* Per-step attribution, one entry per value-trace step. `method` is a STABLE
 * identity id: records naming the same method share it (compact, assigned in
 * first-seen order over the map), so a method keeps its identity across a
 * tiered re-JIT even when its version and address change; it is -1 when the PC
 * is in no method. `record` is the exact owning method-map index (-1 when
 * unattributed); `version` is that record's version/code_index (0 when
 * unattributed). */
typedef struct asmtest_method_attr {
    int32_t method;
    int32_t record;
    uint64_t version;
} asmtest_method_attr_t;

/* Resolve `pc` to the method-map record that owns it: the record whose
 * [addr, addr+size) contains pc (or, for a size == 0 record, whose addr equals
 * pc) and — when several records cover pc after an in-place tiered re-JIT at a
 * reused address — the one with the GREATEST version (ties resolve to the last
 * such record, the newest load). Returns the record index in `methods`, or -1
 * if pc is owned by no method. Pure; the map need not be sorted (a linear scan
 * keeps the tiered-collision rule simple and a JIT method-map is small). */
int asmtest_method_resolve_pc(const asmtest_method_t *methods, size_t nmethods,
                              uint64_t pc);

/* Attribute every step of `v` — its per-step instruction offset read as an
 * absolute PC — to its owning method + version, writing up to `out_cap` entries
 * into `out` (one per step, in step order). Reuses asmtest_method_resolve_pc per
 * step and assigns each distinct method NAME a stable identity id, so re-JIT'd
 * versions of one method share their `method` id while their `version` differs.
 * Returns the number of steps written (min(steps_len, out_cap)), or -1 on a NULL
 * trace or NULL out. A NULL / empty method-map attributes every step to -1. */
int asmtest_method_attribute(const asmtest_method_t *methods, size_t nmethods,
                             const asmtest_valtrace_t *v,
                             asmtest_method_attr_t *out, size_t out_cap);

/* ------------------------------------------------------------------ */
/* Phase 4 (increment 2) — GC-move address canonicalization            */
/*                                                                     */
/* The HARD half of the managed-interpretability layer. When the .NET   */
/* GC COMPACTS the heap, a live object at OldRangeBase is relocated to   */
/* NewRangeBase; a raw L0 value trace captured ACROSS the compaction     */
/* then keys memory def-use on addresses that alias FALSELY. A store to  */
/* an object BEFORE the move (at the old address) and the matching load  */
/* AFTER the move (at the new address) look unrelated, so the def-use    */
/* edge is LOST; and an unrelated object that later occupies the vacated */
/* old address ALIASES the first, forging a false edge. Both break the   */
/* memory last-writer map (asmtest_defuse_build), which is "GC-uncanoni- */
/* calized" until this pass runs.                                        */
/*                                                                       */
/* This canonicalizer consumes the move-range records EventPipe's         */
/* GCBulkMovedObjectRanges publishes — {OldRangeBase, NewRangeBase,        */
/* RangeLength}, each tagged with the value-trace STEP boundary its GC     */
/* takes effect at — and remaps every absolute memory address in a value  */
/* trace to a STABLE canonical identity: the object's FINAL resting        */
/* address after every compaction that will relocate it. A pre-move access */
/* is forwarded to that final address; a post-move access already sits     */
/* there; so the def-use survives the move, the offset WITHIN the object   */
/* is preserved (i.e. (object, field) identity), and an unrelated object   */
/* that reuses the vacated old address is NOT forwarded, so it no longer   */
/* aliases.                                                                */
/*                                                                        */
/* PURE address math — no Capstone, no Unicorn, no runtime — so it unit-    */
/* tests on every host, exactly like the L1 / L2 passes above. The LIVE     */
/* EventPipe feed that turns GCBulkMovedObjectRanges events into            */
/* asmtest_gcmove_t records at the right step boundaries is a LATER          */
/* increment; this is the pure transform it will drive. Full object          */
/* identity via GCBulkType / Node / Edge is likewise deferred.               */
/* ------------------------------------------------------------------ */

/* One GC move-range record — the shape of an EventPipe GCBulkMovedObjectRanges
 * entry. The half-open heap range [old_base, old_base+len) was relocated to
 * [new_base, new_base+len) by a compacting GC. `step` is the value-trace step
 * boundary the compaction takes effect at: a record at a step < `step` predates
 * the move (its address is in the OLD space and is forwarded), a record at a
 * step >= `step` postdates it (already at the NEW address). Ranges sharing one
 * `step` model a single GC's GCBulkMovedObjectRanges batch — they are disjoint
 * in the old space, so an address is relocated at most once per boundary. */
typedef struct asmtest_gcmove {
    uint64_t old_base;
    uint64_t new_base;
    uint64_t len;
    uint32_t step;
} asmtest_gcmove_t;

/* Map an absolute memory address `phys` observed at value-trace `step` to its
 * canonical address — the object's FINAL resting place after every compaction
 * (a move whose boundary is strictly greater than `step`) that will relocate it.
 * Pure and allocation-free: `moves` MUST be sorted ascending by `step` (the
 * order EventPipe emits GC events in; ranges within one GC may appear in any
 * order among themselves). The same (moves, step, phys) always yields the same
 * result; a NULL / empty move set returns `phys` unchanged. */
uint64_t asmtest_gcmove_canon(const asmtest_gcmove_t *moves, size_t nmoves,
                              uint32_t step, uint64_t phys);

/* Rewrite every ABSOLUTE-memory (AT_LOC_MEM_ABS) record's resolved `addr` in `v`
 * to its canonical address (asmtest_gcmove_canon), in place, so a subsequent
 * asmtest_defuse_build links def-use across GC compactions with no false alias.
 * Register and routine-offset (AT_LOC_MEM_OFF) records are left untouched — GC
 * moves live in the absolute heap space. `moves` need not be pre-sorted: the
 * caller's array is used directly when already ascending by `step`, else a
 * private sorted copy is made. Returns the count of records whose address
 * actually changed, or (size_t)-1 on a NULL trace. Call it ONCE on a freshly
 * captured trace (it mutates the addresses in place). */
size_t asmtest_gcmove_canonicalize(asmtest_valtrace_t *v,
                                   const asmtest_gcmove_t *moves,
                                   size_t nmoves);

/* ------------------------------------------------------------------ */
/* Phase 4 (increment 4) — real OBJECT identity                        */
/*                                                                     */
/* Increment 2 above keys managed memory def-use on ADDRESSES: it forwards
 * every pre-compaction access to the object's FINAL resting place, which
 * reconnects an object's own def-use across a GC and separates an unrelated
 * object that reuses a vacated slot. That correctly shipped and nothing here
 * changes it. What address identity CANNOT express is its one documented
 * residual: a pre-window record touching memory that a GC then slides a LIVE
 * object into ALIASES that object, forging a def-use edge between two things
 * that were never the same object — addresses alone cannot tell them apart.
 *
 * This increment adds real OBJECT identity. A heap SNAPSHOT taken AFTER the
 * capture — live {addr, size, type_id} nodes from the runtime's GCBulkNode /
 * GCBulkEdge / GCBulkType events, in the SNAPSHOT address space (after every
 * compaction has been applied) — is joined with the increment-2 move set, kept
 * recording until the snapshot. asmtest_objid_locate INVERTS asmtest_gcmove_-
 * canon (walk the batches DESCENDING by step, inverting the at-most-one range
 * whose NEW span covers the running address), so for any record (step s, raw
 * address x) we can ask WHICH node occupied x at step s. A record with an owner
 * keys on (node.addr + offset) — object identity, byte-equal to the increment-2
 * forward canon for genuinely-owned bytes. A no-owner record whose forwarded
 * address nonetheless lands inside a live node's snapshot range is the
 * false-alias case; it is re-keyed into a RESERVED space (bit 63 set — never a
 * canonical Linux x86-64 user address) so it can no longer collide with the
 * object. Everywhere the snapshot is silent the result is BYTE-IDENTICAL to
 * increment 2: object identity REFINES address identity, never replaces it
 * (nnodes == 0 degrades to asmtest_gcmove_canonicalize exactly).
 *
 * PURE C — no Capstone, no Unicorn, no runtime — the same dependency tier as
 * dataflow.c / dataflow_gcmove.c, so it unit-tests on every host; it links
 * AGAINST dataflow_gcmove.c for the forward canon it refines. The LIVE feed
 * (the gccanon-attach lane's EventPipe heap dumper + the post-capture
 * MovedReferences2 stamps) drives this pure transform; see
 * docs/internal/implementations/dataflow-f4-object-identity.md.
 * ------------------------------------------------------------------ */

/* One heap-snapshot node — the shape of a GCBulkNode entry (Address / Size /
 * TypeID; the event's EdgeCount is a stream-consistency check for the reader,
 * not an identity input, so it is not carried here). `addr` is in the SNAPSHOT
 * space: the address space AFTER every batch in the accompanying move set has
 * been applied (the object's final resting place). */
typedef struct asmtest_gcnode {
    uint64_t addr;
    uint64_t size;
    uint64_t type_id; /* GCBulkType join key; 0 = unknown */
} asmtest_gcnode_t;

/* Inverse of asmtest_gcmove_canon: map a SNAPSHOT-space address back to where it
 * sat at `step`. Walk the batches DESCENDING by step; for each batch whose
 * boundary POSTDATES `step` (batch.step > step), invert the at-most-one range
 * whose NEW span covers the running address. Pure and allocation-free; `moves`
 * MUST be sorted ascending by `step` (as asmtest_gcmove_canon requires) AND have
 * DISJOINT new spans within each batch — the precondition asmtest_objid_owner /
 * asmtest_objid_canonicalize enforce for it by dropping overlapping-new ranges
 * before calling in. A NULL / empty move set returns `snap_addr` unchanged. */
uint64_t asmtest_objid_locate(const asmtest_gcmove_t *moves, size_t nmoves,
                              uint32_t step, uint64_t snap_addr);

/* The heap node that CONTAINED `raw_addr` at value-trace `step`: the node i whose
 * step-time span [asmtest_objid_locate(nodes[i].addr, step), + nodes[i].size)
 * covers raw_addr. Returns 0 and fills *owner (node index) and *offset (raw_addr
 * minus the node's step-time base) on a hit, or -1 when no node owned the byte (a
 * conservative miss — the caller keeps the address-identity floor). `moves` need
 * not be pre-sorted or disjoint: a private sorted copy is made and overlapping
 * NEW spans within a batch are dropped for the inverse walk (an undecidable owner
 * becomes "no owner", never a guessed identity). Either out-pointer may be NULL. */
int asmtest_objid_owner(const asmtest_gcnode_t *nodes, size_t nnodes,
                        const asmtest_gcmove_t *moves, size_t nmoves,
                        uint32_t step, uint64_t raw_addr, size_t *owner,
                        uint64_t *offset);

/* Re-key tag OR'd into the canonical address of a no-owner record whose forwarded
 * address collides with a live node's snapshot range. Bit 63 is never set in a
 * canonical Linux x86-64 user-space effective address, so a tagged key can never
 * equal an untagged one (asmtest_defuse_build compares keys for equality only),
 * and two no-owner records sharing one canon address share one tagged key (their
 * mutual edges survive) while neither can alias the object. */
#define ASMTEST_OBJID_NOOBJ (1ull << 63)

/* Rewrite every AT_LOC_MEM_ABS record in `v` in place, keying each on (object,
 * offset) where the snapshot has evidence and degrading to increment-2 address
 * identity where it does not:
 *   owner found            -> addr = nodes[owner].addr + offset
 *   no owner, no collision -> addr = asmtest_gcmove_canon(moves, ..., addr)
 *   no owner, canon(addr)
 *     inside a node's range -> addr = asmtest_gcmove_canon(...) | ASMTEST_OBJID_NOOBJ
 * The owner branch is byte-equal to the forward canon for genuinely-owned bytes
 * (an object forwards affinely, so a true edge that survives asmtest_gcmove_-
 * canonicalize survives here unchanged); the no-owner/no-collision branch is
 * exactly increment-2 behavior, so nnodes == 0 degrades to asmtest_gcmove_-
 * canonicalize byte-for-byte. Register and routine-offset records are untouched.
 * Returns the count of records whose addr changed, or (size_t)-1 on a NULL trace.
 * Call it ONCE on a freshly captured trace (it mutates addresses in place). */
size_t asmtest_objid_canonicalize(asmtest_valtrace_t *v,
                                  const asmtest_gcnode_t *nodes, size_t nnodes,
                                  const asmtest_gcmove_t *moves, size_t nmoves);

/* ------------------------------------------------------------------ */
/* Phase 4 (increment 3) — runtime-helper SUMMARY EDGES                */
/*                                                                     */
/* A managed value trace steps THROUGH the CoreCLR runtime helpers the  */
/* JIT emits for language-level operations: an ALLOCATION helper for a   */
/* `new`, a WRITE-BARRIER for a reference-field store, a GENERIC-DICT     */
/* lookup for a shared-generic type/method handle. Those helper bodies    */
/* are ordinary instrumented blocks — a raw def-use built over the trace  */
/* WOULD thread the caller's data flow through the helper's internal      */
/* instructions (scratch regs, card-table math, slow-path branches),      */
/* which is both noisy and CoreCLR-version-specific.                       */
/*                                                                        */
/* This pass instead models a RECOGNIZED helper call as a SUMMARY: at the  */
/* helper's entry step it emits exactly the helper's declared INPUT reads   */
/* and OUTPUT writes (its data-flow contract — e.g. an alloc helper reads    */
/* its MethodTable arg and writes the new object reference to the return     */
/* register) and DROPS the helper body's own records, so a def-use built     */
/* over the trace connects the caller's flow ACROSS the helper (arg def ->    */
/* summary node -> return use) WITHOUT descending into CoreCLR internals.     */
/* An UNRECOGNIZED call is left alone — its body is descended normally, so    */
/* the pass never fabricates a summary edge it cannot justify (conservative). */
/*                                                                            */
/* Helpers are identified by the increment-1 method resolver: each step's PC  */
/* resolves to a method record whose NAME is matched against a helper table   */
/* (exact, or a trailing '*' prefix pattern). A representative built-in table  */
/* of the three canonical helper shapes ships as asmtest_helper_default_table; */
/* the register ids there mirror the x86-64 SysV convention (Capstone reg ids, */
/* kept as literals so this PURE C TU needs no Capstone). The transform reuses  */
/* asmtest_defuse_build over a rewritten record stream, so it stays on the same */
/* pure tier as the L1/L2 passes and unit-tests on every host. The LIVE helper  */
/* identification (a runtime symbolizer feeding the method-map) is a producer   */
/* concern; this is the pure model it drives. Incidental caller-saved clobbers   */
/* are deliberately NOT modelled — only the declared input->output contract.     */
/* ------------------------------------------------------------------ */

/* A helper input/output LOCATION. AT_HELPER_REG names a register directly (the
 * common case: an arg or the return register). AT_HELPER_MEM_AT_REG names the
 * memory the helper touches THROUGH a pointer register — the write-barrier's
 * destination field, whose absolute address is resolved from `reg`'s captured
 * value at the call site (skipped, conservatively, when that value is unknown). */
typedef enum {
    AT_HELPER_REG = 0, /* a register, by reg id                              */
    AT_HELPER_MEM_AT_REG =
        1, /* memory at the address currently held in reg `reg`  */
} at_helper_loc_kind_t;

typedef struct asmtest_helper_loc {
    at_helper_loc_kind_t kind;
    uint32_t reg;  /* REG: the reg id; MEM_AT_REG: the pointer reg          */
    uint16_t size; /* MEM_AT_REG byte width (0 -> 8); ignored for REG        */
} asmtest_helper_loc_t;

/* One known-helper entry: a method NAME (or a trailing-'*' prefix pattern) and
 * its data-flow contract — the input locations that flow to the output
 * locations. `ins` / `outs` are borrowed (the caller's table owns them). */
typedef struct asmtest_helper {
    const char *name;
    const asmtest_helper_loc_t *ins;
    size_t n_in;
    const asmtest_helper_loc_t *outs;
    size_t n_out;
} asmtest_helper_t;

/* Match a resolved method `name` against a helper table: an entry matches on an
 * exact string equality, or — when the entry name ends in '*' — on that prefix.
 * Returns the FIRST matching entry index, or -1 (a NULL name / NULL table never
 * matches). Pure; the table is a small hand-curated list, so a linear scan is
 * both simplest and ample. */
int asmtest_helper_match(const asmtest_helper_t *helpers, size_t nhelpers,
                         const char *name);

/* The built-in representative .NET runtime-helper table: an allocation helper
 * (MethodTable arg -> new object reference in the return reg), a write-barrier
 * (reference value -> the destination field in memory), and a generic-dictionary
 * lookup (context -> resolved handle in the return reg). Register ids mirror the
 * x86-64 SysV convention as Capstone reg ids. Returns a pointer to static storage
 * (never NULL); `*n_out`, when non-NULL, receives the entry count. */
const asmtest_helper_t *asmtest_helper_default_table(size_t *n_out);

/* Build the last-writer def-use graph over `v` (as asmtest_defuse_build does) but
 * SUMMARIZE each recognized runtime-helper call. A step whose PC resolves (via the
 * increment-1 method-map) to a method whose name matches `helpers` begins a helper
 * RUN — the maximal following stretch of steps that resolve to the SAME helper
 * entry. At the run's FIRST step the graph gains the helper's declared input reads
 * and output writes, and every record inside the run is dropped, so the caller's
 * flow links across the helper without the body's internal instructions. An
 * unrecognized call is descended normally (its records pass through unchanged), so
 * no summary edge is invented for it. A MEM_AT_REG output/input resolves its
 * absolute address from the pointer register's most recent captured value; when
 * that value is unknown the memory location is skipped (no false edge).
 *
 * Returns an asmtest_defuse_t (free with asmtest_defuse_free) whose nsteps still
 * spans the whole trace, so the L2 slicer works over it unchanged. A NULL trace
 * returns NULL; a NULL/empty helper table (or a NULL/empty method-map) degrades to
 * a plain asmtest_defuse_build (nothing is summarized). */
asmtest_defuse_t *asmtest_defuse_build_summarized(
    const asmtest_valtrace_t *v, const asmtest_method_t *methods,
    size_t nmethods, const asmtest_helper_t *helpers, size_t nhelpers);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_VALTRACE_H */
