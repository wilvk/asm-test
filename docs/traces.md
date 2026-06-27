# Execution traces and coverage

The emulator tier can record what a routine executed while it ran. A traced call
captures two related views:

- an **instruction trace**: every instruction address, in execution order
- **basic-block coverage**: the distinct basic-block entry offsets reached

Both views use byte offsets from the routine entry. Offset `0` is the first byte
of the code passed to `emu_call_traced` or a guest-specific traced call.

Tracing is useful when a return value is not enough. You can check that tests
reach every path you care about, compare a narrow input against a broader
"universe" of blocks, inspect a loop's dynamic behavior, or feed coverage into
the emulator fuzzing helpers.

## Recording a trace

Tracing is opt-in. In C, you provide an `emu_trace_t` and any buffers you want to
fill. Either buffer can be omitted by leaving its pointer `NULL`.

```c
uint64_t insns[256];
uint64_t blocks[64];

emu_trace_t tr = {0};
tr.insns = insns;
tr.insns_cap = 256;
tr.blocks = blocks;
tr.blocks_cap = 64;

emu_result_t r;
long args[] = {7};

ASSERT_TRUE(emu_call_traced(E, (void *)classify, 64, args, 1, 0, &r, &tr));
ASSERT_NO_FAULT(&r);
```

The same pattern works for the cross-architecture guests:

```c
emu_arm64_call_traced(e, code, code_len, args, nargs, max_insns, &r, &tr);
emu_riscv_call_traced(e, code, code_len, args, nargs, max_insns, &r, &tr);
emu_arm_call_traced(e, code, code_len, args, nargs, max_insns, &r, &tr);
emu_call_win64_traced(e, fn, code_len, args, nargs, max_insns, &r, &tr);
```

The dynamic-language FFI surface uses an opaque trace handle instead of exposing
the C struct layout:

```c
emu_trace_t *tr = asmtest_emu_trace_new(256, 64);
asmtest_emu_call6_traced(e, fn, a0, a1, a2, a3, a4, a5, nargs, 0, out, tr);

unsigned long long n = asmtest_emu_trace_blocks_len(tr);
int hit_entry = asmtest_emu_trace_covered(tr, 0);

asmtest_emu_trace_free(tr);
```

## What the fields mean

| Field | Meaning |
|---|---|
| `insns` | Ordered instruction offsets, one entry per executed instruction until `insns_cap` fills |
| `insns_len` | Entries written to `insns` |
| `insns_total` | Total executed instructions, including entries dropped after the buffer fills |
| `blocks` | Distinct basic-block entry offsets |
| `blocks_len` | Distinct block offsets recorded |
| `blocks_total` | Dynamic block entries, so loop re-entry increments this |
| `truncated` | At least one requested buffer filled and dropped later entries |

`blocks` is de-duplicated. If a loop enters the same block 100 times,
`blocks_len` grows once and `blocks_total` grows 100 times.

`insns` is ordered and not de-duplicated. It is the right view for "what happened
step by step"; `blocks` is the right view for "which regions did this input
reach?".

## Accumulating coverage

Tracing appends to an existing `emu_trace_t`. That means you can run many inputs
through one trace to form a coverage union:

```c
uint64_t blocks[64];
emu_trace_t all = {0};
all.blocks = blocks;
all.blocks_cap = 64;

long inputs[] = {-5, 0, 7};
for (int i = 0; i < 3; i++) {
    emu_result_t r;
    long args[] = {inputs[i]};
    emu_call_traced(E, (void *)classify, 64, args, 1, 0, &r, &all);
}

ASSERT_BLOCKS_AT_LEAST(&all, 3);
ASSERT_BLOCK_COVERED(&all, 0);
```

This is the usual way to answer "did this input set reach the expected blocks?".
Use one fresh trace for a single input, and another accumulated trace as the
comparison universe.

```c
size_t missed = emu_coverage_uncovered(&covered, &universe, stdout);
ASSERT_EQ(missed, 0);
```

`emu_trace_report(&tr, stdout)` prints the distinct block count, dynamic block
entries, instruction count, and covered offsets. `emu_trace_lcov(&tr, name, out)`
emits an lcov-style record where block offsets stand in for line numbers.

## Source-line coverage

The emulator records machine-code offsets, not source locations. To report
source-line coverage, provide a normalized line map:

```c
static const emu_line_entry_t rows[] = {
    {0, 10},
    {4, 11},
    {8, 12},
};
emu_line_map_t map = {rows, 3};

emu_trace_source_report(&covered, &map, stdout);
emu_trace_lcov_source(&covered, &map, "classify.s", out);
```

Each row starts at a byte offset and names the source line that owns the range
until the next row. A line counts as covered when a covered basic block begins in
that line's range.

The framework deliberately consumes this table rather than parsing DWARF itself.
You can build the map from an assembler listing, `objdump`, `readelf`, or any
other tool that can normalize debug information into `(offset, line)` rows.

## Disassembly

Trace offsets become easier to read when Capstone is available. The disassembly
helpers annotate recorded offsets with instruction text:

```c
emu_trace_disasm(&tr, EMU_ARCH_X86_64, code, code_len, stdout);
emu_trace_report_disasm(&tr, EMU_ARCH_X86_64, code, code_len, stdout);
emu_coverage_uncovered_disasm(&covered, &universe,
                              EMU_ARCH_X86_64, code, code_len, stdout);
```

Without Capstone, these helpers fall back to the same offset-only reports. See
[Disassembly](disassembly.md) for build flags and examples.

## Limits

Trace coverage is **basic-block coverage**, not edge coverage. It can prove a
block entry was reached; it does not directly prove that every edge between
blocks was taken.

Source-line coverage is only as precise as the line map you provide. Several
blocks can map to one line, and one source statement can generate multiple
blocks.

Traces are per emulator handle and per call sequence. They are not thread-safe
shared collectors. Use a separate trace for each concurrent worker or protect the
handle externally.

The emulator records guest execution, so self-modifying code, indirect branches,
and faults are observed as the virtual CPU sees them. For native hardware timing
or cycle counts, use the benchmark tier instead.

## Related APIs

| Need | API |
|---|---|
| x86-64 System V trace | `emu_call_traced` |
| x86-64 Win64 trace | `emu_call_win64_traced` |
| AArch64 trace | `emu_arm64_call_traced` |
| RISC-V RV64 trace | `emu_riscv_call_traced` |
| ARM32 trace | `emu_arm_call_traced` |
| Dynamic FFI trace handle | `asmtest_emu_trace_new` / `asmtest_emu_trace_free` |
| Check a block | `emu_trace_covered` / `asmtest_emu_trace_covered` |
| Human report | `emu_trace_report` |
| Missed blocks | `emu_coverage_uncovered` |
| Offset-level lcov | `emu_trace_lcov` |
| Source report | `emu_trace_source_report` |
| Source lcov | `emu_trace_lcov_source` |
