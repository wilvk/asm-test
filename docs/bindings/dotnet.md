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
`FaultKind` for where and why one hit). See [Language bindings](../bindings.md)
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

Every public member of `Asmtest.cs`, with an example and its options. All four
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
today. See [Packaging the bindings](../packaging.md) for the staging plan.
