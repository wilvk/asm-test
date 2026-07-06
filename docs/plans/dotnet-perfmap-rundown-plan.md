# asm-test — dependency-free in-process perf-map rundown (§D0.2, hand-rolled): implementation plan

Make the whole-window managed-method breakdown
([examples/dotnet/methods](../../examples/dotnet/methods/)) name **warm** methods
(JIT-compiled *before* the scope — e.g. `System.Console.WriteLine`), not only the
**cold** methods JIT'd inside it — **without a NuGet dependency and without a launch
knob**. This is the "Option B, hand-rolled" realisation of the managed plan's
[§D0.2 pre-arm rundown](scoped-tracing-managed-plan.md); the shipped `JitMethodMap`
(an in-proc `MethodLoadVerbose` `EventListener`) sees only methods JIT'd *after* it is
enabled, so warm methods land in the unlabelled `[native runtime]` remainder.

> **Status: BUILT + validated.** Shipped as `DiagnosticsIpc` + `JitMethodMap.LoadPerfMap`
> + `AsmTrace(withRundown: true)` in [bindings/dotnet/hwtrace/HwTrace.cs](../../bindings/dotnet/hwtrace/HwTrace.cs),
> demoed by [examples/dotnet/rundown](../../examples/dotnet/rundown/). Validated in the
> asmtest-dotnet container: rundown-on names warm `Program::Main`; `DOTNET_EnableDiagnostics=0`
> → `RundownEnabled=false`, clean fallback to the cold-only `JitMethodMap`; binding test 33/0.
> Adversarially reviewed (socket timeouts so it never hangs the scope, `.`/`::` dedupe,
> `DisablePerfMap` on close, EnablePerfMap once before arming — never single-steps the socket).

## Why a rundown, and why hand-rolled

Warm methods need a **rundown** — an enumeration of *already*-JIT'd methods. An
in-process `EventListener` cannot get one (rundown is an EventPipe/diagnostics-session
concept). The runtime *can* be told, at runtime, to enable perf-map generation, and
CoreCLR's `PerfMap::Enable` **runs down all already-JIT'd methods** (walks the loaded
R2R assemblies + JIT code heap) into `/tmp/perf-<pid>.map`, then logs new ones forward.
The public client for that is `DiagnosticsClient.EnablePerfMap` in the
`Microsoft.Diagnostics.NETCore.Client` NuGet package — but it is just a client for a
documented binary protocol over a socket the runtime already opens. Hand-rolling that
one message keeps the binding **dependency-free** (BCL sockets only) and needs no
`DOTNET_PerfMapEnabled` launch knob. The cost is a small, version-pinned slice of a
binary protocol we maintain ourselves (see Risks).

## The verified wire protocol (pinned to .NET 8+)

Confirmed against `dotnet/diagnostics` source. Every CoreCLR process opens a Unix
domain socket `/tmp/dotnet-diagnostic-<pid>-<disambiguation>-socket` at startup (unless
`DOTNET_EnableDiagnostics=0`). One message enables the perf-map:

```
IpcMessage = Header (20 bytes) + Payload (4 bytes)          # total 24
Header:
  magic       14 bytes   "DOTNET_IPC_V1\0"  (13 ASCII + NUL)
  size        u16 LE      24                 (header + payload)
  commandSet  u8          0x04               (Process)
  commandId   u8          0x05               (EnablePerfMap)
  reserved    u16         0x0000
Payload:
  perfMapType u32 LE      3                  (PerfMapType.PerfMap -> /tmp/perf-<pid>.map)
```

`PerfMapType`: `None=0, All=1, JitDump=2, PerfMap=3`. Response is an IpcMessage whose
header `commandSet == 0xFF` (Server) and `commandId == 0x00` (OK) on success (payload a
u32 HRESULT). We read + validate the response, then confirm the perf-map file appears.

## Pieces to build (all in `bindings/dotnet/hwtrace/`, examples in `examples/dotnet/`)

1. **`DiagnosticsIpc` (new, C#).** BCL-sockets-only client for the one command:
   - Glob `/tmp/dotnet-diagnostic-<Environment.ProcessId>-*-socket`; connect a
     `Socket(AddressFamily.Unix, Stream, unspecified)` to the newest match.
   - Send the 24-byte `EnablePerfMap(PerfMap)` message above; read + validate the
     response header.
   - Return `bool` — false (clean self-skip) if the socket is absent, connect/send/recv
     fails, or the response is not OK. Never throws.
2. **`JitMethodMap.LoadPerfMap(path)` (extend the shipped class).** Parse
   `/tmp/perf-<pid>.map` lines `<hexStart> <hexSize> <name>` into the existing
   `(Start,End,Name)` entries, then `Freeze()`; `Resolve` already binary-searches. This
   makes the map cover **warm + cold** (the perf-map rundown superset).
3. **`AsmTrace(..., bool withRundown = false)`.** Opt-in. In the ctor (so the rundown
   captures pre-scope warm methods and forward-logs the cold ones), call
   `DiagnosticsIpc.EnablePerfMap()`. On `Dispose`, after capture, `LoadPerfMap` the file
   and attribute the captured `Addresses` against it → `Methods` now includes warm
   methods. If the rundown self-skips or the file never appears, fall back to the
   existing cold-only `JitMethodMap` result (and record why via `SkipReason`-style note).
4. **Example.** Extend or add an [examples/dotnet/methods](../../examples/dotnet/methods/)
   run with `withRundown: true`, asserting a known **warm** method (e.g.
   `System.Console.WriteLine`) now appears in the breakdown.

## Timing / flush

The perf-map is written by the runtime as methods are run down / JIT'd. Enable in the
**ctor** so the pre-scope rundown lands early; read the file on `Dispose`. If the file
is buffered and reads partial mid-process, add a bounded settle (poll the file for
growth to stop, with a short timeout) before parsing — determined empirically during
the build, not assumed.

## Self-skip / honesty

- No socket (`DOTNET_EnableDiagnostics=0`), connect/protocol error, or no perf-map file
  → self-skip to the cold-only result; the feature never throws and never blocks the
  scope.
- Document the **version pin** (the Process/EnablePerfMap command + `PerfMapType` are a
  private-ish contract, stable across .NET 6–9, EnablePerfMap added in .NET 8) and the
  self-diagnostics caveat (self-connecting the diagnostics socket is fine for this light,
  fire-and-forget command; heavier self-EventPipe/rundown sessions have known deadlock
  reports, which is why this stays opt-in and narrow).

## Tests / validation

- Container run of the methods example with `withRundown: true`: assert a warm method
  (`System.Console.WriteLine` or another known-warm BCL method) appears in `Methods`,
  and `ColdPath` still appears. Cold-only run (default) unchanged.
- `make docker-hwtrace-dotnet` binding test stays green.
- Validate the socket-absent path self-skips (e.g. `DOTNET_EnableDiagnostics=0`).

## Risks

- **Protocol drift.** A wrong byte fails at runtime (self-skip), not build. Mitigated by
  pinning to the verified values + the fall-back to cold-only. If a future runtime moves
  the command, the feature self-skips rather than misbehaving.
- **Perf-map buffering** (the timing/flush point above).
- **Self-diagnostics deadlock** for heavy ops — not this light command, but kept opt-in.

## Relationship

- Realises [scoped-tracing-managed-plan §D0.2](scoped-tracing-managed-plan.md) without
  the NuGet dependency; the cold-only path is [§D0.1](scoped-tracing-managed-plan.md)
  (the shipped `JitMethodMap`). Both converge on the same C perf-map/attribution back end
  ([asmtest_hwtrace_symbolize_bucket](../../include/asmtest_hwtrace.h)) conceptually,
  though this reads the perf-map in-managed for O(log n) resolution over a large window.
