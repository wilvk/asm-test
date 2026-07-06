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

## Run them

```sh
make hwtrace-dotnet-example          # runs both, in a plain container / on this host
make docker-hwtrace-dotnet-example   # runs both, in the asmtest-dotnet image

# or one at a time:
dotnet run --project examples/dotnet/wholewindow/wholewindow.csproj
dotnet run --project examples/dotnet/region/region.csproj
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

## API used

- `new AsmTrace()` / `new AsmTrace(code)` — the empty-ctor and region-scoped scopes.
- `AsmTrace.Addresses` — the raw absolute addresses a whole-window scope captured
  (range-classify these to attribute them).
- `AsmTrace.Path` — the rendered disassembly (region-scoped form).
- `AsmTrace.Armed` / `.Truncated` / `.SkipReason` — arm state and honest degradation.
