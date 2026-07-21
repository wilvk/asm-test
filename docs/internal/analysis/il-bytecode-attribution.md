# Analysis: can native trace points be mapped to IL / bytecode?

*Status: analysis / findings. This document records a research investigation, not
shipped behaviour. It is the attribution-granularity sequel to the control-flow
tracing story: the shipped tiers record **which native instructions ran** (the
shared [`asmtest_trace_t`](../../../include/asmtest_trace.h) — ordered instruction
offsets + basic blocks), and this note asks the finer question — for a managed or
otherwise higher-level language, can a captured native point be tied back to a
specific **IL offset / bytecode index / source line**, not merely to a method
name? For the capture machinery it leans on see the siblings
[jit-runtime-tracing.md](jit-runtime-tracing.md) (the foreign-JIT / hardware-trace
face), [scoped-inprocess-tracing.md](scoped-inprocess-tracing.md) (the cooperative
`using`-block face) and [data-flow-capture.md](data-flow-capture.md) (the
data-value sibling); the managed-runtime address plumbing is planned in
[scoped-tracing-managed-plan.md](../archive/plans/scoped-tracing-managed-plan.md). Terms
(`JIT`, `jitdump`, `ReadyToRun`, `CoreCLR`, `BCL`) are in the
[glossary](../../project/glossary.md).*

> **Update 2026-07-21 — superseded.** This investigation has since been built:
> see
> [native-il-bytecode-attribution.md](../implementations/native-il-bytecode-attribution.md)
> (T1–T7 landed, commits `25fd351`…`f599777`). The analysis below is preserved
> as written; dated notes mark the determinations that are now false.

## Question

For the managed languages — and for Node/V8 and the other runtimes the project
wraps — is it possible to identify the points in a native trace that correspond to
**IL / bytecode** instructions (or source lines), rather than stopping at the
enclosing method name?

## Determination

**Not today, for any runtime — but the feasibility splits three ways, and it turns
on one property the architecture already forces.** asm-test ships **method-level**
naming for managed code and can map native offsets to **source lines** through one
existing out-of-band hook; it carries **no bytecode/IL-offset representation
anywhere** *(update 2026-07-21: no longer true — the `asmtest_srcmap` schema
shipped in [`include/asmtest_trace.h`](../../../include/asmtest_trace.h)
~:159-243, T3)*. Whether the mapping *can* be built depends on whether the
managed code runs:

- **JIT-compiled** — the native PC *is* the managed machine code, and the runtime
  emits a native-offset → IL/bytecode table you can join. Buildable.
- **interpreted** — the native PC sits in the shared eval loop, and the live
  bytecode index lives in VM register/frame state that a control-flow-only trace
  cannot observe. Not recoverable from the offset stream alone; needs a state probe.

This is the same wall CPython hits, and it is why *"control-flow-complete but no
register/memory state"* is the load-bearing property of the trace record.

## Why it is method-level today

Three facts from the shipped source set the ceiling.

1. **The trace record is pure control-flow.**
   [`asmtest_trace_t`](../../../include/asmtest_trace.h) stores only ordered native
   offsets (`off = ip - base`), de-duplicated basic-block starts, totals, and a
   `truncated` bit — **no registers, no memory, and no code bytes**. Byte recovery
   is a *separate* facility ([`asmtest_codeimage`](../../../include/asmtest_codeimage.h),
   soft-dirty page snapshots); Capstone is an optional annotation layer that
   disassembles caller/codeimage-supplied bytes into instruction text. None of
   these carries a bytecode dimension.

2. **The only generic address → semantics hook is `emu_line_map_t`.** It is an
   ascending `(code byte-offset → 1-based source line)` table —
   [asmtest_trace.h:135-143](../../../include/asmtest_trace.h), explicitly *"the
   shape of a DWARF line program or an assembler listing, produced out-of-band."*
   Its row is `{ uint64 offset; uint32 line; }`: **no IL-offset, no bytecode index
   (bci), no file id, no module/function-offset field.** Expressing true
   bytecode-offset granularity would require a schema extension that exists nowhere
   in `include/` or `src/`.
   *(Update 2026-07-21: that schema extension now exists — `asmtest_srcmap`,
   [`include/asmtest_trace.h`](../../../include/asmtest_trace.h) ~:159-243, T3.)*

3. **The managed name resolvers stop at method identity.** The perf-map parser
   `perfmap_symbol_at` reads only `start size name`
   ([src/hwtrace.c:2289](../../../src/hwtrace.c)); the jitdump reader
   `asmtest_jitdump_find` parses only `JIT_CODE_LOAD` and *skips every other record*
   — including the `JIT_CODE_DEBUG_INFO` records that carry per-address line/IL data
   ([src/ptrace_backend.c:190](../../../src/ptrace_backend.c)).
   *(Update 2026-07-21: no longer skipped — `JIT_CODE_DEBUG_INFO` is parsed at
   [src/ptrace_backend.c:335](../../../src/ptrace_backend.c) and bridged into
   the line map, T1.)* The one explicit
   IL-level statement in the analysis docs is a **negative** one: *"you need the
   JIT's IL-to-native map (rich debug info / PDB)… without it, data flow stays at
   the assembly level"* ([data-flow-capture.md](data-flow-capture.md)).

## The deciding split — compiled vs interpreted

| | Native PC points at… | Bytecode index lives in… | Recoverable from the trace? |
|---|---|---|---|
| **JIT'd code** (TurboFan, C2, RyuJIT, YJIT, V8 Sparkplug, LuaJIT traces) | the managed machine code itself | a runtime-emitted native→IL/bci table | **Yes** — join the table, keyed to the live code version |
| **Interpreted code** (Ignition, HotSpot interp, CPython, PUC-Lua, YARV) | the shared eval loop / opcode handler | a VM register / frame slot (`instr_ptr`, `cfp->pc`, `savedpc`, `kBytecodeOffsetFromFp`) | **No** — not in the PC stream; needs an added state probe |

## Per-runtime matrix

| Runtime | Shipped now | Native→bytecode feasible? | Granularity ceiling |
|---|---|---|---|
| **.NET / CoreCLR** (RyuJIT) | method name + native offset | **Yes** (JIT tiers), high confidence — unbuilt | enclosing IL offset (→ source line via PDB) |
| **JVM / HotSpot** (C1/C2) | method name (already via JVMTI) | **Yes** (JIT), production-proven | bci at debug points, inline-aware |
| **V8 / Node** — Sparkplug tier | method name | **Yes**, dense bidirectional map | per-bytecode-boundary offset |
| **V8 / Node** — TurboFan / Maglev | method name | **Partial** — source pos dense; bci at deopt points | source line/col; bci sparse |
| **V8 / Node** — Ignition interp | method name | **No** (from PC) | needs frame-slot read |
| **WebAssembly** (V8, Wasmtime, Wasmer) | function name | **Yes** — wasm *is* the bytecode | wasm code-section offset (→ src via DWARF) |
| **CPython** (interpreter) | C symbol only | **No** — fundamental | needs `instr_ptr` / `sys.monitoring` |
| **Lua / LuaJIT** | FFI host only | **Partial** — opcode *kind* yes, *index* no | LuaJIT: BCPos at `SNAP` points only |
| **Ruby / YJIT** | nothing wired | **Partial** (YJIT native) | `(ISEQ, insn_idx)` per block |
| **Go / Rust / C / C++ / Zig** | method name + symbols | **N/A** — no bytecode exists | source line via DWARF / pclntab |

*(Update 2026-07-21: the .NET row's "unbuilt" and the JVM row are overtaken —
the .NET `0x20000` listener shipped (T5, `dfd5a85`) and a JVMTI bci agent +
`java-bci` lane shipped (T6, `a998179`); see the banner at top.)*

## Per-runtime findings

### .NET / CoreCLR — the cleanest win

RyuJIT genuinely emits a native-offset → IL-offset table
(`ICorDebugInfo::OffsetMapping`), exposed by three real surfaces, any of which
bridges a native PC to an IL offset:

- the EventPipe/ETW **`MethodILToNativeMap`** event, gated by
  `JittedMethodILToNativeMapKeyword = 0x20000` (payload: `MethodID`, `ReJITID`,
  `CountOfMapEntries`, parallel `ILOffsets[]` / `NativeOffsets[]`), with
  `…DCStart/DCStop` rundown twins for pre-arm methods;
- the profiler API **`ICorProfilerInfo::GetILToNativeMapping`** (and inline-aware
  `GetILToNativeMapping3`), returning `COR_DEBUG_IL_TO_NATIVE_MAP`;
- the debugger API `ICorDebugCode::GetILToNativeMapping`.

Three corrections that fell out of adversarial verification and matter for any
implementation:

- **It is not in the jitdump.** CoreCLR's PAL jitdump writer emits only
  `JIT_CODE_LOAD` (debug-info is a standing TODO), and the perf `JIT_CODE_DEBUG_INFO`
  record is a *source-file/line/column* table anyway, not an IL-offset map. So the
  IL route is the ETW/EventPipe/profiler surface, not the jitdump.
  *(Update 2026-07-21: the reader half of this gap is closed — `JIT_CODE_DEBUG_INFO`
  records are now parsed ([src/ptrace_backend.c:335](../../../src/ptrace_backend.c))
  and bridged to the line map, T1; the point that the .NET IL route is the
  EventPipe surface stands, and that surface shipped too — see below.)*
- **The join is two events, keyed `(MethodID, ReJITID)`.** `MethodILToNativeMap`
  carries no start address; you correlate it to `MethodLoadVerbose_V2` to recover
  `MethodStartAddress`, then `native_off = ip − MethodStartAddress` and binary-search
  `NativeOffsets[]`.
- **`ReJITID` does not distinguish tiers.** Tier0, Tier1 and OSR bodies of one
  method all share `ReJITID = 0` at different addresses, and `MethodILToNativeMap`
  emits no native-code-version id — so `(MethodID, ReJITID)` alone cannot select a
  tier's map. The real version key is the **code range**, which is exactly what the
  `codeimage` temporal recorder already resolves on the native side.

Today the in-proc `EventListener` subscribes only `JITKeyword 0x10`;
[scoped-inprocess-tracing.md](scoped-inprocess-tracing.md) (§ "runtime-events
route") names `0x20000` as a *plan*, not shipped. `NativeAOT` is the exception (no
runtime ICorProfiler IL map; only limited AOT DWARF).
*(Update 2026-07-21: shipped — the in-proc `MethodILToNativeMap` (keyword
`0x20000`) listener landed in
[bindings/dotnet/hwtrace/HwTraceProgram.cs](../../../bindings/dotnet/hwtrace/HwTraceProgram.cs)
~:1530; T5, commit `dfd5a85`.)*

**Granularity ceiling:** enclosing IL offset, not one IL offset per native
instruction — entries are recorded only at IL-instruction / stack-empty / call-site
/ sequence-point boundaries, with pseudo-offsets `NO_MAPPING (−1)`, `PROLOG (−2)`,
`EPILOG (−3)`. Source line/column is one hop further via portable-PDB sequence
points where a PDB exists (BCL / R2R methods often ship none).

### JVM / HotSpot — closer than it looks

The bridge is JVMTI `CompiledMethodLoad`, which hands two things per nmethod:

- a `jvmtiAddrLocationMap` whose `jlocation` **is the bytecode index** on HotSpot
  (`GetJLocationFormat == JVMTI_JLOCATION_JVMBCI`); and
- a `compile_info` chain whose **`JVMTI_CMLR_INLINE_INFO`** record
  (`jvmtiCompiledMethodLoadInlineRecord`, from the HotSpot-specific `jvmticmlr.h` —
  *not* a fictional `JVMTI_KIND_INLINE`) exposes `PCStackInfo: pc → methods[]/bcis[]`,
  i.e. the full inlined `(method, bci)` stack. `bci → line` then comes from
  `GetLineNumberTable` + `GetSourceFileName` + `GetMethodName` /
  `GetMethodDeclaringClass`.

**The key point for asm-test: it already uses `CompiledMethodLoad`** — via the
`libperf-jvmti.so` agent that emits a jitdump ([examples/jit_trace.c](../../../examples/jit_trace.c))
— but consumes only `JIT_CODE_LOAD` (name), *dropping the `JIT_CODE_DEBUG_INFO`
(`addr, line, file`) records it already receives.* That is the cheap interim path:
extend the jitdump parser, get address→source-line with no new agent (at the cost of
flattened inlines and line-not-bci granularity).
*(Update 2026-07-21: both halves have shipped — the jitdump parser now consumes
`JIT_CODE_DEBUG_INFO` ([src/ptrace_backend.c:335](../../../src/ptrace_backend.c),
T1), and a JVMTI bci agent + `java-bci` lane landed for true bci granularity
(T6, commit `a998179`).)*

Caveats (verified): the `AddrLocationMap` is **optional** (may be `NULL`), its
entries are coarse ranges at debug points (safepoint polls, call return addresses,
deopt/exception points) — **not** a per-instruction table — so a native PC's bci is
snapped to the nearest recorded point, not defined between them. The flat map loses
inlining (use the inline record); C2/tier-4 debug info is sparse where C1 is dense;
`jlocation == bci` is HotSpot-specific, not JVMTI-mandated. GraalVM native-image is a
different (DWARF-based, no live JVMTI) path.

### V8 / Node — tier-dependent, and richer than a first pass suggests

Verification upgraded the V8 picture materially:

- **Sparkplug / baseline** Code keeps a dedicated, always-present
  **`BytecodeOffsetTable`** — a bidirectional native-pc ↔ Ignition bytecode-offset
  map at every bytecode boundary (`Code::GetBytecodeOffsetForBaselinePC` /
  `GetBaselinePCForBytecodeOffset`), used for GC/OSR/deopt. This is a genuinely dense
  native→bytecode map.
- **TurboFan / Maglev** Code keeps a `SourcePositionTable`
  (native offset → `SourcePosition{ ScriptOffset, InliningId }`, i.e. source
  line/col via `Script::line_ends`) densely, plus native→bytecode only at deopt-safe
  points via the `DeoptimizationData` translation array.
- **Ignition interpreter**: the offset is held in
  `kInterpreterBytecodeOffsetRegister` (spilled to the `kBytecodeOffsetFromFp` frame
  slot as a tagged Smi). Dispatch is per-opcode threaded handlers, so the native PC
  identifies the opcode *handler* but not the offset — you must read the frame slot.

V8 can emit all of this into a **standard jitdump** via `--perf-prof`
(`JIT_CODE_DEBUG_INFO`), which the current reader drops. Enablement is
**launch-time only** (`--perf-prof` / `--prof`, or Node's V8-flag passthrough); there
is no attach-time enable as there is for .NET/JVM, and the structures are
V8-version-fragile.

### WebAssembly — the "bytecode" *is* wasm

The bytecode layer here is a byte offset into the module code section, and every
engine keeps a native → wasm-offset table: V8 `WasmCode` source positions, Wasmtime
Cranelift `InstructionAddressMap` / `SourceLoc`, Wasmer `FrameInfo`. V8 emits wasm
source positions into a standard jitdump with the public flags
`--perf-prof --perf-prof-annotate-wasm` (needs a build with `enable_profiling=true`)
— no linking against V8 internals. **Wasmtime is the cheapest path** (stable Rust
API; AOT `.cwasm` has no code-move hazard). The wasm-offset → *original* Rust/C++
line is a second, DWARF-in-wasm hop. Optimizing tiers reorder/inline (many-to-one);
Liftoff→TurboFan tier-up reuses addresses, so the version-aware `codeimage` recorder
is required.

### Lua / LuaJIT — partial, and finer than "one C loop"

PUC-Lua runs all bytecode inside `luaV_execute`, but **opcodes are distinguishable**
in a native trace: each opcode is a separate basic block (distinct native PC) and
many dispatch to distinct helpers, so a control-flow trace can tell opcode *kinds*
apart by intra-function address and callee. What it cannot recover is the bytecode
*index* (which instance/iteration) — that lives in the local `pc` (`ci->u.l.savedpc`
in 5.2+, `ci->savedpc` in 5.1), synced to VM state only at call/error/hook points.
LuaJIT trace mcode does carry `BCPos`, but only at `SNAP` snapshot points
(guards/exits/loop headers), per-trace with trace-id + exit context. Upstream LuaJIT
emits neither `/tmp/perf-<pid>.map` nor perf jitdump (only a VTune hook), so the
existing parsers have nothing to consume without a fork/patch. The Lua binding today
uses LuaJIT only as an **FFI host** to trace native fixtures, never as a traced JIT.

### Ruby / YJIT — real bytecode, nothing wired

YARV is real bytecode (`insn_idx` into `iseq_encoded`; the ISEQ line table maps
`insn_idx → line`). Interpreted code keeps the live insn in `cfp->pc` (memory) — not
recoverable from the PC stream. **YJIT** tags each compiled `Block` with
`(ISEQ, start insn_idx)`, so the mapping exists, but it is only exposed via
`--yjit-dump-disasm` (dev/Capstone builds), not a machine-consumable format. The
realistic near-term win is **method naming**: `ruby --yjit --yjit-perf` (3.3+) writes
a standard `/tmp/perf-<pid>.map` the existing perfmap parser could ingest — not yet
wired into the Ruby binding. Code GC / lazy stubs make it a temporal-bytes problem.

### Go / Rust / C / C++ / Zig — a category error, already solved

These compile straight to native; there is **no bytecode/IL layer at runtime**
(LLVM IR, Swift SIL, Zig AIR, Go SSA never ship in the image). The correct analog of
a bytecode offset is a **source line**, and `emu_line_map_t` already accepts exactly
that shape. What is missing is only an auto-populator: a DWARF `.debug_line` reader
(libdw / llvm-symbolizer / addr2line) for C/C++/Rust/Zig/Swift, and a `gopclntab`
reader for Go (present even in stripped binaries). asm-test's own Keystone-assembled
code has no debug info and would need an assembler listing generated at assemble
time. Method naming comes from the ELF symtab / pclntab plus per-language demanglers.

## Cross-cutting caveats

- **Never 1:1.** Even where buildable, the ceiling is the *enclosing* IL offset / bci
  snapped to the nearest recorded debug point — one IL/bytecode op lowers to many
  native instructions, and optimizing tiers (TurboFan, C2, tier1, YJIT block
  specialization, LuaJIT trace inlining, AOT `-O2`) make it many-to-one and need
  inline-table / snapshot walking the flat `emu_line_map_t` cannot represent.
- **Version-match is mandatory.** A native offset only resolves against the code
  version live at capture time (V8 compacting GC, wasm tier-up, YJIT code GC,
  .NET tiering). This is the one hard part already solved on the native side by the
  time-aware `codeimage` recorder.
- **Interpreter state is the wall.** In V8 Ignition, PUC-Lua, Ruby YARV and CPython
  the live bytecode index sits in VM memory/registers, not the native PC, so it is
  unrecoverable from the control-flow stream and needs an added state probe
  (analogous to the planned CPython `instr_ptr` / `f_lasti` read).
- **Statistical HW trace can't do exact bytecode.** IBS/LBR sample skid corrupts the
  bci; exact attribution needs an exact PC stream (`#DB` single-step / ptrace /
  DynamoRIO, or Intel PT / CoreSight ETM). PT/LBR are absent on the Zen 2 dev host.
- **Enablement asymmetry.** .NET/JVM/CPython allow attach-time enable; V8 and Ruby
  feeds are launch-time only.

## What building it would take

Two pieces, small relative to what already exists:

1. **Schema.** Add an `il_offset` / `bci` field (and optionally an inline-frame
   stack) alongside `off = ip − base` in the trace record — the one thing missing
   everywhere — plus a widened line-map row (file id + offset kind) so the same hook
   can carry a bytecode offset, not only a source line.
2. **A per-runtime ingester, keyed to the `codeimage` version timeline.** A captured
   native PC first resolves to the code version live at its timestamp, then to the
   nearest-preceding map entry:
   - **.NET** — subscribe `0x20000`, parse `MethodILToNativeMap` (+ rundown), join via
     `(MethodID, ReJITID)` → `MethodStartAddress`, or attach an `ICorProfiler` shim
     using `GetILToNativeMapping2/3`.
   - **JVM** — capture the `AddrLocationMap` + `JVMTI_CMLR_INLINE_INFO` records already
     delivered by `CompiledMethodLoad`; or, cheapest, decode the jitdump
     `JIT_CODE_DEBUG_INFO` records the reader currently skips.
   - **V8 / Wasmtime** — decode the jitdump debug-info records; add the frame-slot read
     for the Ignition interpreter case.
   - **Go / native** — a DWARF `.debug_line` / `gopclntab` populator for the line map.

The **single cheapest first step** across JVM and V8 is to stop discarding the
`JIT_CODE_DEBUG_INFO` records `asmtest_jitdump_find` already receives — that yields
address → source-line with no new agent and no new capture path.

## Sources

Runtime IL/bytecode-mapping surfaces: [dotnet ILToNativeMap], [dotnet runtime
events], [ICorProfilerInfo], [JVMTI], [jvmticmlr.h], [V8 linux-perf],
[wasmtime debugging], [YJIT]. Repo anchors: [`asmtest_trace_t`](../../../include/asmtest_trace.h),
[`asmtest_codeimage`](../../../include/asmtest_codeimage.h),
[`asmtest_jitdump_find`](../../../src/ptrace_backend.c),
[`perfmap_symbol_at`](../../../src/hwtrace.c).

[dotnet ILToNativeMap]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilerinfo2-getiltonativemapping2-method
[dotnet runtime events]: https://learn.microsoft.com/en-us/dotnet/fundamentals/diagnostics/runtime-method-events
[ICorProfilerInfo]: https://learn.microsoft.com/en-us/dotnet/framework/unmanaged-api/profiling/icorprofilerinfo-getiltonativemapping-method
[JVMTI]: https://docs.oracle.com/en/java/javase/21/docs/specs/jvmti.html#CompiledMethodLoad
[jvmticmlr.h]: https://github.com/openjdk/jdk/blob/master/src/hotspot/share/prims/jvmticmlr.h
[V8 linux-perf]: https://v8.dev/docs/linux-perf
[wasmtime debugging]: https://docs.wasmtime.dev/examples-debugging-native-debugger.html
[YJIT]: https://github.com/ruby/ruby/blob/master/doc/yjit/yjit.md
