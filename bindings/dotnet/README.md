# asm-test — .NET binding

Run, **capture**, and **emulate** assembly routines through the
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

## Deferred

A published NuGet package with `runtimes/<rid>/native/` payloads, a
`LibraryImport` source generator, and xUnit/NUnit integration of the `Assert`
helpers are future work; the reusable library module with Tier-2 assertions ships
today.
