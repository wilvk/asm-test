# examples/dotnet — scoped in-process tracing, live

Runnable .NET demos of the scoped-trace facility from the
[zero-config plan](../../docs/plans/scoped-tracing-zeroconfig-plan.md) (§Z0/§Z1),
one project per scope form. Both run live on this dev box — an AMD Zen 5 with no
Intel PT — via the single-step **WEAK** tier, and self-skip cleanly (exit 0) where
single-step cannot run.

| Project | Scope form | Shows |
|---|---|---|
| [wholewindow/](wholewindow/) | `using (new AsmTrace())` | zero-config whole-window capture + attributing two native leaves apart from the runtime |
| [region/](region/) | `using (new AsmTrace(code))` | the same scope shape, scoped to one routine → exactly its assembly |
| [methods/](methods/) | `using (new AsmTrace())` | labelling the captured window by **managed method** (§D0.1) — names an arbitrary **cold** method |
| [rundown/](rundown/) | `new AsmTrace(withRundown: true)` | also names **warm + ReadyToRun (R2R) BCL** methods (§D0.2) via an in-process jitdump rundown — dependency-free, no launch knob |
| [assemblies/](assemblies/) | `new AsmTrace(byMethod: true, withRundown: true)` | groups the labelled window **by declaring assembly**, listing the methods called in each (`AsmMethod.Assembly` / `.ShortName`) |
| [annotated/](annotated/) | `new AsmTrace(byMethod: true, withRundown: true)` | an **annotated execution trace** — each executed instruction next to the method it ran in (`AsmTrace.Disassembly` / `AsmInstruction`) |

## Run them

```sh
make hwtrace-dotnet-example          # runs all six, in a plain container / on this host
make docker-hwtrace-dotnet-example   # runs all six, in the asmtest-dotnet image

# or one at a time:
dotnet run --project examples/dotnet/wholewindow/wholewindow.csproj
dotnet run --project examples/dotnet/region/region.csproj
dotnet run --project examples/dotnet/methods/methods.csproj
dotnet run --project examples/dotnet/rundown/rundown.csproj
dotnet run --project examples/dotnet/assemblies/assemblies.csproj
dotnet run --project examples/dotnet/annotated/annotated.csproj
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
[docs/plans/scoped-tracing-zeroconfig-plan.md](../../docs/plans/scoped-tracing-zeroconfig-plan.md)
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
[dotnet-perfmap-rundown-plan.md](../../docs/plans/dotnet-perfmap-rundown-plan.md).

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
  [annotated/](annotated/).
- `AsmTrace.Path` — the rendered disassembly (region-scoped form).
- `AsmTrace.Armed` / `.Truncated` / `.SkipReason` — arm state and honest degradation.
- `JitMethodMap` — the underlying in-process address→method map (§D0.1), if you want
  it standalone; `Stop()` then `Freeze()` then `Resolve(addr)`.
