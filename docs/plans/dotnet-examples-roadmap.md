# asm-test — dotnet examples roadmap: more reportable info + new examples

The six in-process [examples/dotnet](../../examples/dotnet/) demos each report an **aggregate**
(per-method, per-assembly) or the **raw stream**. But `AsmTrace.Addresses` is a full **dynamic**
trace (execution order, *with repeats*), and several binding capabilities have **no example at
all**. This roadmap is the deduped, feasibility-checked output of a design pass over the binding
surface; it separates what can be **reported** (mostly cheap `Report.cs` additions) from new
**example projects**, and records what the single-step tier honestly *cannot* do.

> **The honest currency is instruction counts, not time.** The single-step WEAK tier records
> instruction *offsets in execution order* — no timestamps, no cycles — and amplifies ~1M
> runtime instructions per managed call. Any "time"/"overhead"/"ns" number would be meaningless;
> everything below is counts, coverage, or structure.

## Status

**Built:** `tiers`, `hotspots`, `coverage` (+ `AsmMethod.Tier`), then `callgraph` (dynamic call
tree from the labelled stream) and `ptrace_native` (out-of-process single-step via
`Ptrace.TraceCall` — the foundation for the attach story). The rest are proposed, ranked
best-first by (value × feasibility); the out-of-process headline `ptrace_dotnet` (attach to the
live `jit_dotnet` target) builds directly on `ptrace_native`.

## New info to REPORT (feasible today from already-captured data)

| Report | What it adds | Data source |
|---|---|---|
| **tiers** ✅ | Instructions by **JIT tier** — R2R `[PreJIT]` vs `[Tier0]`/`[MinOptJitted]` vs `[OptimizedTier1]`: how much executed code ran precompiled vs freshly JIT'd. | `AsmMethod.Tier` (new) + `Count`; `RundownEnabled` gates R2R visibility |
| **hotspots** ✅ | Hottest RIPs — dedup the dynamic trace by execution count; a loop body pops out at ~N×. | `Disassembly` grouped by `Address` (or `Addresses` by value) |
| flat-profile | `perf report` parity: self-count + **Overhead %** + cumulative %. | `Methods[].Count` / `LabelledInstructions` |
| amplification | user vs BCL vs native-runtime split + the WEAK-tier amplification factor. | `Addresses.Length − LabelledInstructions`, `Assembly` |
| runtime-gaps | rank the largest `RuntimeBefore` bursts by the method they precede. | `AsmInstruction.RuntimeBefore` + `.Method` |
| footprint / locality | code working-set (distinct 4 KB pages) + jump-distance locality histogram. | `Addresses` arithmetic |
| runtimebuckets | name the ~1M runtime lump by module (`libclrjit`/`libcoreclr`/`[anon JIT]`). | **small helper**: P/Invoke `asmtest_hwtrace_symbolize_bucket` (exported; works post-close on `Addresses`) |

## New EXAMPLE projects

**Feasible today:**
- **coverage** ✅ — basic-block coverage of a branchy native routine, accumulated across inputs
  (the biggest unused capability: `HwTrace.Create(blocks:N)`/`BlockOffsets`/`Covered`/`InsnOffsets`).
  Reports covered/total %, never-covered blocks (lcov shape), and the "missing test case" story.
  Region scope = single-step's sweet spot: deterministic, zero runtime noise, CI-runnable.
- **hotspots** ✅ / **tiers** ✅ — as above, as standalone projects.
- **instructionmix** — mnemonic-class histogram (move/arith/branch/call/SIMD/mem) + control-flow
  density. What *kind* of work runs. Source: `AsmInstruction.Text` first token.
- **callgraph** — reconstruct the call tree/flame graph from the labelled stream: walk
  `Disassembly` with a shadow stack, classify transitions call/return by the prior mnemonic.
  Honest caveat: only labelled instructions, so approximate.
- **perfannotate** — `perf annotate` but instruction-exact: per-instruction execution-count
  histogram of a native region with proportional bars. Source: `HwTrace.InsnOffsets()`.
- **loops** — per-loop trip counts from backedges (repeat counts + parsing branch targets).

**Out-of-process (what in-process single-step structurally cannot do):**
- **ptrace_native** — `Ptrace.TraceCall` forks a child that runs a native leaf while the parent
  single-steps it — no in-process SIGTRAP perturbation. The foundation.
- **ptrace_dotnet** — attach to the live [jit_dotnet](../../examples/dotnet/jit_dotnet/) target and
  trace a real JIT'd method in *another* GC'd, multi-threaded runtime — the scenario in-process
  tracing is forbidden to do. `RunTo`/`TraceAttached`/`ProcPerfmapSymbol` are all bound; the
  example owns attach/detach. Flow proven in C by [examples/jit_trace.c](../../examples/jit_trace.c).
- **descent** / **descent_dotnet** — exact nested call tree with self-vs-inclusive counts via the
  `Descent` call-descent shadow stack (`Ptrace.TraceCallEx`), native then out-of-process managed.
- **codeimage** — one address holding two code bodies over logical time (`CodeImage.BytesAt`);
  a self-patched blob (honest: real tier0→tier1 relocates to a *new* address, so a fixed-region
  code-image does not capture managed tiering).

**Needs a small helper:**
- **classify** — bind `asmtest_disas_is_call/is_branch/is_ret/probe/call_target` (exported next to
  the already-bound `asmtest_disas`) so `callgraph`/`instructionmix` stop string-parsing `Text` and
  can draw a static CFG + direct-call-target map over a region.

## Honestly NOT feasible (for calibration)

- **Wall-clock / cycle timing flame graph** — no timestamps or cycles; ~1M-insn amplification makes
  any "time" meaningless. Counts only.
- **In-process EXACT managed call tree** — `Descent` runs only through the out-of-process ptrace
  stepper; in-process there is no shadow stack and the native runtime is elided between labelled
  runs, so inclusive cost cannot be honestly attributed to a frame. The exact tree is the
  out-of-process `descent` example; in-process, only the approximate `callgraph` reconstruction.
- **N-region `attribute_window`** — needs the LIVE scope handle, which `AsmTrace` frees in
  `Dispose`; the surviving post-close primitive is `symbolize_bucket` (the `runtimebuckets` report),
  and `wholewindow` already tells leaves apart via `CountInRange`.
