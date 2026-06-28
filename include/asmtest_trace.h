/*
 * asmtest_trace.h — engine-neutral execution-trace substrate.
 *
 * The trace model (ordered instruction offsets + distinct basic-block offsets,
 * with totals and a truncation bit) started life inside the Unicorn emulator
 * tier (asmtest_emu.h / src/emu.c). It is the shared *sink* every trace backend
 * fills: the Unicorn hooks, the DynamoRIO drmgr client (asmtest_drtrace.h), and
 * the Intel PT / ARM CoreSight decoders (asmtest_hwtrace.h) are interchangeable
 * *backends* that all record the same `asmtest_trace_t` offsets. The begin/end
 * markers are backend-neutral, so a caller can switch backends without changing
 * test code.
 *
 * This header owns the canonical `asmtest_trace_t` type, the source line-map
 * types, the architecture enum used by the disassembly annotation layer, and the
 * engine-neutral allocate/report/coverage helpers. It has no Unicorn, DynamoRIO,
 * Capstone, or libipt dependency and may be included on its own.
 *
 * Compatibility: asmtest_emu.h #includes this header and re-exports the historical
 * `emu_trace_t` / `emu_arch_t` / `emu_*` names as thin typedef/wrapper aliases, so
 * existing emulator-tier code and the language bindings compile unchanged.
 */
#ifndef ASMTEST_TRACE_H
#define ASMTEST_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Canonical execution-trace record                                    */
/*                                                                     */
/* Tracing is opt-in and APPENDS: zero the struct, point insns/blocks  */
/* at caller-owned buffers (either may stay NULL to skip that          */
/* dimension), then run a traced call. Re-running with the same struct  */
/* accumulates, so the union of basic blocks across many inputs answers */
/* "did the tests reach every block?". Addresses are recorded as byte   */
/* offsets from the start of the registered routine (offset 0 = entry). */
/* ------------------------------------------------------------------ */
typedef struct asmtest_trace {
    /* Ordered instruction trace: each executed instruction's offset, in
     * execution order, up to insns_cap entries. */
    uint64_t *insns;
    size_t insns_cap;
    size_t insns_len;      /* entries written to insns[] (<= insns_cap)      */
    uint64_t insns_total;  /* instructions executed (counts past insns_cap)  */

    /* Basic-block coverage: the DISTINCT block-start offsets entered,
     * de-duplicated, up to blocks_cap entries. */
    uint64_t *blocks;
    size_t blocks_cap;
    size_t blocks_len;     /* distinct blocks recorded (<= blocks_cap)       */
    uint64_t blocks_total; /* block entries; a loop counts each pass         */

    bool truncated;        /* a buffer filled and at least one entry dropped */
} asmtest_trace_t;

/* ------------------------------------------------------------------ */
/* Generic fill points (factored out of the Unicorn callbacks so the   */
/* DynamoRIO and hardware-trace backends share the exact append/dedup/  */
/* truncate semantics). trace_append_block dedups against blocks[] and  */
/* always bumps blocks_total; trace_append_insn records in order and    */
/* always bumps insns_total. A NULL trace is a no-op.                   */
/* ------------------------------------------------------------------ */
void trace_append_insn(asmtest_trace_t *t, uint64_t off);
void trace_append_block(asmtest_trace_t *t, uint64_t off);

/* ------------------------------------------------------------------ */
/* Canonical opaque-handle allocate / free / query (new code path)     */
/*                                                                     */
/* asmtest_trace_new allocates the struct plus the two caller-owned     */
/* buffers in one handle so a dynamic-FFI binding need not lay out the  */
/* struct; instruction recording is active when insns_cap > 0, block    */
/* recording when blocks_cap > 0. asmtest_trace_covered is the form new */
/* code should call (see the compatibility shims below).                */
/* ------------------------------------------------------------------ */
asmtest_trace_t *asmtest_trace_new(size_t insns_cap, size_t blocks_cap);
void asmtest_trace_free(asmtest_trace_t *t);
/* True (1) if basic-block offset `off` is in the distinct block set. Canonical
 * form; returns int to match the FFI accessor convention. */
int asmtest_trace_covered(const asmtest_trace_t *t, uint64_t off);
/* Human-readable coverage summary (distinct blocks, totals, sorted offsets). */
void asmtest_trace_report(const asmtest_trace_t *t, FILE *out);

/* ------------------------------------------------------------------ */
/* Opaque-handle FFI accessors (dynamic-language bindings)             */
/*                                                                     */
/* asmtest_emu_trace_new keeps its historical name for binding ABI     */
/* stability but is backend-neutral (no Unicorn dependency); it is an   */
/* alias of asmtest_trace_new. The readers expose the fields a binding  */
/* cannot lay out by hand.                                              */
/* ------------------------------------------------------------------ */
asmtest_trace_t *asmtest_emu_trace_new(size_t insns_cap, size_t blocks_cap);
void asmtest_emu_trace_free(asmtest_trace_t *t);
unsigned long long asmtest_emu_trace_insns_total(const asmtest_trace_t *t);
unsigned long long asmtest_emu_trace_blocks_len(const asmtest_trace_t *t);
unsigned long long asmtest_emu_trace_blocks_total(const asmtest_trace_t *t);
int asmtest_emu_trace_truncated(const asmtest_trace_t *t);
unsigned long long asmtest_emu_trace_block_at(const asmtest_trace_t *t, size_t i);
/* Compatibility shim: same predicate as asmtest_trace_covered (int return). */
int asmtest_emu_trace_covered(const asmtest_trace_t *t, unsigned long long off);

/* ------------------------------------------------------------------ */
/* Engine-neutral coverage / report helpers (historical emu_* names)   */
/*                                                                     */
/* Kept exported under their existing names so ASSERT_BLOCK_COVERED,    */
/* the bindings, and the ABI manifest are unaffected. emu_trace_covered */
/* (bool) is a compatibility shim for asmtest_trace_covered (int); new  */
/* code should prefer the canonical form.                              */
/* ------------------------------------------------------------------ */
bool emu_trace_covered(const asmtest_trace_t *t, uint64_t off);
void emu_trace_report(const asmtest_trace_t *t, FILE *out);
size_t emu_coverage_uncovered(const asmtest_trace_t *covered,
                              const asmtest_trace_t *universe, FILE *out);
void emu_trace_lcov(const asmtest_trace_t *t, const char *name, FILE *out);

/* ------------------------------------------------------------------ */
/* Source-line coverage mapping                                        */
/*                                                                     */
/* Supply an ascending (offset, line) table — the shape of a DWARF line */
/* program or an assembler listing, produced out-of-band — to report    */
/* coverage against SOURCE LINES instead of raw offsets. Row i covers   */
/* [entries[i].offset, entries[i+1].offset); the last row extends to    */
/* the routine end. A line counts as covered when a covered block begins */
/* within its offset range.                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint64_t offset; /* code byte-offset where this source line begins  */
    uint32_t line;   /* 1-based source line number                      */
} emu_line_entry_t;

typedef struct {
    const emu_line_entry_t *entries; /* ascending by .offset            */
    size_t count;
} emu_line_map_t;

const emu_line_entry_t *emu_line_lookup(const emu_line_map_t *map, uint64_t off);
size_t emu_trace_source_report(const asmtest_trace_t *covered,
                               const emu_line_map_t *map, FILE *out);
void emu_trace_lcov_source(const asmtest_trace_t *covered,
                           const emu_line_map_t *map, const char *source_file,
                           FILE *out);

/* ------------------------------------------------------------------ */
/* Disassembly annotation layer (optional, Capstone)                   */
/*                                                                     */
/* Offsets recorded by any backend are rendered back to instruction     */
/* text from caller-supplied code bytes. The canonical entry points are */
/* backend-neutral by name (asmtest_*) and take an explicit base_addr   */
/* so native / hardware regions at a runtime base resolve PC-relative    */
/* operands correctly; the historical emu_* spellings (asmtest_emu.h)   */
/* default base_addr to EMU_CODE_BASE. Without Capstone every entry     */
/* point degrades to the bare-offset form.                             */
/* ------------------------------------------------------------------ */

/* Guest architecture for the disassembler (mirrors the emulator guest set and
 * asm_arch_t's ordering). emu_arch_t / EMU_ARCH_* are compatibility aliases. */
typedef enum {
    ASMTEST_ARCH_X86_64 = 0,
    ASMTEST_ARCH_ARM64 = 1,
    ASMTEST_ARCH_RISCV64 = 2,
    ASMTEST_ARCH_ARM32 = 3,
} asmtest_arch_t;

/* True iff this build links Capstone, so the helpers produce instruction text
 * rather than degrading to bare offsets. */
bool asmtest_disas_available(void);

/* Disassemble the single instruction at code[off] for `arch`, formatting
 * "<mnemonic> <operands>" into buf (always NUL-terminated). `base_addr` is the
 * address the bytes run at, so PC-relative operands resolve to absolute targets.
 * Returns the instruction byte length, or 0 when Capstone is absent or the bytes
 * do not decode (buf set to "" so the caller can fall back to the offset). */
size_t asmtest_disas(asmtest_arch_t arch, const uint8_t *code, size_t code_len,
                     uint64_t base_addr, uint64_t off, char *buf, size_t buflen);

/* Ordered instruction trace, each entry disassembled (a readable listing). */
void asmtest_trace_disasm(const asmtest_trace_t *t, asmtest_arch_t arch,
                          const uint8_t *code, size_t code_len,
                          uint64_t base_addr, FILE *out);

/* Coverage summary with each listed block offset annotated with its first
 * instruction; falls through to the offset-only form without Capstone/code. */
void asmtest_trace_report_disasm(const asmtest_trace_t *t, asmtest_arch_t arch,
                                 const uint8_t *code, size_t code_len,
                                 uint64_t base_addr, FILE *out);

/* The blocks in `universe` absent from `covered`, annotated; returns the
 * uncovered count (0 = full coverage of the universe). */
size_t asmtest_trace_coverage_uncovered_disasm(const asmtest_trace_t *covered,
                                               const asmtest_trace_t *universe,
                                               asmtest_arch_t arch,
                                               const uint8_t *code,
                                               size_t code_len,
                                               uint64_t base_addr, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* ASMTEST_TRACE_H */
