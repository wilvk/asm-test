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

## Hardware tracing + scoped tracing (single-step tier)

The hardware-trace peer, needing **no** DynamoRIO install: the wrapper is
[`hwtrace/HwTrace.cs`](hwtrace/HwTrace.cs), P/Invoking `libasmtest_hwtrace` via a
`DllImportResolver` (`ASMTEST_HWTRACE_LIB` → the NuGet `runtimes/<rid>/native/`
slot → `<repo>/build/` → by soname). The portable default is the single-step
backend (any x86-64 Linux, no PMU, no privilege); PT/LBR/CoreSight self-skip off
their hardware. It also compiles into the packable `AsmTest.dll`
(`asmtest-lib.csproj`), so NuGet consumers get the whole surface:

- **`AsmTrace : IDisposable`** — the scoped-tracing construct
  (import + `using`, auto-named from the call site, never throws):
  - `new AsmTrace(code)` — region scope over a `NativeCode` routine; `Path` holds
    the rendered listing on close.
  - `new AsmTrace()` — the zero-config whole-window scope; auto-inits the tier
    (no `HwTrace.Init` needed), exposes `Addresses` (data-only default) —
    `renderPath: true` opts into rendering `Path`.
  - `byMethod: true` — label the window by managed method (`Methods`,
    `Disassembly`, `InstructionsIn`); `withRundown: true` also names warm +
    ReadyToRun BCL methods via an in-process diagnostics-socket rundown.
- **`HwTrace`** — tier lifecycle (`Available`/`Init`/`Shutdown`), per-trace
  coverage recorders (`Create`/`Register`/`Region`), backend/tier auto-selection
  (`Resolve`/`Auto`/`ResolveTiers`/`AutoTier`).
- **`Ptrace` / `Descent` / `CodeImage`** — out-of-process tracing, call descent,
  and the time-aware code-image recorder.

The runnable smoke test (TAP-style, self-skips off x86-64 Linux):

```sh
make hwtrace-dotnet-test           # or, directly:
dotnet run --project bindings/dotnet/hwtrace/hwtrace.csproj
```

Live demos live under [`examples/dotnet/`](../../examples/dotnet/) (`make
hwtrace-dotnet-example`); the API story is in
[docs/bindings/dotnet.md](../../docs/bindings/dotnet.md).

## Deferred

A `LibraryImport` source generator and xUnit/NUnit integration of the `Assert`
helpers are future work. (The NuGet package with `runtimes/<rid>/native/`
payloads ships today — `make dotnet-package` builds it and the release workflow
publishes it.)
