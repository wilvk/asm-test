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
| [rundown/](rundown/) | `new AsmTrace(withRundown: true)` | also names **warm** methods (§D0.2) via an in-process perf-map rundown — dependency-free, no launch knob |

## Run them

```sh
make hwtrace-dotnet-example          # runs all three, in a plain container / on this host
make docker-hwtrace-dotnet-example   # runs all three, in the asmtest-dotnet image

# or one at a time:
dotnet run --project examples/dotnet/wholewindow/wholewindow.csproj
dotnet run --project examples/dotnet/region/region.csproj
dotnet run --project examples/dotnet/methods/methods.csproj
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
`libasmtest_hwtrace` (auto-resolved from the in-tree `build/`).

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

## rundown — naming WARM methods too (§D0.2, observed output)

The §D0.1 `JitMethodMap` only names methods JIT'd *inside* the scope, so warm methods
(e.g. `Program::Main`, the BCL) land in the runtime remainder. `withRundown: true` asks
the runtime — over its own diagnostics socket, **no NuGet, no `DOTNET_PerfMapEnabled`** —
to run down all already-JIT'd methods into `/tmp/perf-<pid>.map`, which `AsmTrace` folds
into the breakdown:

```
rundown enabled: True; captured 973328 instructions (truncated); 8 methods labelled; 156 instructions.
    ...
    22  instance void [rundown] Asmtest.AsmTrace::.ctor(...)[MinOptJitted]   <- WARM (perf-map)
    20  int32 [rundown] Program::Main()[MinOptJitted]                        <- WARM (perf-map)
    11  Program.Work                                                          <- cold (listener)
     6  System.Threading.Monitor.Enter                                        <- cold (listener)
-> a WARM method — Program::Main, JIT'd at startup BEFORE the scope — is now named.
```

Warm methods carry the perf-map `::` spelling; cold in-scope methods keep the listener's
dotted spelling. Self-skips to the cold-only result where diagnostics are off
(`DOTNET_EnableDiagnostics=0` → `rundown enabled: False`). See
[dotnet-perfmap-rundown-plan.md](../../docs/plans/dotnet-perfmap-rundown-plan.md).

## API used

- `new AsmTrace()` / `new AsmTrace(code)` / `new AsmTrace(byMethod: true)` /
  `new AsmTrace(withRundown: true)` — the empty-ctor, region-scoped, method-labelling
  (cold), and rundown (warm + cold) scopes.
- `AsmTrace.RundownEnabled` — whether the §D0.2 rundown was accepted; `DiagnosticsIpc` /
  `JitMethodMap.LoadPerfMap` are the underlying pieces.
- `AsmTrace.Addresses` — the raw absolute addresses a whole-window scope captured.
- `AsmTrace.CountInRange(start, len)` — how many landed in a known native region
  (tells native leaves apart; used by [wholewindow/](wholewindow/)).
- `AsmTrace.Methods` / `.LabelledInstructions` / `.MethodsObserved` /
  `.InstructionsIn(name)` — the per-managed-method breakdown of a `byMethod` scope
  (data only; the caller presents it). Used by [methods/](methods/).
- `AsmTrace.Path` — the rendered disassembly (region-scoped form).
- `AsmTrace.Armed` / `.Truncated` / `.SkipReason` — arm state and honest degradation.
- `JitMethodMap` — the underlying in-process address→method map (§D0.1), if you want
  it standalone; `Stop()` then `Freeze()` then `Resolve(addr)`.
