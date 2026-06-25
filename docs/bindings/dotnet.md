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

Pass a routine as an **assembly string**. Present only in the Keystone-carrying
`libasmtest_emu_asm` (`make dotnet-asm-test` points `ASMTEST_LIB` at it);
`Emu.AsmAvailable` is false against the plain lib.

```csharp
[Fact] public void InlineAssembler() {       // optional: the routine IS the text
    if (!Asm.Emu.AsmAvailable) return;        // false against the plain libasmtest_emu
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

## Run the tests

```sh
export LD_LIBRARY_PATH=$PWD/build      # DYLD_LIBRARY_PATH on macOS
dotnet test
make dotnet-asm-test                   # or: point ASMTEST_LIB at libasmtest_emu_asm first
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
