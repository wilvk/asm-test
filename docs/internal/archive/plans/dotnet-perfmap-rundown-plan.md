# asm-test — dependency-free in-process jitdump rundown (§D0.2, hand-rolled): implementation plan

Make the whole-window managed-method breakdown
([examples/dotnet/rundown](../../../../examples/dotnet/rundown/)) name **warm** methods
(JIT-compiled *before* the scope) **and ReadyToRun (R2R) precompiled BCL methods** (e.g.
the whole `System.Console.WriteLine` write path), not only the **cold** methods JIT'd
inside it — **without a NuGet dependency and without a launch knob**. This is the
"Option B, hand-rolled" realisation of the managed plan's
[§D0.2 pre-arm rundown](../../plans/scoped-tracing-managed-plan.md); the shipped `JitMethodMap`
(an in-proc `MethodLoadVerbose` `EventListener`) sees only methods JIT'd *after* it is
enabled, so warm and R2R methods land in the unlabelled `[native runtime]` remainder.

> **Status: BUILT + validated.** Shipped as `DiagnosticsIpc` + `JitMethodMap.LoadJitDump`
> + `AsmTrace(withRundown: true)` in [bindings/dotnet/hwtrace/HwTrace.cs](../../../../bindings/dotnet/hwtrace/HwTrace.cs),
> demoed by [examples/dotnet/rundown](../../../../examples/dotnet/rundown/). Validated in the
> asmtest-dotnet container: rundown-on names the R2R Console write path
> (`StreamWriter::WriteLine[PreJIT]`, `ConsolePal::Write[PreJIT]`) + warm `Program::Main`
> — 38 methods / 30 R2R; `DOTNET_EnableDiagnostics=0` → `RundownEnabled=false`, clean fallback
> to the cold-only `JitMethodMap`; binding test 33/0. Adversarially reviewed (socket timeouts
> so it never hangs the scope, `.`/`::` dedupe, `DisablePerfMap` on close, EnablePerfMap once
> before arming — never single-steps the socket).

## Why a rundown, and why the jitdump (NOT the perf-map)

Warm + R2R methods need a **rundown** — an enumeration of *already-loaded* methods. An
in-process `EventListener` cannot get one (rundown is an EventPipe/diagnostics-session
concept). The runtime *can* be told, at runtime, to enable perf-map generation, and
CoreCLR's `PerfMap::Enable` runs down loaded methods. **Crucial correction (verified in
`coreclr/vm/perfmap.cpp`):** R2R method names go ONLY to the binary jitdump
(`/tmp/jit-<pid>.dump`), NEVER the text perf-map (`/tmp/perf-<pid>.map`, which is
JIT-only), and the R2R rundown (`ReadyToRunInfo::MethodIterator`) runs ONLY for
`PerfMapType.JitDump`/`All` — **not** `PerfMap`. So the command must request `JitDump(2)`,
not `PerfMap(3)`, and read the jitdump. (An earlier iteration requested `PerfMap` and
missed R2R entirely — the whole BCL.) The public client for the command is
`DiagnosticsClient.EnablePerfMap` in the `Microsoft.Diagnostics.NETCore.Client` NuGet
package — but it is just a client for a documented binary protocol over a socket the
runtime already opens. Hand-rolling that one message keeps the binding **dependency-free**
(BCL sockets only) and needs no `DOTNET_PerfMapEnabled` launch knob. The cost is a small,
version-pinned slice of a binary protocol we maintain ourselves (see Risks).

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
  perfMapType u32 LE      2                  (PerfMapType.JitDump -> /tmp/jit-<pid>.dump, JIT + R2R)
```

`PerfMapType`: `None=0, All=1, JitDump=2, PerfMap=3`. **Use `JitDump(2)`** — only
JitDump/All run the R2R rundown; `PerfMap(3)` is JIT-only. Response is an IpcMessage whose
header `commandSet == 0xFF` (Server) and `commandId == 0x00` (OK) on success (payload a
u32 HRESULT). We read + validate, then read the jitdump. Paired `DisablePerfMap` = command
`0x06`, no payload. The jitdump is the perf `JIT_CODE_LOAD` binary format (magic `"JiTD"`,
records carrying `code_addr`+`code_size`+name); `JitMethodMap.LoadJitDump` parses it into
the address→name map (dedup by start address vs the listener's cold entries).

## Pieces to build (all in `bindings/dotnet/hwtrace/`, examples in `examples/dotnet/`)

1. **`DiagnosticsIpc` (new, C#).** BCL-sockets-only client for the one command:
   - Glob `/tmp/dotnet-diagnostic-<Environment.ProcessId>-*-socket`; connect a
     `Socket(AddressFamily.Unix, Stream, unspecified)` to the newest match.
   - Send the 24-byte `EnablePerfMap(JitDump)` message above; read + validate the
     response header. (Paired `DisablePerfMap`, command `0x06`, no payload.)
   - Return `bool` — false (clean self-skip) if the socket is absent, connect/send/recv
     fails, or the response is not OK. Never throws.
2. **`JitMethodMap.LoadJitDump(path)` (extend the shipped class).** Parse the binary
   `/tmp/jit-<pid>.dump` (perf `JIT_CODE_LOAD` records: `code_addr`+`code_size`+name) into
   the existing `(Start,End,Name)` entries, then `Freeze()`; `Resolve` already binary-
   searches. This makes the map cover **JIT + warm + R2R** (the jitdump superset), deduped
   by start address vs the listener entries.
3. **`AsmTrace(..., bool withRundown = false)`.** Opt-in. In the ctor (BEFORE arming, so
   the rundown's socket code is not single-stepped), call `DiagnosticsIpc.EnablePerfMap()`.
   On `Dispose`, after capture, `LoadJitDump` the file + `DisablePerfMap`, and attribute the
   captured `Addresses` → `Methods` now includes warm + R2R methods. If the rundown
   self-skips or the file never appears, fall back to the cold-only `JitMethodMap` result.
4. **Example.** [examples/dotnet/rundown](../../../../examples/dotnet/rundown/) runs
   `withRundown: true` and asserts the R2R BCL Console write path
   (`StreamWriter::WriteLine[PreJIT]`, `ConsolePal::Write[PreJIT]`) now appears.

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
- **Jitdump size / accumulating cost.** Unlike the tiny text perf-map, the jitdump embeds
  every method's machine code, and the runtime re-runs the warm+R2R rundown on *every*
  `withRundown` scope, appending to `jit-<pid>.dump` (never deleted) — so a long-lived
  process opening *many* rundown scopes pays growing disk + re-parse cost (O(cumulative
  methods) per scope; the parser skips code bytes, so memory stays bounded). Fine for the
  intended occasional-introspection use (single/few scopes, validated); if it matters,
  parse only the appended tail or truncate between scopes. Opt-in keeps it off by default.
- **Jitdump flush/partial file** at Dispose — a trailing unflushed record is bounded off by
  `rec + recSize > fs.Length` and skipped; the scope-open rundown is long flushed.
- **Self-diagnostics deadlock** for heavy ops — not this light command, but kept opt-in.

## Relationship

- Realises [scoped-tracing-managed-plan §D0.2](../../plans/scoped-tracing-managed-plan.md) without
  the NuGet dependency; the cold-only path is [§D0.1](../../plans/scoped-tracing-managed-plan.md)
  (the shipped `JitMethodMap`). Both converge on the same C perf-map/attribution back end
  ([asmtest_hwtrace_symbolize_bucket](../../../../include/asmtest_hwtrace.h)) conceptually,
  though this reads the perf-map in-managed for O(log n) resolution over a large window.
