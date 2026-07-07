# .NET binding

The [.NET binding](https://github.com/wilvk/asm-test/tree/main/bindings/dotnet)
drives asm-test from C# / .NET via **P/Invoke** (`DllImport`). It ships a reusable
library module,
[`Asmtest.cs`](https://github.com/wilvk/asm-test/blob/main/bindings/dotnet/Asmtest.cs),
that keeps all P/Invoke inside and exposes the `Regs` / `Emu` / `EmuResult` types
plus an `Assert` helper — so your test code never declares a native entry point.

The entry points are the opaque-handle FFI layer, so no C struct layout is
mirrored: `asmtest_corpus_routine` for routine addresses, `asmtest_capture6` /
`_fp2` + `asmtest_regs_*` accessors for capture, and `asmtest_emu_call2` +
accessors for the emulator (faults as data: `Faulted`, plus `FaultAddr` /
`FaultKind` for where and why one hit). See [Language bindings](index.md)
for the shared architecture.

## Setup

From the repository root, build the native library:

```sh
make shared-emu      # libasmtest_emu.{so,dylib} — capture trampoline + emulator + FFI accessors
```

Add [`Asmtest.cs`](https://github.com/wilvk/asm-test/blob/main/bindings/dotnet/Asmtest.cs)
to your test project (until the NuGet package ships) and export the library path
so `asmtest_emu` resolves by soname:

```sh
export LD_LIBRARY_PATH=$PWD/build      # DYLD_LIBRARY_PATH on macOS
```

## Usage

Load your routine library with `NativeLibrary`, and assert with any runner
(xUnit shown):

```csharp
using System;
using System.Runtime.InteropServices;
using Xunit;
using Asm = Asmtest;   // alias so Asm.Assert doesn't collide with Xunit.Assert

public class MyRoutineTests {
    static readonly IntPtr Lib = NativeLibrary.Load("./libmyroutines.so");
    static IntPtr Fn(string name) => NativeLibrary.GetExport(Lib, name);

    [Fact] public void AddSigned() {
        using var r = new Asm.Regs();
        r.Capture6(Fn("add_signed"), 40, 2);    // call through the real ABI
        Asm.Assert.Ret(r, 42);
        Asm.Assert.AbiPreserved(r);              // callee-saved registers restored
    }

    [Fact] public void FpAdd() {
        using var r = new Asm.Regs();
        r.CaptureFp2(Fn("fp_add"), 1.5, 2.25);
        Asm.Assert.Fp(r, 3.75);
    }

    [Fact] public void UnderEmulator() {         // faults become data, never a crash
        using var e = new Asm.Emu();
        using var res = e.Call2(Fn("add_signed"), 40, 2);
        Asm.Assert.NoFault(res);
        Asm.Assert.EmuReg(res, "rax", 42);
    }
}
```

The wrapper's `Corpus.Routine(name)` resolves the built-in fixtures;
`NativeLibrary` (as above) resolves your own routines.

## In-line assembler (optional)

Pass a routine as an **assembly string**. `libasmtest_emu` carries the Keystone
in-line assembler, so this works out of the box under `make dotnet-test`.
`Emu.AsmAvailable` is a defensive probe — false only against an older/leaner lib
pointed at by `ASMTEST_LIB`.

```csharp
[Fact] public void InlineAssembler() {       // optional: the routine IS the text
    if (!Asm.Emu.AsmAvailable) return;        // defensive: false only against an older/leaner lib
    using var e = new Asm.Emu();
    // Intel, up to six args; throws AsmtestException (carrying the Keystone
    // diagnostic) if the string fails to assemble.
    using var res = e.CallAsm("mov rax, rdi; add rax, rsi; ret", new long[] { 40, 2 });
    Asm.Assert.EmuReg(res, "rax", 42);
    // AT&T syntax + an instruction cap, then assemble-only for another arch:
    e.CallAsm("mov %rdi,%rax; add %rsi,%rax; ret", new long[] { 10, 32 },
              Asm.AsmSyntax.Att, maxInsns: 2);
    byte[] arm64Ret = Asm.Emu.Assemble("ret", Asm.AsmArch.Arm64);   // C0 03 5F D6
}
```

The multi-arch `Emu.Assemble(…)` covers x86-64/arm64/riscv64/arm32 — even guests
the x86 emulator can't run.

## Function reference

Every public member of `Asmtest.cs`, with an example and its options. All six
handle types (`Regs`, `Emu`, `EmuResult`, `Trace`, `Guest`, `GuestResult`)
are `IDisposable` — wrap them in `using`. A routine reference is an `IntPtr`
address (`Corpus.Routine(name)` or `NativeLibrary.GetExport`); the byte-array
paths take raw machine code.

### Resolving routines

```csharp
IntPtr fn  = Asm.Corpus.Routine("add_signed");          // a built-in corpus fixture
IntPtr mine = NativeLibrary.GetExport(
                  NativeLibrary.Load("./libmyroutines.so"), "my_routine");
bool hasAsm = Asm.Emu.AsmAvailable;                      // assembler compiled in?
```

### Capture tier — `Regs`

```csharp
using var r = new Asm.Regs();
r.Capture6(fn, 40, 2);                 // up to 6 integer args (missing default to 0)
r.CaptureFp2(fn, 1.5, 2.25);           // two double args; FP return in r.FRet
r.CaptureVecF32(fn, new[] {            // up to 8 128-bit vectors, four float32 lanes each
    new float[] { 1, 2, 3, 4 } });
ulong  ret  = r.Ret;                   // integer return (rax)
double fret = r.FRet;                  // scalar double return (xmm0)
float[] v   = r.VecF32(0);             // four float32 lanes of vector register 0
bool cf     = r.FlagSet("CF");         // condition flag by name (CF/PF/ZF/SF/OF)
bool abi    = r.AbiPreserved;          // every callee-saved register restored
```

* `Capture6(fn, a0=0 … a5=0)` — six optional `long` slots.
* `CaptureFp2(fn, f0, f1)` — two `double`s into xmm0/xmm1.
* `CaptureVecF32(fn, float[][] vectors)` — each inner array is four float32 lanes
  (one 128-bit vector); up to eight. The vector return is `VecF32(0)`.

### Emulator tier — `Emu` / `EmuResult`

```csharp
using var e = new Asm.Emu();                       // x86-64 Unicorn guest
using var res = e.Call2(fn, 40, 2);                // routine address + two int args
using var rb  = e.CallBytes(code, 40, 2);          // raw machine-code bytes, up to 6 int args
using var rf  = e.CallFp(code, new long[]{1}, new double[]{1.5});   // doubles -> xmm0..7
using var rv  = e.CallVec(code, null, new[]{ new float[]{1,2,3,4} });// 128-bit vecs -> xmm0..7
using var rw  = e.CallWin64(code, 1, 2, 3, 4);     // Microsoft x64 (rcx, rdx, r8, r9)
```

* `Call2(fn, a0, a1)` — the only path that takes a routine **address** (reads a
  64-byte code window); two integer args.
* `CallBytes` / `CallFp` / `CallVec` / `CallWin64` / `CallTraced` take a raw
  `byte[] code` and run it whole — for assembled snippets and cross-convention
  routines. A `null` `iargs`/`args` is treated as empty.

Read the outcome (faults are data, never a crash):

```csharp
bool   faulted = res.Faulted;          // hit an invalid access?
ulong  addr    = res.FaultAddr;        // where (valid when Faulted)
Asm.FaultKind k = res.FaultKind;       // None / Read / Write / Fetch
ulong  rax     = res.Reg("rax");       // any GP register, plus "rip" / "rflags"
double d       = res.XmmF64(0, 0);     // xmm lane as double (scalar FP return)
float  f       = res.XmmF32(0, 0);     // xmm lane as float32 (vector return)
```

### Execution trace / coverage — `Trace`

```csharp
using var t = new Asm.Trace(insnsCap: 4096, blocksCap: 4096);
using var res = e.CallTraced(code, new long[]{ 1, 2 }, t);
bool entered = t.Covered(0x0);         // was the basic block at this byte-offset hit?
Asm.Assert.Covered(t, 0x0);            // the assertion form
```

`new Trace(insnsCap, blocksCap)` sets the recorder's buffer capacities (default
4096 each). `Emu.CallTraced(code, args, trace)` records while running.

### Native tracing — `NativeTrace` (optional, DynamoRIO)

A separate, optional tier (see [Native runtime tracing](../guides/tracing/native-tracing.md))
traces **host-native code as it runs inside this process** via an in-process
DynamoRIO client, rather than emulated guest bytes. Bring DynamoRIO up once with
`DrTrace.Initialize`, materialize machine code as a `NativeCode`, register it
under a name into a `NativeTrace`, run it inside the marked region, then read back
coverage and the ordered instruction stream. Always self-skip on
`DrTrace.Available()` — it never throws and returns false when the lib or
DynamoRIO is absent.

```csharp
using Asm = Asmtest;

if (!DrTrace.Available()) return;          // self-skip: no lib / no DynamoRIO
DrTrace.Initialize();                       // dr_app_setup + dr_app_start, once
try {
    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret
    var bytes = new byte[] {
        0x48,0x89,0xF8, 0x48,0x01,0xF0, 0x48,0x3D,0x64,0x00,0x00,0x00,
        0x7E,0x03, 0x48,0xFF,0xC8, 0xC3 };
    var code = Asm.NativeCode.FromBytes(bytes);

    // instruction mode: 64 blocks, 64 insns
    var trace = Asm.NativeTrace.Create(blocks: 64, instructions: 64);
    trace.Register("addcap", code);
    trace.Region("addcap", () => code.Call(40, 2));   // 42 <= 100 -> jle taken
    bool entered = trace.Covered(0x0);                 // entry block hit

    ulong[] blocks = trace.BlockOffsets();             // distinct block starts
    ulong[] insns  = trace.InsnOffsets();              // jle-taken path: {0x0, 0x3, 0x6, 0xc, 0x11}

    trace.Free();
    code.Free();

    // symbol mode: trace an exported function by name, no region/markers
    var sym = Asm.NativeTrace.Create(blocks: 64, instructions: 64);
    sym.RegisterSymbol("asmtest_symbol_demo", 256);
    long r = DrTrace.SymbolDemo(3, 4);                 // a*2+b == 10
    bool symHit = sym.Covered(0x0);
    sym.Free();
} finally {
    DrTrace.Shutdown();                                 // back to UNINIT
}
```

Under .NET — a managed runtime — this in-process tier **self-skips at run time**:
in-process DynamoRIO can't take over the CLR's background threads, so prefer the
out-of-band Intel PT path (see the central doc). Linux x86-64 only; full reference
in [Native runtime tracing](../guides/tracing/native-tracing.md).

### Hardware / single-step tracing — `HwTrace` (optional)

A sibling native tier records the **same** `asmtest_trace_t` coverage from the real
CPU, but needs no separate engine install: it defaults to the **single-step**
backend (the CPU's `EFLAGS.TF` trap flag), so `HwTrace.Available(...)` is true and
it **traces live on any x86-64 Linux** — CI and plain containers included — where
the DynamoRIO tier self-skips under .NET (the CLR's threads block in-process
takeover). Intel PT and AMD LBR are picked automatically on the bare-metal hardware
that has them.

```csharp
using Asmtest;

if (!HwTrace.Available(HwBackend.SingleStep)) return;   // self-skip off x86-64 Linux
HwTrace.Init(HwBackend.SingleStep);
try {
    // mov rax,rdi; add rax,rsi; cmp rax,100; jle +3; dec rax; ret  (two blocks)
    var bytes = new byte[] {
        0x48,0x89,0xF8, 0x48,0x01,0xF0, 0x48,0x3D,0x64,0x00,0x00,0x00,
        0x7E,0x03, 0x48,0xFF,0xC8, 0xC3 };
    var code = NativeCode.FromBytes(bytes);

    var tr = HwTrace.Create(blocks: 64, instructions: 64);
    tr.Register("add2", code);
    long r = 0;
    tr.Region("add2", () => { r = code.Call(20, 22); });   // 42; jle taken, dec skipped

    ulong[] insns = tr.InsnOffsets();      // {0x0, 0x3, 0x6, 0xc, 0x11} — == Unicorn/PT
    bool entered  = tr.Covered(0);
    tr.Free();
    code.Free();
} finally {
    HwTrace.Shutdown();
}
```

`HwTrace.Resolve(HwPolicy.Best)` / `HwTrace.Auto(HwPolicy.Best)` pick the host's
most-faithful available backend (Intel PT → AMD LBR → single-step), and
`HwTrace.ResolveTiers` / `AutoTier` extend the cascade across the DynamoRIO and
emulator tiers. An out-of-process `Ptrace` surface traces a method in a **separate**
process (fork-and-step, foreign-process attach + run-to-method, and `/proc`-map /
jitdump resolution) — the managed-runtime path. Full reference in
[Native runtime tracing](../guides/tracing/native-tracing.md).

The `Descent` class (`IDisposable`) makes the tracer follow call-outs instead of stepping
over them: `new Descent(DescentLevel.DescendKnown)`, optionally `AllowRegion(base, len)` or a
`SetResolver(...)` delegate (a P/Invoke upcall, `GCHandle`-pinned for the handle's lifetime),
then `Ptrace.TraceCallEx(code, args, trace, descent, region)`. Read `descent.Edges()` and
`descent.FrameInsns(f)` for each nested frame. Level 3 (`DescendAll`) is default-off and
best-effort on a live runtime — see [Call descent levels](../guides/tracing/native-tracing.md#call-descent-levels).

**Scoped tracing** — the reference `using` scope (`AsmTrace : IDisposable`), the
canonical *import + scope* shape. It auto-names the region from the call site
(`[CallerMemberName]` / `[CallerLineNumber]`), and `Dispose` renders the executed
assembly into `Path` (and to stdout unless `emit: false`); `Truncated` is the
thread-scope honesty bit.

```csharp
HwTrace.Init(HwBackend.SingleStep);
var code = NativeCode.FromBytes(new byte[] { 0x48,0x89,0xF8, 0x48,0x01,0xF0, 0xC3 }); // add2; ret
AsmTrace scope;
using (scope = new AsmTrace(code, emit: false))   // auto-named "Method:<line>"
    code.Call(20, 22);                             // 42
// scope.Path holds the disassembly that executed; scope.Truncated is the thread-scope bit
HwTrace.Shutdown();
```

**Whole-window (zero-config) scope** — the empty ctor needs no region and no
`HwTrace.Init` (it auto-inits the portable single-step tier, §Z0) and never
throws: where no backend can run it self-skips with `Armed == false` and a
human-readable `SkipReason`. The default is **data-only** — `Path` stays empty
and the scope exposes the captured window as data; `renderPath: true` opts into
rendering `Path` (live-memory disassembly + truncation banner).

```csharp
AsmTrace ww;
using (ww = new AsmTrace(emit: false))     // zero config: captures whatever runs here
    HotPath(data);
// ww.Addresses    — the raw absolute addresses, in execution order
// ww.CountInRange(base, len)              — attribute a known native region
// ww.Truncated / ww.Armed / ww.SkipReason — the honesty surface
```

**Managed-method labelling (§D0.1/§D0.2)** — `byMethod: true` gives the scope an
in-process JIT map (CoreCLR `MethodLoadVerbose`) that names methods JIT'd inside
the window; `withRundown: true` additionally asks the runtime — over its own
diagnostics socket, no NuGet dependency, no launch knob — for a jitdump rundown,
so **warm** (JIT'd before the scope) and **ReadyToRun BCL** methods are named
too. `RundownEnabled` reports whether the runtime accepted it (false ⇒ clean
cold-only degradation, e.g. under `DOTNET_EnableDiagnostics=0`).

```csharp
AsmTrace ww;
using (ww = new AsmTrace(emit: false, byMethod: true, withRundown: true))
    Work();
// ww.Methods              — per-method instruction counts (AsmMethod.Assembly/.ShortName)
// ww.InstructionsIn(name) — instructions attributed to methods matching `name`
// ww.Disassembly          — the labelled stream (AsmInstruction: address, text, method)
```

Tiering note: for a `byMethod` scope the labelling now decodes against the code-image
**version live in the window** (the map feeds `asmtest_codeimage_track` per
`MethodLoadVerbose`, and close-time disassembly renders at that version), so a body that
re-tiers or moves *after* the window still renders the bytes that ran; only untracked
native-runtime addresses fall back to live memory. Runnable demos of every report live
under [examples/dotnet/](https://github.com/wilvk/asm-test/tree/main/examples/dotnet).

**Named-method form (§D0.3, Option B lazy-arm)** — `AsmTrace.Method(delegate)` traces one
managed method's own JIT'd body: reliable, exact offsets, where the whole-window form is
best-effort. It resolves the standalone body (forcing the JIT via `PrepareMethod`, reading
the address from the listener, or the jitdump rundown for a warm/R2R body), registers it as
a region, and self-skips (never throws) if resolution or arming fails. `Invoke` is
**managed-safe by construction**: it mints a native pointer to the body and does
`arm → call → disarm` entirely in native code (`asmtest_hwtrace_call_scoped`), so neither
`DynamicInvoke` nor any runtime machinery runs under `EFLAGS.TF` — an in-window
`pthread_create` cannot fault the process (the whole-window form has no such guarantee). See
[managed-singlestep-lazy-arm-plan.md](https://github.com/wilvk/asm-test/blob/main/docs/internal/plans/managed-singlestep-lazy-arm-plan.md).

```csharp
using var t = AsmTrace.Method((Func<long,long,long>)HotPath, emit: false);
var r = (long)t.Invoke(20L, 22L);   // Invoke is the library's own NoInlining call site
// t.Path is exactly HotPath's executed body; t.Armed / t.SkipReason are the honesty bits
```

`Invoke` marshals through a `(long…)->long` shim table (arities 0–6); a signature it cannot
express (ref/out, structs, >6 args) **auto-falls back** to the out-of-process stepper rather
than stepping the reflection machinery in-process. Pass `outOfProcess: true` to force that
route explicitly — a bundled helper reverse-attaches and single-steps the body out of band,
so the calling thread is **never** armed with `EFLAGS.TF` (self-skips, with a `SkipReason`,
where Yama refuses the attach — never a silent miss). The call always runs; only its capture
degrades.

**Structural classifiers** — `Disas.IsCall/IsBranch/IsRet(addr)` and
`Disas.TryCallTarget(addr, out target)` classify a live instruction address (Capstone-gated),
so a caller walking `Disassembly` can follow control flow without string-parsing mnemonics.

### Cross-arch guests — `Guest` / `GuestResult`

```csharp
using var g = new Asm.Guest(Asm.GuestArch.Arm64);  // Arm64 | RiscV | Arm
using var res = g.Call(code, 40, 2);               // raw bytes, ints in the guest ABI regs
ulong x0 = res.Reg("x0");                          // by name: x0/sp, a0/x10, r0…
bool flt = res.Faulted;                            // faults are data here too
using var tr = g.CallTraced(code, new long[]{1}, t);  // arm64 only (throws otherwise)
```

`new Guest(arch)` opens a guest that runs **raw machine-code bytes** on any host.
`GuestResult` exposes `Faulted` and `Reg(name)`; `CallTraced` is wired for arm64.

### In-line assembler — `Emu.CallAsm` / `Emu.Assemble` (optional)

```csharp
if (Asm.Emu.AsmAvailable) {
    using var res = e.CallAsm("mov rax, rdi; add rax, rsi; ret", new long[]{40, 2});
    using var r2  = e.CallAsm("mov %rdi,%rax; ret", new long[]{42},
                              Asm.AsmSyntax.Att, maxInsns: 8);   // syntax + insn cap
    byte[] bytes  = Asm.Emu.Assemble("ret", Asm.AsmArch.Arm64);  // text -> bytes, any arch
}
```

* `CallAsm(src, args=null, syntax=Intel, maxInsns=0)` — assemble x86-64 `src` and
  run it (≤6 int args). `maxInsns: 0` runs to `ret`. Throws `AsmtestException`
  (Keystone diagnostic) on a bad string or a Keystone-free build.
* `Assemble(src, arch=X86_64, syntax=Intel, addr=0x00100000)` — assemble-only,
  any of `AsmArch.{X86_64,Arm64,RiscV64,Arm32}`; `addr` is the base load address.
* `AsmSyntax` covers `Intel`/`Att`/`Nasm`/`Masm`/`Gas` (x86 input dialects).

### Tier-2 assertions — `Assert`

Each throws `AsmtestException` with a legible message (xUnit/NUnit report it).

```csharp
Asm.Assert.Ret(r, 42);                 // r.Ret == 42
Asm.Assert.AbiPreserved(r);            // callee-saved restored
Asm.Assert.Flag(r, "CF", set: true);   // flag set/clear
Asm.Assert.Fp(r, 3.75);                // r.FRet == 3.75
Asm.Assert.VecF32(r, 0, new float[]{1,2,3,4});  // vector lanes of register 0
Asm.Assert.NoFault(res);               // emulator run clean
Asm.Assert.Fault(res);                 // emulator run faulted
Asm.Assert.EmuReg(res, "rax", 42);     // x86 guest register
Asm.Assert.GuestReg(gres, "x0", 42);   // cross-arch guest register
Asm.Assert.Covered(t, 0x0);            // basic block entered
```

## Run the tests

```sh
export LD_LIBRARY_PATH=$PWD/build      # DYLD_LIBRARY_PATH on macOS
dotnet test
```

`make dotnet-test` (from the repo root) builds `libasmtest_emu` + the routine
fixture lib, then `dotnet run`s the conformance app
([`Program.cs`](https://github.com/wilvk/asm-test/blob/main/bindings/dotnet/Program.cs)),
which replays the corpus through `Asmtest.cs`. `make docker-dotnet` runs it in an
isolated container.

## Maturity

A published NuGet package with `runtimes/<rid>/native/` payloads, a
`LibraryImport` source generator, and xUnit/NUnit integration of the `Assert`
helpers are future work; the reusable library module with Tier-2 assertions ships
today. See [Packaging the bindings](../reference/packaging.md) for the staging plan.

:::{note}
The Track D/E/F surface — `Emu.WatchWrites`, `Emu.GuardReg`, `Emu.FuzzCover`,
`Emu.MutationTest`, and `Avx.CaptureVec256` — ships in `Asmtest.cs` and is mapped
in the [shared capability table](index.md); the semantics of each are documented
in full on the [Python reference page](python.md), which this binding mirrors
name-for-name.
:::
