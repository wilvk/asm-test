# examples/dotnet — scoped tracing, live

Runnable .NET demos of the scoped-trace facility from the
[zero-config plan](../../docs/internal/plans/scoped-tracing-zeroconfig-plan.md) (§Z0/§Z1),
one project per report. Most run on the portable **in-process single-step** WEAK tier (any
x86-64 Linux, no privilege); others use the **out-of-process** ptrace stepper
(`localscope_oop*`, `crashproof-showdown`, crash-proof, needs ptrace) and the **AMD LBR**
hardware tier (`amdhot`/`amdlbr`/`amd-period-sweep`, needs Zen 3+ + `CAP_PERFMON`;
`amd-snapshot` also needs `CAP_BPF`). Beyond whole-window captures, the set now also covers the
**scoped** forms (`single-method`, `async-stitch`), tracing **workflows** (`perf-triage-drill`,
`trace-diff`, `coverage-guided-fuzz`, `trace-cost-overhead`), and the family's **shape**
(`tier-ladder`, `crashproof-showdown`, `crashproof-survey`). Every example
**self-skips cleanly (exit 0)** where its tier is unavailable, so the whole set runs anywhere.

| Project | Scope form | Shows |
|---|---|---|
| [wholewindow/](wholewindow/) | `using (new AsmTrace())` | zero-config whole-window capture + attributing two native leaves apart from the runtime |
| [region/](region/) | `using (new AsmTrace(code))` | the same scope shape, scoped to one routine → exactly its assembly |
| [methods/](methods/) | `using (new AsmTrace())` | labelling the captured window by **managed method** (§D0.1) — names an arbitrary **cold** method |
| [rundown/](rundown/) | `new AsmTrace(withRundown: true)` | also names **warm + ReadyToRun (R2R) BCL** methods (§D0.2) via an in-process jitdump rundown — dependency-free, no launch knob |
| [localscope/](localscope/) | `new AsmTrace(byMethod: true, withRundown: true)` | a whole block of **inline C# with no `Work()` method** (LINQ, closures, tuples, pattern matching, generics, local functions) — names the block's **own** compiled code (`Program::Main`, the lambda display class), proving the inline block itself is the traced unit |
| [localscope_oop/](localscope_oop/) | `Ptrace.TraceWindowCall` + `AddrChannel` | the **§D3 crash-proof, OUT-OF-PROCESS** whole-window scope — captures a whole window (a frame + published leaf regions) from a forked tracee, so THIS thread is never `EFLAGS.TF`-armed (a ptrace-stop is not gated by the tracee's signal mask, so it survives code the in-process `localscope` is forbidden to step) |
| [localscope_oop_managed/](localscope_oop_managed/) | `AsmTrace.Window(() => {…})` | the same crash-proof OOP scope over a whole block of **MANAGED C#** — a reverse-attached helper single-steps this thread out of band, so the block **survives an in-scope thrown/caught exception** that `localscope` (in-process) must omit. Names the block's own compiled code; deep mid-window JITs are elided (a documented follow-up) |
| [windowhybrid/](windowhybrid/) | `AsmTrace.WindowHybrid(() => {…})` | the **hybrid** whole-window (§E1): compose the two crash-proof forms — pass 1 runs the cheap `WindowHot` AMD-LBR survey to find the hot methods, pass 2 runs the exact `Window` but publishes **ONLY the hot methods' regions**, so the per-instruction stepper is spent on the hot managed slice while the cold remainder is elided (no new native stepper — `WindowHot` + `Window` composed). `Survey` exposes the pass-1 histogram. **Degrades** to a full exact `Window` off Zen/`CAP_PERFMON`, self-skips off ptrace. Runs `body` twice (must be deterministic) |
| [amdhot/](amdhot/) | `using (new AsmTrace(HwBackend.AmdLbr))` | the **statistical AMD-LBR whole-window survey**, as an **inline `using` scope** — AMD LBR samples the branch stack out of band while the managed block runs at **native speed**, bucketing sampled branch-target endpoints into a **hot-method histogram** (`IsStatistical`, `Methods[].Count` = sample weight, deep BCL named). Crash-proof (no `EFLAGS.TF`/SIGTRAP) and near-native, the honest AMD whole-window shape (exact is a hardware dead end). `AsmTrace.WindowHot(Action)` is the delegate sibling. Needs Zen 3+/LBR + `CAP_PERFMON` → self-skips in the plain lane |
| [amdlbr/](amdlbr/) | `HwTrace.Init(AmdLbr)` + `using (new AsmTrace(code))` | the **region** scope on the **AMD LBR hardware tier** — the same `using (new AsmTrace(code))` shape as [region/](region/) but reconstructing a native routine from the 16-deep branch-record stack (out-of-band, near-native) instead of single-step. AMD LBR is region-scoped + sampled, so the capture is retried until a window lands in-region. Needs Zen 3+/LBR + `CAP_PERFMON` |
| [assemblies/](assemblies/) | `new AsmTrace(byMethod: true, withRundown: true)` | groups the labelled window **by declaring assembly**, listing the methods called in each (`AsmMethod.Assembly` / `.ShortName`) |
| [annotated/](annotated/) | `new AsmTrace(byMethod: true, withRundown: true)` | an **annotated execution trace** — each executed instruction next to the method it ran in (`AsmTrace.Disassembly` / `AsmInstruction`) |
| [tiers/](tiers/) | `new AsmTrace(byMethod: true, withRundown: true)` | executed instructions grouped **by JIT tier** — R2R vs cold/hot JIT (`AsmMethod.Tier`) |
| [hotspots/](hotspots/) | `new AsmTrace(byMethod: true)` | the **most-executed instructions** — a loop pops out of the dynamic trace (dedup `Disassembly` by `Address`) |
| [coverage/](coverage/) | `HwTrace.Create(blocks: N)` + `Region` | **basic-block coverage** of a branchy native routine across inputs — the never-covered block is the missing test case |
| [callgraph/](callgraph/) | `new AsmTrace(byMethod: true, withRundown: true)` | reconstruct the dynamic **call tree + edges** from the labelled stream (shadow stack over `Disassembly`) |
| [ptrace_native/](ptrace_native/) | `Ptrace.TraceCall(...)` | single-step native code running **out of process** (a forked `PTRACE_TRACEME` child) — no in-process SIGTRAP |
| [blockstep/](blockstep/) | `Ptrace.TraceCallBlockstep(...)` | **BTF block-step** (`PTRACE_SINGLEBLOCK`) — the SAME exact trace at ~1 stop per taken branch instead of per instruction; the only exact real-CPU capture on Zen 2 |
| [ptrace_dotnet/](ptrace_dotnet/) | `Ptrace` attach to `jit_dotnet` | **attach to a live CoreCLR** and single-step a real JIT'd method in *another* process — what in-process single-step cannot do |
| [flatprofile/](flatprofile/) | `new AsmTrace(byMethod: true, withRundown: true)` | **`perf report` parity** — methods by self instruction count with Overhead % + cumulative % (`Methods[].Count`) |
| [amplification/](amplification/) | `new AsmTrace(byMethod: true, withRundown: true)` | the window split **user vs BCL vs native runtime** + the WEAK-tier amplification factor (`Addresses.Length` − `LabelledInstructions`) |
| [runtimegaps/](runtimegaps/) | `new AsmTrace(byMethod: true, withRundown: true)` | the largest **native-runtime bursts**, ranked by the method they precede (`AsmInstruction.RuntimeBefore`) |
| [footprint/](footprint/) | `using (new AsmTrace())` | code **working-set** (distinct 4 KB pages) + a **jump-distance locality** histogram (`Addresses` arithmetic) |
| [runtimebuckets/](runtimebuckets/) | `using (new AsmTrace())` | name the ~1M runtime lump **by module** (`HwTrace.SymbolizeBuckets` → `asmtest_hwtrace_symbolize_bucket`, page-deduped) |
| [instructionmix/](instructionmix/) | `new AsmTrace(byMethod: true, withRundown: true)` | the **mnemonic-class histogram** + control-flow density — what *kind* of work runs (`AsmInstruction.Text` + `Disas`) |
| [perfannotate/](perfannotate/) | `HwTrace.Region` | **`perf annotate`, instruction-exact** — per-instruction execution counts of a native region with heat bars (`InsnOffsets()`) |
| [loops/](loops/) | `HwTrace.Region` | **per-loop trip counts** from backedges (backward transitions in the `InsnOffsets()` stream) — a nested loop shows two |
| [descent/](descent/) | `Ptrace.TraceCallEx` + `Descent` | the **exact nested call tree** (self-vs-inclusive counts) via the call-descent shadow stack — a native A→B→C blob |
| [descent_dotnet/](descent_dotnet/) | `Ptrace.TraceAttachedEx` + `Descent` | call descent against a **live CoreCLR** — attach to `jit_dotnet`'s `chain` mode and step INTO `Program::Leaf` as nested frames |
| [codeimage/](codeimage/) | `CodeImage.Track` / `BytesAt` | **one address, two code bodies** over logical time — a self-patched blob the timeline keeps both versions of |
| [single-method/](single-method/) | `AsmTrace.Method(del).Invoke` | trace **exactly one managed method body** — lazy-arms `EFLAGS.TF` *inside* the call, so only that JIT'd body is stepped (zero whole-window runtime amplification); the managed counterpart to [region/](region/), and the SAFE single-step posture (+ its crash-proof `outOfProcess: true` variant) |
| [perf-triage-drill/](perf-triage-drill/) | survey → `AsmTrace.Method` | the two-phase profiler workflow — a whole-window **by-method survey** finds the hottest managed method, then **drills** into exactly that body. The motivated framing of `single-method` |
| [concurrent-isolation/](concurrent-isolation/) | two threads × `AsmTrace.Method` | **two overlapping single-step windows at once** — each thread keeps its own non-empty slice, neither truncates the other (the per-thread range stack; the affirmative proof behind the cross-thread `Truncated` guard) |
| [async-stitch/](async-stitch/) | `AsmStitchedTrace.Step` | follow **one logical operation across an `await`/`Task.Run` hop** and stitch each hop's capture into one seq-ordered trace — the first LIVE producer of the shipped stitch-merge core (a pool-thread hop merges by `Seq`) |
| [trace-diff/](trace-diff/) | two `HwTrace` regions + set-diff | an **exact coverage DELTA between two runs** — which basic blocks a change turned ON/OFF + per-instruction count deltas. Distinct from `coverage` (a union) and `codeimage` (two bodies at one address) |
| [coverage-guided-fuzz/](coverage-guided-fuzz/) | region single-step per input | the **per-input marginal coverage delta** driving an AFL keep/discard decision — watch the corpus grow until the input that reaches the rare guarded block is flagged |
| [trace-cost-overhead/](trace-cost-overhead/) | Stopwatch × every tier | the **honest cost of each tier** — the same native loop untraced vs single-step vs block-step vs OOP ptrace vs AMD LBR, as a slowdown-multiplier + stop-count table |
| [descend-all/](descend-all/) | `Descent(DescendAll)` + guardrails | **auto-descend into UNKNOWN callees**, bounded by `SetMaxDepth`/`SetInsnBudget`/`SetWatchdogMs`/denylist, reporting honest `DepthCapped()`/`Truncated()` — the inverse of [descent/](descent/)'s curated allow-set |
| [crashproof-showdown/](crashproof-showdown/) | in-proc child vs `AsmTrace.Window` | the FATAL boundary **as an observed fact** — the same thread-spawning block force-kills an in-process single-step child (exit 133 = SIGTRAP) yet is captured clean out of band |
| [crashproof-survey/](crashproof-survey/) | Method / Window / WindowHot | one workload through all three crash-proof-capable forms side by side — a **fidelity / cost / safety table** (exact-body vs exact-window vs sampled-AMD) |
| [tier-ladder/](tier-ladder/) | `HwTrace.ResolveTiers` / `AutoTier` | this host's **honest degradation ladder** with a reason per rung, from the public cascade API — and how `CeilingFree`/`NativeOnly` policies drop rungs. Pure enumeration, no tracing |
| [amd-period-sweep/](amd-period-sweep/) | `AsmTrace.WindowHot(period:)` | the AMD-LBR statistical survey **swept across sample periods** — finer (more PMIs/throttle) vs coarser (cheaper) on the same hot block. Needs Zen 3+/LBR + `CAP_PERFMON` |
| [amd-snapshot/](amd-snapshot/) | `AmdSnapshot.Trace` | the **deterministic boundary LBR snapshot** — EXACT capture of a tiny single-shot routine that the sampled survey truncates, via a HW breakpoint at the region exit + `bpf_get_branch_snapshot`. Needs `CAP_BPF` + `CAP_PERFMON` + a BPF-toolchain build → self-skips otherwise |

See the [dotnet examples roadmap](../../docs/internal/archive/plans/dotnet-examples-roadmap.md) for the full design
pass behind these and what the single-step tier honestly cannot do.

(The sibling [jit_dotnet/](jit_dotnet/) is **not** a scope demo: it is a bare CoreCLR
workload traced *out of process* by the C `jit_trace` harness — driven by
`make hwtrace-jit-dotnet` and friends, not by the example lane below.)

## Run them

```sh
make hwtrace-dotnet-example          # runs all of them, in a plain container / on this host
make docker-hwtrace-dotnet-example   # runs all of them, in the asmtest-dotnet image

# or one at a time:
dotnet run --project examples/dotnet/wholewindow/wholewindow.csproj
dotnet run --project examples/dotnet/region/region.csproj
dotnet run --project examples/dotnet/methods/methods.csproj
dotnet run --project examples/dotnet/rundown/rundown.csproj
dotnet run --project examples/dotnet/assemblies/assemblies.csproj
dotnet run --project examples/dotnet/annotated/annotated.csproj
dotnet run --project examples/dotnet/tiers/tiers.csproj
dotnet run --project examples/dotnet/hotspots/hotspots.csproj
dotnet run --project examples/dotnet/coverage/coverage.csproj
dotnet run --project examples/dotnet/callgraph/callgraph.csproj
dotnet run --project examples/dotnet/ptrace_native/ptrace_native.csproj
dotnet run --project examples/dotnet/blockstep/blockstep.csproj
dotnet run --project examples/dotnet/ptrace_dotnet/ptrace_dotnet.csproj
dotnet run --project examples/dotnet/flatprofile/flatprofile.csproj
dotnet run --project examples/dotnet/amplification/amplification.csproj
dotnet run --project examples/dotnet/runtimegaps/runtimegaps.csproj
dotnet run --project examples/dotnet/footprint/footprint.csproj
dotnet run --project examples/dotnet/runtimebuckets/runtimebuckets.csproj
dotnet run --project examples/dotnet/instructionmix/instructionmix.csproj
dotnet run --project examples/dotnet/perfannotate/perfannotate.csproj
dotnet run --project examples/dotnet/loops/loops.csproj
dotnet run --project examples/dotnet/descent/descent.csproj
dotnet run --project examples/dotnet/descent_dotnet/descent_dotnet.csproj
dotnet run --project examples/dotnet/codeimage/codeimage.csproj
dotnet run --project examples/dotnet/localscope/localscope.csproj
dotnet run --project examples/dotnet/localscope_oop/localscope_oop.csproj
dotnet run --project examples/dotnet/localscope_oop_managed/localscope_oop_managed.csproj
dotnet run --project examples/dotnet/windowhybrid/windowhybrid.csproj  # hybrid: AMD survey -> hot-slice exact; degrades in the plain lane
dotnet run --project examples/dotnet/amdhot/amdhot.csproj          # AMD LBR: needs Zen 3+ + CAP_PERFMON
dotnet run --project examples/dotnet/amdlbr/amdlbr.csproj          # AMD LBR: needs Zen 3+ + CAP_PERFMON
dotnet run --project examples/dotnet/single-method/single-method.csproj
dotnet run --project examples/dotnet/perf-triage-drill/perf-triage-drill.csproj
dotnet run --project examples/dotnet/concurrent-isolation/concurrent-isolation.csproj
dotnet run --project examples/dotnet/async-stitch/async-stitch.csproj
dotnet run --project examples/dotnet/trace-diff/trace-diff.csproj
dotnet run --project examples/dotnet/coverage-guided-fuzz/coverage-guided-fuzz.csproj
dotnet run --project examples/dotnet/trace-cost-overhead/trace-cost-overhead.csproj
dotnet run --project examples/dotnet/descend-all/descend-all.csproj
dotnet run --project examples/dotnet/crashproof-showdown/crashproof-showdown.csproj
dotnet run --project examples/dotnet/crashproof-survey/crashproof-survey.csproj
dotnet run --project examples/dotnet/tier-ladder/tier-ladder.csproj
dotnet run --project examples/dotnet/amd-period-sweep/amd-period-sweep.csproj    # AMD LBR: needs Zen 3+ + CAP_PERFMON
dotnet run --project examples/dotnet/amd-snapshot/amd-snapshot.csproj            # AMD snapshot: needs CAP_BPF + CAP_PERFMON
```

To iterate — an **interactive shell** in the `asmtest-dotnet` container with the working
tree live-mounted at `/src` (edit on the host, build + run inside), the shared lib built,
and the resolver env set:

```sh
make dev-dotnet
# then, inside the container:
#   dotnet run --project examples/dotnet/methods/methods.csproj
```

Each project is dependency-free — it compiles the binding source
([HwTrace.cs](../../bindings/dotnet/hwtrace/HwTrace.cs)) directly and P/Invokes
`libasmtest_hwtrace` (auto-resolved from the in-tree `build/`). Each is split in two:
`Program.cs` holds the setup, workload, and the `using (new AsmTrace(...))` scope, and
`Report.cs` (a `static Report.Print(...)`) holds the presentation — reading the closed
scope's data and printing it, with no tracing logic.

## wholewindow — `using (new AsmTrace())` (observed output)

The empty ctor captures *everything* the thread ran, into a large **sparse** buffer
(no fixed 64k cap). The demo calls two native leaves in one scope and **attributes**
each captured address to its origin by range:

```
armed 'Main:57', add2=42, sub2=18, captured 1048576 instructions (truncated).
attribution of the captured window, by origin:
    leaf A  (add2)         : 5      <- mov,add,cmp,jle,ret
    leaf B  (sub2)         : 3      <- mov,sub,ret
    runtime (.NET / other) : 1048568
-> multiple leaves are told apart from each other and from the runtime.
```

The ~1M runtime instructions are the .NET runtime's own JIT'd code — single-step of
a managed caller amplifies enormously, which is why the leaves sit past the old 64k
window and the buffer had to grow (and is honestly `truncated`). The same separation
is proven deterministically, with no runtime noise, by the C host test
`test_wholewindow_buckets` (two native leaves → `leafA: 5`, `leafB: 5` as separate
named buckets via `asmtest_hwtrace_attribute_window`). The clean managed path is the
**STRONG** whole-window PT tier (region-filtered at decode), forward-look here.

## region — `using (new AsmTrace(code))` (observed output)

The same facility, scoped to a native region, yields *exactly* its instructions:

```
armed 'Main:49', add2(20,22) = 42.
rendered listing — EXACTLY the routine's executed instructions:
    0:	mov rax, rdi
    3:	add rax, rsi
    6:	cmp rax, 0x64
    c:	jle 0x...
    11:	ret
truncated: False
```

## methods — labelling the window by managed method (observed output)

The closest thing to the fully aspirational snippet — `using (new AsmTrace(byMethod:
true)) { ColdPath(data); }` over an **arbitrary, cold** managed method — that runs
without Intel PT. Pass `byMethod: true` and the scope itself owns an in-process
`JitMethodMap` (§D0.1, CoreCLR `MethodLoadVerbose`) and, on close, exposes the
per-method breakdown as data — `ww.Methods`, `ww.LabelledInstructions`,
`ww.InstructionsIn("ColdPath")` — so the demo has no map lifecycle or classification
loop of its own:

```
ColdPath(7,100) = 35350; captured 973472 instructions (truncated); 84 methods observed;
  1784 labelled by method.

managed methods that executed in the window (by instruction count):
        1733  Program.ColdPath          <- the arbitrary cold method, by name
          26  dynamicClass.IL_STUB_PInvoke
-> the arbitrary COLD method 'ColdPath' is identified BY NAME: 1733 instructions.
(the native runtime — RyuJIT, GC, PAL — is the unlabelled remainder: 971688 instructions.)
```

Honest limits: this is the single-step WEAK tier, so it also single-steps the JIT
compiling the cold method (the ~977k "native runtime" instructions) — intrusive and
slow. It needs no launch knob and no Intel PT, but the non-intrusive, clean path is
the **STRONG** whole-window PT tier (forward-look here). See
[docs/internal/plans/scoped-tracing-zeroconfig-plan.md](../../docs/internal/plans/scoped-tracing-zeroconfig-plan.md)
§Z3 and the managed plan's §D0.1.

## rundown — naming WARM + R2R BCL methods too (§D0.2, observed output)

The §D0.1 `JitMethodMap` only names methods JIT'd *inside* the scope, so it misses both
warm methods (JIT'd before the scope) and — the bigger set — the **ReadyToRun (R2R)** BCL,
which is AOT-precompiled and never JIT'd at all. `withRundown: true` asks the runtime — over
its own diagnostics socket, **no NuGet, no `DOTNET_PerfMapEnabled`** — for a
`PerfMapType.JitDump` rundown, which (unlike the text perf-map, which is JIT-only) runs the
R2R rundown into `/tmp/jit-<pid>.dump`. `AsmTrace` folds that in, so the whole R2R Console
write path gets named:

```
rundown enabled: True; captured 973328 instructions (truncated); 38 methods labelled (30 R2R); 995 instructions.
    ...
    105  System.IO.StreamWriter::WriteLine(string)[PreJIT]        <- R2R BCL
     65  System.ConsolePal::Write(SafeFileHandle,...)[PreJIT]     <- R2R BCL
     20  Program::Main()[MinOptJitted]                            <- warm (JIT'd)
     11  Program.Work                                              <- cold (listener)
-> the R2R BCL Console write path is NAMED (StreamWriter::WriteLine + ConsolePal::Write,
   both [PreJIT]=ReadyToRun) — precompiled methods the cold-only §D0.1 path can never see.
```

`[PreJIT]` marks R2R (precompiled) methods; `[MinOptJitted]`/`[OptimizedTier1]` are JIT
tiers. (`Console::WriteLine(string)` itself is an inlined forwarder, so its *work* shows up
as these R2R callees.) Self-skips to the cold-only result where diagnostics are off
(`DOTNET_EnableDiagnostics=0` → `rundown enabled: False`). See
[dotnet-perfmap-rundown-plan.md](../../docs/internal/archive/plans/dotnet-perfmap-rundown-plan.md).

## assemblies — grouping the window by declaring assembly (observed output)

Same rundown capture, re-cut **by assembly** instead of a flat method list: `AsmMethod.Assembly`
and `AsmMethod.ShortName` split each labelled name (`<ret> [<assembly>] Type::Method(sig)[tier]`)
into its module and its bare `Type::Method(sig)`, so the demo just groups `ww.Methods` by
`m.Assembly` and prints the methods under each. A one-line `Work()` (a `string.Join` + a
`Console.WriteLine`) already spans three assemblies:

```
rundown enabled: True; 51 methods across 4 assemblies (1869 instructions labelled).

  [System.Console]  — 10 method(s), 256 instructions
        65  System.ConsolePal::Write(SafeFileHandle,ReadOnlySpan`1<uint8>,bool)
        16  System.Console::WriteLine(string)
        ...
  [System.Private.CoreLib]  — 32 method(s), 1186 instructions
       184  System.String::JoinCore(ReadOnlySpan`1<char>,ReadOnlySpan`1<string>)
       105  System.IO.StreamWriter::WriteLine(string)
        46  System.Buffers.SharedArrayPool`1[System.Char]::Rent(int32)
        ...
  [(in-scope / no assembly tag)]  — 7 method(s), 385 instructions
       224  System.Buffer.Memmove          <- cold, listener-spelled (no assembly tag)
        78  Program.Work
```

The parse anchors the assembly on the `] ` immediately before `::` (not the first `[`), so a
bracketed return type (`instance !0[] [System.Private.CoreLib] …`, an array return) or a
generic type right before `::` (`…[System.__Canon]::FromManaged`) is grouped by its real
assembly, not a stray bracket. Listener-spelled cold names (`§D0.1`, dotted `Type.Method`)
carry no tag and fall into the `(in-scope / no assembly tag)` bucket honestly.

## annotated — each instruction next to its method (observed output)

The other examples aggregate the window (per method, per assembly). This one keeps it
per-instruction: `AsmTrace.Disassembly` is the **labelled execution stream** — every captured
instruction that resolved to a managed method, disassembled from live memory (on close, while
the code is still mapped) and paired with its method (`AsmInstruction.Text` / `.Method` /
`.ShortMethod` / `.RuntimeBefore`). The demo prints the **complete** stream — all ~1300
instructions, in execution order, at full width, **no truncation** (no per-run cap, no row
cap, no name clipping). You see the whole control flow end to end: the scope arming, then
`Work` → `string.Join` (→ `CastHelpers::StelemRef` ×3, `JoinCore`, `Buffer.Memmove` ×3) →
**the full `Console.WriteLine` descent**:

```
  address         instruction                                   method
  --------------  --------------------------------------------  --------------------------------
                  ... 4 native-runtime insns ...
  0x…f7d40        push rbp                                      System.Console::WriteLine(string)   <-- HERE
  0x…f7d4b        call qword ptr [rip + 0x3dcf7]                System.Console::WriteLine(string)
  0x…f6db0        push rax                                      System.Console::get_Out()
  0x…9210         push rbp                                      System.IO.TextWriter+SyncTextWriter::WriteLine(string)
  0x…0270         push rbp                                      System.IO.StreamWriter::WriteLine(string)
  0x…f8d0         push rbp                                      System.IO.StreamWriter::Flush(bool,bool)
  0x…cf20         push rax                                      System.Text.UTF8Encoding+UTF8EncodingSealed::GetMaxByteCount(int32)
  0x…af0          push rbp                                      System.Text.Encoder::GetBytes(...)   <- transcode → write()
```

The only rows NOT shown are the unlabelled native-runtime instructions between named runs
(RyuJIT/GC/PAL) — they are not in `Disassembly` to disassemble, so their count is noted
(`... N native-runtime insns ...`) instead. `Console.WriteLine` sits ~380 instructions deep
(past `string.Join`'s own BCL descent), which is why the aggregated examples don't surface it.
`Console::WriteLine(string)` is a thin forwarder; its actual work is the callees shown
(`StreamWriter::WriteLine` → `Flush` → the UTF-8 encode
that feeds the `write()` syscall).

Because this is the single-step WEAK tier, the labelled stream is genuine dynamic execution
(loops repeat, calls descend). Disassembly is a **live** decode of self-memory — correct for
the just-captured window; a long-lived process that recompiles/relocates a method after the
scope would want the code-image `render_versioned` path (§Z3, forward-look). Self-skips (exit 0)
where single-step or Capstone is unavailable.

## tiers — executed instructions by JIT tier (observed output)

The method names carry a JIT-tier tag (`[PreJIT]`=ReadyToRun, `[MinOptJitted]`/`[Tier0]`=cold JIT,
`[OptimizedTier1]`=hot), exposed as `AsmMethod.Tier`. Summing `Count` by tier answers a .NET
question the per-method/per-assembly views don't — how much executed code ran precompiled vs
freshly JIT'd:

```
rundown enabled: True; 1300 labelled instructions across 41 methods, 4 tiers.

  tier                                   methods       insns   share
  ----------------------------------------------------------------
  PreJIT — R2R (precompiled BCL)              32        1012   77.8%
  (untagged) — cold, listener-observed         6         198   15.2%
  OptimizedTier1 — hot JIT                     1          48    3.7%
  MinOptJitted — cold JIT                      2          42    3.2%
```

Most of a one-line `Work()` runs precompiled R2R BCL. `RundownEnabled` gates whether `[PreJIT]`
methods are present at all (reported for honesty).

## hotspots — the most-executed instructions (observed output)

`Addresses`/`Disassembly` are a *dynamic* trace (with repeats), so dedup by address → execution
count. A loop body pops out at ~N×; the per-method examples aggregate that away:

```
3885 labelled instruction executions over 97 distinct instructions (40.1x average reuse — the loop).

     count  heat                  instruction   [method]
       201  ####################  0x…db86 jne 0x…db44               [Program.HotLoop]   <- loop back-edge
       200  ####################  0x…db54 sar rcx, 1                 [Program.HotLoop]
       200  ####################  0x…db57 xor rax, rcx               [Program.HotLoop]
       ...
```

## coverage — basic-block coverage of a native routine (observed output)

The biggest unused capability: no other example touches `HwTrace.Create(blocks:N)` /
`BlockOffsets` / `Covered`. Run a branchy native routine over several inputs (a fresh trace each,
unioned in managed), then compare a "test suite" of inputs against the full reachable block set —
region scope makes it deterministic and CI-runnable with zero runtime noise:

```
classify(-5)=1, classify(50)=2, classify(200)=3  (expect 1, 2, 3)

test inputs {50,200} cover 5 of 6 reachable basic blocks (83%):

  block    covered  entry instruction
  0x00       yes    cmp rdi, 0
  0x06       yes    cmp rdi, 0x64
  0x0c       yes    mov rax, 3
  0x15       yes    mov rax, 2
  0x1e       NO     mov rax, 1
  0x25       yes    ret

-> block 0x1e (mov rax, 1) is NEVER covered by the positive-only tests —
   the missing test case is a NEGATIVE input (x < 0). This is exact, not sampled.
```

## callgraph — dynamic call tree from the labelled stream (observed output)

The per-method/per-assembly views flatten the call *structure*. Walk `ww.Disassembly` with a
shadow stack — a method change right after a `call` is an edge; a `ret` pops — to reconstruct it
(approximate: a call reaching a method through a native runtime stub shows as a `RuntimeBefore`
gap). One `Work()` (a `string.Join` + `Console.WriteLine`) reaches depth 13:

```
call tree (first entry into each method, indented by depth):
  Program.Work
    System.Runtime.CompilerServices.CastHelpers::StelemRef(...)
    System.String::Join(string,string[])
      System.String::JoinCore(...)
        System.Buffer.Memmove
    System.Console::WriteLine(string)
      System.IO.TextWriter+SyncTextWriter::WriteLine(string)
        System.IO.StreamWriter::WriteLine(string)
          System.IO.StreamWriter::Flush(bool,bool)
            System.Text.Encoder::GetBytes(...)  ->  ... -> System.Text.Ascii::NarrowUtf16ToAscii(...)
          System.ConsolePal::Write(...)  ->  Interop+Sys::Write(...)  (the write syscall)

top call edges (by frequency):
     3x  Program.Work  ->  ...CastHelpers::StelemRef(...)      (one per array element)
     3x  System.String::JoinCore(...)  ->  System.Buffer.Memmove
```

The exact, noise-free tree (with self-vs-inclusive counts) is the out-of-process descent example
(forward-look); this is the honest in-process reconstruction.

## ptrace_native — out-of-process single-step (observed output)

Every `AsmTrace` example arms the *calling* thread. `Ptrace.TraceCall` instead forks a
`PTRACE_TRACEME` child that runs the code while the parent single-steps it — so nothing in this
process is ever armed with `EFLAGS.TF`. Self-contained (no external process, no perf-map),
CI-runnable, and the foundation the attach story builds on:

```
child computed sum(3,5) = 15 (expect 15); the tracer single-stepped 17 instructions across the process boundary.

executed instructions (disassembled from the child's code image), with step count:
    0x00  x1    mov rax, 0
    0x07  x5    add rax, rdi        <- loop body, 5 iterations
    0x0a  x5    dec rsi
    0x0d  x5    jne 0x…07
    0x0f  x1    ret
```

Self-skips (exit 0) where ptrace is denied (yama `ptrace_scope`, no privilege).

## ptrace_dotnet — attach to a live CoreCLR and trace a real JIT method (observed output)

The headline: single-step a method in *another*, live, GC'd, multi-threaded .NET runtime — the
exact scenario in-process single-step is forbidden to do (the managed-I/O footgun). It launches
the [jit_dotnet/](jit_dotnet/) target (spinning on `Program::Add`, `DOTNET_TieredCompilation=0`
`DOTNET_PerfMapEnabled=1`), resolves `Add` from its perf-map, `PTRACE_ATTACH`es, `RunTo`s the
entry, and single-steps one real invocation:

```
resolved 'int32 [jit_dotnet] Program::Add(int32,int32)[Optimized]'
  @ 0x…1a30 (4 bytes) in live pid 181 (in an executable mapping).

single-stepped 2 instructions of the REAL JIT'd Program::Add in the other process
(it returned 100144877); entry covered: True.

executed instructions (disassembled from the runtime's live code image):
    0x00  x1   lea eax, [rdi + rsi]      <- the optimized a + b
    0x03  x1   ret
```

The child is launched with libc `posix_spawn` and reaped with raw `waitpid`, **not**
`System.Diagnostics.Process` — .NET's `Process` installs a SIGCHLD reaper that races the ptrace
`waitpid` and hangs the attach. All ptrace calls run on one dedicated OS thread, kill-bounded by a
watchdog, so a moved/re-tiered address self-skips rather than hangs. Self-skips (exit 0) where
ptrace is denied or the SDK/method is absent.

## More reports & examples (dotnet-examples-roadmap)

These extend the same substrate — one project per report, `Program.cs` (setup + scope) +
`Report.cs` (presentation) — landing the roadmap's remaining reports and example projects.

### flatprofile — `perf report` parity (observed output)

`Methods` is already sorted by self count; re-cut as the classic profiler table with an Overhead %
and a running cumulative %:

```
rundown enabled: True; 1418 labelled instructions across 41 methods.

      self   overhead     cumul  method
       235     16.57%     16.6%  System.String::JoinCore(...)
       130      9.17%     25.7%  System.Buffer.Memmove
       105      7.40%     33.1%  System.IO.StreamWriter::WriteLine(string)
       ...
        44      3.10%     64.0%  Program.Work
```

Counts only — the single-step WEAK tier has no timestamps, so there is deliberately no "time" column.

### amplification — user vs BCL vs native runtime (observed output)

A one-line `Work()` single-steps ~1M runtime instructions. This measures that: the whole window
(`Addresses.Length`) vs the managed slice (`LabelledInstructions`) vs the native-runtime remainder,
then splits the managed slice by `AsmMethod.Assembly`:

```
rundown enabled: True; captured 966497 instructions in the window (truncated) for ONE managed Work() call.

  bucket                           insns    share
  user code (Program.*)              133    0.01%
  BCL (System.*, labelled)          1177    0.12%
  native runtime (unlabelled)      965187   99.86%
  total window                    966497  100.00%

amplification factor: 738x — 966497 captured instructions per 1310 labelled managed one
```

This is *why* the managed single-step tier is the WEAK tier; the clean managed path is STRONG
whole-window PT (forward-look).

### runtimegaps — the largest native-runtime bursts (observed output)

The runtime instructions between labelled runs are elided, but their count rides on the next
labelled instruction as `RuntimeBefore`. Ranked, and aggregated by the method each burst precedes:

```
1310 labelled instructions; 678136 native-runtime instructions ran between them.

largest single runtime bursts (runtime insns, then the method they precede):
      339839  Program.Work  (push rbp)
      143924  Asmtest.AsmTrace.set_Armed  (push rbp)
       25571  System.String::JoinCore(...)  (push rbp)
```

The biggest bursts are JIT compilation + the runtime call machinery reaching a method's first run.

### footprint — working-set + jump-distance locality (observed output)

The raw `Addresses` answer two microarchitectural questions with pure arithmetic — distinct 4 KB
code pages touched, and how far consecutive instructions jump:

```
captured 1039938 instruction executions (truncated).
code working-set: 704 distinct 4 KB pages (2816 KB of code addresses touched).

jump-distance locality (|addr[i+1] - addr[i]|):
  straight-line (<16 B)     935878    90.0%  ############################
  near   (<256 B)            43722     4.2%  #
  far    (>=1 MB)            16754     1.6%  #
backward transitions (loops / returns): 40249 of 1039937 (3.9%).
```

### runtimebuckets — the runtime lump named by module (observed output)

`HwTrace.SymbolizeBuckets` (P/Invoke of `asmtest_hwtrace_symbolize_bucket`) resolves each address to
its module. A whole window is ~1M addresses and the resolver scans `/proc/self/maps` per address, so
the example resolves **by page** (a 4 KB page belongs to one mapping) — one representative per
distinct page, execution-weighted:

```
symbolized 1039938 captured addresses across 704 code pages into 10 module bucket(s) (window truncated).

         insns    share  module / JIT symbol
        592451    57.0%  libcoreclr.so
        395771    38.1%  libclrjit.so
         42175     4.1%  libc.so.6
           446     0.0%  memfd:doublemapper (deleted)   <- the JIT's own emitted code
```

`SymbolizeBuckets` reads `/proc/<pid>/maps` + the perf-map, not the trace, so it runs post-close on
the retained `Addresses`.

### instructionmix — the mnemonic-class histogram (observed output)

What *kind* of work runs: the first token of each `AsmInstruction.Text`, classed, with control flow
confirmed structurally via `Disas.IsCall/IsBranch/IsRet`:

```
1310 labelled instructions classified (structurally via Disas).

  data-move            498    38.0%  ########################
  stack                195    14.9%  #########
  compare/test         161    12.3%  ########
  branch               159    12.1%  ########
  call/ret             100     7.6%  #####
  arith/logic          129     9.8%  ######
  SIMD/FP               24     1.8%  #

control-flow density: 159 branches + 62 calls = 16.9 per 100 instructions.
```

### perfannotate — `perf annotate`, instruction-exact (observed output)

Region-scoped single-step gives the same view as `perf annotate` but EXACT (not sampled): every
instruction of a native routine with its true execution count. A branchy loop (add only even `i`)
makes the counts vary WITHIN the body:

```
count_even(10) = 20 (expect 20); 70 instructions executed over the region.

   count  heat              addr   instruction
      11  ################  0x06   cmp rcx, rdi
      10  ###############   0x0b   test rcx, 1
       5  #######           0x14   add rax, rcx     <- guarded: runs on EVEN i only
       1  #                 0x1c   ret
```

### loops — per-loop trip counts from backedges (observed output)

A backedge is a taken branch to a lower offset. Walk the ordered `InsnOffsets()` stream; a decrease
is the loop's back-branch firing once per iteration. A nested loop has two:

```
nested(3,4) = 12 (expect 12); 86 instructions executed.

  header  backedge       trips  back-branch instruction
  0x06    0x1f ->           3  jmp ...   <- outer loop
  0x0e    0x1a ->          12  jmp ...   <- inner loop (3*4)
```

Exact, not sampled — a static CFG shows the loops exist, not how many times each ran.

### descent — the exact nested call tree (observed output)

`Ptrace.TraceCallEx` + `Descent` single-steps a forked child INTO its callees as nested frames, so
every frame's own (self) and subtree (inclusive) counts are exact. A 3-level A→B→C blob:

```
A(100) = 107 (expect 107, = 100 + 4[C] + 2[B] + 1[A]); 3 frame(s) recorded.

  frame                       self    incl
  A @0x00                        4       9
    B @0x0d                      3       5
      C @0x17                    2       2
```

This is the noise-free tree the in-process `callgraph` example can only approximate.

### descent_dotnet — call descent against a live CoreCLR (observed output)

The out-of-process managed counterpart: attach to `jit_dotnet`'s new `chain` mode (`Program::Chain`
calls `Program::Leaf` twice), resolve both from the perf-map, and step INTO `Leaf` as nested frames
while it runs in *another*, GC'd, multi-threaded runtime:

```
resolved 'int32 [jit_dotnet] Program::Chain(int32,int32)[Optimized]'
  @ 0x…1a70 (56 bytes); callee 'Program::Leaf(...)' @ 0x…1ad0 (8 bytes) in live pid 259.

single-stepped the REAL JIT'd Program::Chain out of process (it returned 2763886902); 3 frame(s).

  frame                     self    incl
  Program::Chain              22      30
    Program::Leaf              4       4
    Program::Leaf              4       4
```

Same `posix_spawn` + raw `waitpid` + dedicated-thread watchdog discipline as `ptrace_dotnet`.
Self-skips (exit 0) where ptrace is denied or the SDK/methods are absent.

### codeimage — one address, two code bodies over logical time (observed output)

The time-aware code image answers "what bytes were live here at logical time N" — the read a
branch-trace decoder needs when a JIT reuses an address. A self-patched blob (v0: return 1 → v1:
return 2), both bodies kept in the timeline and both really run:

```
  executed v0 (before patch): returned 1 (expect 1)
  executed v1 (after  patch): returned 2 (expect 2)

BytesAt the SAME address, read AFTER the patch, at two logical times:
  seq 1:  b8 01 00 00 00 c3   mov eax, 1
  seq 2:  b8 02 00 00 00 c3   mov eax, 2
```

Honest caveat: a real tier0→tier1 relocates to a *new* address, so a fixed-region code image does
not capture managed tiering — this shows the mechanism on a controlled blob. Self-skips where the
recorder or W^X patching is unavailable.

## New capability & workflow examples

These land the [example-ideation follow-through](../../docs/internal/plans/dotnet-example-ideation-plan.md):
the tracing capabilities the table above didn't yet demonstrate, plus the real-world workflows a
developer reaches for tracing to do.

**Scoped forms** — `single-method` steps exactly one JIT'd body (the managed peer of `region`), with
none of the ~1M-instruction whole-window runtime amplification:

```
armed 'Main:53', Work(21,8) = 80.
rendered listing — the body's executed instructions:
    0:  push rbp   /  ...  /  a5: dec edi  /  ad: cmp ...,0  /  b1: jg ...   (246 insns, none of the runtime)
-- out-of-process (outOfProcess: true — crash-proof, no TF on this thread) -- Work(21,8) = 80
```

`async-stitch` follows one operation across an `await Task.Run(...)` hop and merges by `Seq`
(`hop 1 ran on tid 6 — a pool thread — yet stitched into the same operation`); `concurrent-isolation`
runs two overlapping `EFLAGS.TF` windows and proves neither truncates the other (A=21630 insns, B=32433,
both `Truncated=False`).

**Workflows** — `perf-triage-drill` surveys, then drills (`Hot` wins the by-method histogram at 10589
insns, then its exact body is rendered); `trace-diff` diffs two runs:

```
run A: add2(20,22)=42 (jle TAKEN, dec skipped)   run B: add2(60,60)=119 (falls through, dec runs)
block 0x0e  dec rax  + ON in B   <- the block the change turned on ;  per-insn delta 0x0e B-A=+1
```

`coverage-guided-fuzz` flags the input that first reaches a rare guarded block (`validate(4919)=57005 KEEP
+1 block 0x20 <== deep bug block reached`); `trace-cost-overhead` prints the honest tier cost:

```
untraced          1.19 us   1.0x        block-step (OOP fork)   29.69 ms   2 blocks     24,976x
single-step      160.31 ms  134,857x    OOP single-step (fork)  37.65 ms   9,002 insns  31,674x
AMD LBR region     5.83 us  4.9x
```

**Family shape** — `crashproof-showdown` runs the fatal boundary as an observed fact
(`in-process child exit code = 133 (128+SIGTRAP) — force-killed as predicted` … `out-of-process leg:
SURVIVED, captured 10 methods`);
`crashproof-survey` tables Method vs Window vs WindowHot; `tier-ladder` prints this host's cascade
with a reason per rung and shows `CeilingFree` drop the AMD LBR rung + `NativeOnly` drop the emulator
floor. `descend-all` auto-descends unknown callees and trips its guardrail honestly
(`SetMaxDepth(1) → DepthCapped=true, C folded into B`).

**AMD** — `amd-period-sweep` sweeps the survey period (smaller = more samples/throttle, larger =
coarser); `amd-snapshot` is the deterministic boundary snapshot that captures a tiny routine the
sampler truncates (self-skips `built without the BPF toolchain` where `CAP_BPF`/libbpf is absent).

## API used

- `new AsmTrace()` / `new AsmTrace(code)` / `new AsmTrace(byMethod: true)` /
  `new AsmTrace(withRundown: true)` — the empty-ctor, region-scoped, method-labelling
  (cold), and rundown (warm + cold) scopes.
- `AsmTrace.RundownEnabled` — whether the §D0.2 rundown was accepted; `DiagnosticsIpc`
  (`EnablePerfMap(JitDump)`) / `JitMethodMap.LoadJitDump` are the underlying pieces.
- `AsmTrace.Addresses` — the raw absolute addresses a whole-window scope captured.
- `AsmTrace.CountInRange(start, len)` — how many landed in a known native region
  (tells native leaves apart; used by [wholewindow/](wholewindow/)).
- `AsmTrace.Methods` / `.LabelledInstructions` / `.MethodsObserved` /
  `.InstructionsIn(name)` — the per-managed-method breakdown of a `byMethod` scope
  (data only; the caller presents it). Used by [methods/](methods/).
- `AsmMethod.Assembly` / `.ShortName` — split a labelled name into its declaring assembly
  and its bare `Type::Method(sig)` (empty assembly for a listener-spelled dotted name).
  Used by [assemblies/](assemblies/).
- `AsmTrace.Disassembly` / `.DisassemblyAvailable` and `AsmInstruction` (`.Address` / `.Text`
  / `.Method` / `.ShortMethod` / `.Assembly` / `.RuntimeBefore`) — the labelled execution
  stream, each instruction disassembled and paired with its method. Used by
  [annotated/](annotated/), [hotspots/](hotspots/), and [callgraph/](callgraph/).
- `Ptrace.TraceCall(code, len, args, trace)` / `Ptrace.Available()` — single-step native code
  out of process (a self-contained forked child) into an `HwTrace` handle (`.Handle` /
  `InsnOffsets()` / `BlockOffsets()`). Used by [ptrace_native/](ptrace_native/).
- `Ptrace.ProcPerfmapSymbol(pid, name)` / `.ProcRegionByAddr(pid, addr)` / `.RunTo(pid, addr)` /
  `.TraceAttached(pid, base, len, trace)` — resolve + single-step a method in an
  externally-attached process. Used by [ptrace_dotnet/](ptrace_dotnet/).
- `AsmMethod.Tier` — the JIT/compilation tier (`PreJIT`/`MinOptJitted`/`OptimizedTier1`/…;
  empty for an untagged listener name). Used by [tiers/](tiers/).
- `HwTrace.Create(blocks:N, instructions:M)` + `Register` + `Region` and
  `BlockOffsets()` / `Covered(off)` / `BlocksLen()` / `InsnOffsets()` / `InsnsTotal()` /
  `Truncated()` — region **coverage** (basic blocks + instruction offsets). Used by
  [coverage/](coverage/).
- `AsmTrace.Path` — the rendered disassembly (region-scoped form).
- `AsmTrace.Armed` / `.Truncated` / `.SkipReason` — arm state and honest degradation.
- `JitMethodMap` — the underlying in-process address→method map (§D0.1), if you want
  it standalone; `Stop()` then `Freeze()` then `Resolve(addr)`.
- `HwTrace.SymbolizeBuckets(ips, pid, cap)` / `HwBucket` — bucket ABSOLUTE addresses by
  their containing module / JIT symbol (P/Invoke of `asmtest_hwtrace_symbolize_bucket`;
  post-close safe). Used by [runtimebuckets/](runtimebuckets/).
- `Ptrace.TraceCallEx(code, args, trace, descent, region)` /
  `Ptrace.TraceAttachedEx(pid, base, len, trace, descent)` and `Descent`
  (`AllowRegion` / `SetMaxDepth` / `Frames*` / `Edges`) — the call-descent shadow stack for
  exact nested trees. Used by [descent/](descent/) and [descent_dotnet/](descent_dotnet/).
- `CodeImage.Track` / `.Refresh` / `.Now` / `.BytesAt` — the time-aware code image: what bytes
  were live at an address as of a logical timestamp. Used by [codeimage/](codeimage/).
- `Disas.Available` / `.IsCall` / `.IsBranch` / `.IsRet` — structural control-flow classifiers
  over live addresses. Used by [instructionmix/](instructionmix/), [loops/](loops/), and
  [callgraph/](callgraph/).
- `AsmTrace.Method(del, emit, outOfProcess)` + `.Invoke(args)` / `.Path` / `.Truncated` — lazy-arm
  and step exactly one managed method body (in-process, or crash-proof out of band). Used by
  [single-method/](single-method/), [perf-triage-drill/](perf-triage-drill/),
  [concurrent-isolation/](concurrent-isolation/), [crashproof-survey/](crashproof-survey/).
- `AsmStitchedTrace` (`.Step(del, args)` / `.Complete()` / `.Hops` / `StitchHop` / `.Path`) — stitch
  one logical operation's hops across threads into a seq-ordered trace. Used by [async-stitch/](async-stitch/).
- `AsmTrace.Window(Action)` / `AsmTrace.WindowHot(Action, period)` — the crash-proof out-of-process
  (exact) and AMD-LBR (sampled, `IsStatistical`) whole-window forms. Used by
  [crashproof-showdown/](crashproof-showdown/), [crashproof-survey/](crashproof-survey/), [amd-period-sweep/](amd-period-sweep/).
- `HwTrace.ResolveTiers(policy)` / `.AutoTier(policy)` / `.Resolve` / `.Auto` / `.DegradationNote()` /
  `.LibraryPath()` and `TierChoice` / `TracePolicy` — the host's cross-tier degradation cascade.
  Used by [tier-ladder/](tier-ladder/).
- `Descent(DescentLevel.DescendAll)` + `SetMaxDepth` / `SetInsnBudget` / `SetWatchdogMs` /
  `UseDefaultDenylist` / `DepthCapped()` — auto-descend unknown callees under guardrails. Used by
  [descend-all/](descend-all/).
- `AmdSnapshot.Available()` / `.Trace(code, exitOff, run, trace)` — the deterministic boundary LBR
  snapshot (`asmtest_amd_snapshot_trace`). Used by [amd-snapshot/](amd-snapshot/).
