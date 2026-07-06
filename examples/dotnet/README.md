# examples/dotnet — scoped in-process tracing, live

A runnable .NET demo of the scoped-trace facility, centred on the aspirational
**empty-ctor** form from the
[zero-config plan](../../docs/plans/scoped-tracing-zeroconfig-plan.md) (§Z0/§Z1):

```csharp
using (new AsmTrace())      // no NativeCode, no [base,len) — zero config
{
    HotPath(data);          // whatever runs here is captured …
}                           // … and its executed asm rendered on Dispose
```

## Run it

```sh
make hwtrace-dotnet-example          # in a plain container / on this host
make docker-hwtrace-dotnet-example   # in the asmtest-dotnet image
```

It self-skips cleanly (exit 0) where the single-step backend cannot run (e.g. off
Linux). On this dev box — an AMD Zen 5 with no Intel PT — it runs via the
single-step **WEAK** tier: the CPU traps after every instruction the thread
executes inside the scope, and the recorded absolute addresses are disassembled
from live memory on `Dispose`.

## What it shows (observed output on this machine)

The example runs the **same scope two ways** so the honest trade-off is visible.

**(1) `new AsmTrace()` — zero config, whole-window.** It captures *everything* the
thread ran. For a managed caller that is the .NET runtime's own JIT'd code, so the
window is large and honestly `truncated`:

```
    armed, auto-named 'Main:64', call returned 42.
    captured 65536 instructions of real executed machine code (window overflowed — truncated).
    a sample of the disassembly (this is the .NET runtime's own
    JIT'd code executing the call — the honest whole-window cost):
        pop rbp
        ret
        mov eax, 0
        mov rdx, qword ptr [rbp - 8]
        sub rdx, qword ptr fs:[0x28]      <- a real CLR stack-canary check
        je 0x...
```

This is the plan's *"you get everything, including the runtime"* — faithful, and
noisy. Pointing single-step at live managed code is a documented footgun; the tiny
native leaf is buried in the CLR's own code, which is why the clean managed path
is the **STRONG** whole-window PT tier (region-filtered at decode), forward-look on
this AMD host.

**(2) `new AsmTrace(code)` — same import + scope, scoped to one routine.** The same
facility, scoped to a native region, yields *exactly* its executed instructions:

```
    armed, auto-named 'Main:88', call returned 42.
    rendered listing — EXACTLY the routine's executed instructions:
             0:	mov rax, rdi
             3:	add rax, rsi
             6:	cmp rax, 0x64
             c:	jle 0x...
            11:	ret
    truncated: False
    -> region-scoped gives the clean, isolated assembly path.
```

`add2(20, 22) = 42`, so the `cmp`/`jle` is taken and the `dec` at offset `0xc` is
correctly absent from the trace.

## Files

- [Program.cs](Program.cs) — the demo (both scope forms).
- [asmscope.csproj](asmscope.csproj) — dependency-free console app; compiles the
  binding source ([HwTrace.cs](../../bindings/dotnet/hwtrace/HwTrace.cs)) directly
  and P/Invokes `libasmtest_hwtrace` (auto-resolved from the in-tree `build/`).
