# asm-test — .NET binding

Run, **capture**, **emulate**, and **assemble** assembly routines through the
[asm-test](https://github.com/wilvk/asm-test) framework from C# / .NET, via
**P/Invoke** (`DllImport`).

The entry points are the opaque-handle FFI layer (`src/ffi.c`), so no C struct
layout is mirrored: `asmtest_corpus_routine` for routine addresses,
`asmtest_capture6` / `_fp2` + `asmtest_regs_*` accessors for capture, and
`asmtest_emu_call2` + accessors for the emulator (faults as data: `Faulted`, plus
`FaultAddr` / `FaultKind` — a `FaultKind` enum — for where and why one hit).

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

Pass a routine as an **assembly string**. `libasmtest_emu` carries the Keystone
in-line assembler, so this works out of the box under `make dotnet-test`.
`Emu.AsmAvailable` is a defensive probe — false only against an older/leaner lib
pointed at by `ASMTEST_LIB`.

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

## Corpus fixtures (optional)

`Corpus.Routine("add_signed")` resolves a canonical conformance routine to its
address. Those routines live in the **fixtures** lib (`libasmtest_corpus`), which
is *not* bundled in the NuGet package (it is dev/test fixtures, not framework
code). Point `ASMTEST_CORPUS_LIB` at a built `libasmtest_corpus.{so,dylib}` to use
it; guard with `Corpus.Available`. Without it, `Corpus.Routine` throws a clear
`AsmtestException` (not a raw `DllNotFoundException`) — matching the Ruby/Node
bindings, which likewise only load the corpus when `ASMTEST_CORPUS_LIB` is set.

```csharp
if (Corpus.Available)
{
    IntPtr fn = Corpus.Routine("add_signed");
    // ... capture/emulate through fn
}
```

## Native tracing (DynamoRIO, optional)

The peer of the emulator tier for tracing **host-native** code as it runs *inside
this .NET process*, backed by DynamoRIO. The wrapper is
[`drtrace/DrTrace.cs`](drtrace/DrTrace.cs) — `DrTrace.Available()` /
`Initialize()` / `Shutdown()` plus `NativeCode` (executable W^X memory) and
`NativeTrace` (block coverage + an ordered instruction stream). It loads
`libasmtest_drapp` via a `DllImportResolver`: `ASMTEST_DRAPP_LIB` →
`<repo>/build/libasmtest_drapp.so` → by soname. Advanced, Linux-x86-64-only,
opt-in — `DrTrace.Available()` is false (never throws) when the lib is absent or
DynamoRIO is unresolvable, so callers self-skip.

The runnable smoke test mirrors `bindings/python/tests/test_drtrace.py`; it
self-skips (prints `SKIP`, exits 0) without DynamoRIO:

```sh
# from the repo root
ASMTEST_DRAPP_LIB=$PWD/build/libasmtest_drapp.so \
  dotnet run --project bindings/dotnet/drtrace/drtrace.csproj
```

It is its own project (`drtrace/drtrace.csproj`, `EnableDefaultCompileItems=false`
+ explicit `Compile` includes) and `asmtest.csproj` excludes the `drtrace/`
subtree (one `<Compile Remove>`), so the two never glob-collide.

## Deferred

A published NuGet package with `runtimes/<rid>/native/` payloads, a
`LibraryImport` source generator, and xUnit/NUnit integration of the `Assert`
helpers are future work; the reusable library module with Tier-2 assertions ships
today.
