# asm-test — .NET binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from C# / .NET, via
**P/Invoke** (`DllImport`).

The entry points are the opaque-handle FFI layer (`src/ffi.c`), so no C struct
layout is mirrored: `asmtest_corpus_routine` for routine addresses,
`asmtest_capture6` / `_fp2` + `asmtest_regs_*` accessors for capture, and
`asmtest_emu_call2` + accessors for the emulator (faults as data).

## Run

```sh
make dotnet-test      # from the repo root (needs the shared libs + .NET SDK)
make docker-dotnet    # or in an isolated container
```

The reusable module is [`Asmtest.cs`](Asmtest.cs) — it keeps all P/Invoke
(`DllImport`) inside and exposes the `Regs` / `Emu` / `EmuResult` classes plus an
`Assert` helper, so calling code never declares a native entry point.
[`Program.cs`](Program.cs) is a thin consumer that replays the corpus through it.
`make dotnet-test` builds `libasmtest_emu` + the routine fixture lib, then
`dotnet run`s the app. The native libs are resolved by the loader via
`LD_LIBRARY_PATH` (set by the target) — `DllImport("asmtest_emu")` /
`("asmtest_corpus")` find them by soname.

## In-line assembler (optional)

Pass a routine as an **assembly string**. Present only in the Keystone-carrying
`libasmtest_emu_asm` (`make dotnet-asm-test` points `ASMTEST_LIB` at it);
`Emu.AsmAvailable` is false against the plain lib.

```csharp
if (Emu.AsmAvailable)
{
    using var e = new Emu();
    // Intel, up to six args; throws AsmtestException (with the Keystone
    // diagnostic) if the string fails to assemble.
    using var res = e.CallAsm("mov rax, rdi; add rax, rsi; ret", new long[] { 40, 2 });
    // res.Reg("rax") == 42. AT&T + an instruction cap:
    e.CallAsm(src, new long[] { 10, 20, 12 }, AsmSyntax.Att, maxInsns: 0);
    // Multi-arch text -> bytes (x86-64/arm64/riscv64/arm32):
    byte[] a64 = Emu.Assemble("ret", AsmArch.Arm64);
}
```

## Deferred

A published NuGet package with `runtimes/<rid>/native/` payloads, a
`LibraryImport` source generator, and xUnit/NUnit integration of the `Assert`
helpers are future work; the reusable library module with Tier-2 assertions ships
today.
