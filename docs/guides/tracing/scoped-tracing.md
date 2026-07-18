# Scoped in-process tracing

The developer-ergonomics face of the tracing machinery: bracket a region of a
program's **own** code with a scope construct — `using`/RAII/`with`/`defer`/a block —
and get back the **assembly that executed inside it**, rendered on close. The
developer-visible surface is reduced to the **package import plus the scope**.

```csharp
using (new AsmTrace())         // the scope — auto-named, arms lazily
{
    HotPath(data);             // whatever runs here: its executed asm is captured
}
```

This repackages shipped machinery — the per-thread `begin`/`end` region markers, the
single-step / hardware backends, and the Capstone renderer — behind a thin per-language
shim over a small shared C core. It adds **no new capture primitive**. See
[Hardware tracing](hardware-tracing.md) for the backends and the shared-core primitives
this rides, and the [implementation summary](../../scoped-tracing-implementation.md)
for the full done-vs-forward-look accounting.

## The model

A scope is exactly an enable/disable window. On close the shim:

1. **Registers-then-begins** under a call-site auto-name (a self-naming scope must
   register under the same generated name it will begin; `register_region` is
   idempotent by name, so a looped or sprinkled scope reuses one slot).
2. **Renders** the recorded instruction offsets to Capstone disassembly text — the
   `Path`/`path` the scope exposes, also written to stdout unless emission is disabled.
3. **Flags `truncated`** if the scope closed on a **different OS thread** than it
   opened — the work hopped (`await` / `Task.Run` / `go func()` / a thread-pool
   continuation) and the capture followed the arming thread, not the work. A partial
   trace is never presented as complete.

Three properties hold across every binding:

- **Thread scope, not logical-operation scope.** Capture follows the calling thread.
  Work that hops threads is not captured unless the (forward-look) async-hop stitching
  is active; the `truncated` bit is the honest backstop.
- **You get everything that ran — including the runtime.** An unwarmed body traces the
  JIT compiling it, GC, and BCL plumbing. The region-scoped mode used by the shipped
  decoders is clean (out-of-region instructions are dropped at decode); the empty-scope
  whole-window mode is honest but noisy.
- **Self-skip is the house style, and the model is Linux-only.** Every scope compiles
  and runs everywhere; where no faithful backend is available it degrades to a recorded
  no-op that records why. On any x86-64 Linux the single-step backend (`EFLAGS.TF`)
  makes it work with no PMU, no `perf_event`, no privilege, and no decoder beyond
  Capstone — including CI and plain containers.
- **The empty-scope whole-window ladder picks WEAK vs STRONG at arm time.** For the
  region-free `using (new AsmTrace())` form, the tier is auto-selected: the **STRONG**
  Intel PT tier (a quiet, near-zero-overhead whole-window capture) is chosen only on a
  bare-metal Intel host that exposes the `intel_pt` PMU **and** whose whole-window decode
  proves itself at runtime; everywhere else the ladder lands on the **WEAK** single-step
  tier, the universal floor. The **CEILING** AMD LBR tier is never auto-selected for the
  exact whole-window contract — a sampled branch survey cannot meet it — so on AMD the
  ladder uses WEAK, and the quiet sampled complement is reached explicitly with
  `new AsmTrace(HwBackend.AmdLbr)` (live floor Zen 4+). The explicit
  `using (new AsmTrace(HwBackend.IntelPt))` inline form arms the same STRONG PT window
  directly (exact, not statistical; silicon-gated — self-skips off bare-metal Intel PT
  with a reason that names the PT gate).

## Per-language shape

One pattern, ten realisations. Each binding auto-names from the call site, arms lazily
on first scope, renders on close, and surfaces the `truncated` thread-scope bit.

| Binding | Construct | Auto-name |
|---|---|---|
| [Python](../../bindings/python.md#hardware--single-step-tracing--hwtrace-optional) | `with HwTrace.scope(code) as t:` | `sys._getframe` |
| [C++](../../bindings/cpp.md#hardware--single-step-tracing--hwtrace-optional) | `asmtest::ScopedTrace` (RAII, `noexcept` dtor) | `__builtin_FILE`/`__builtin_LINE` (C++17) |
| [Zig](../../bindings/zig.md#hardware--single-step-tracing--hwtrace-optional) | `tr.scope(&code, @src(), emit)` → `Scope` + `defer deinit()` | `@src()` at the call site |
| [Ruby](../../bindings/ruby.md#hardware--single-step-tracing--hwtrace-optional) | `tr.scope(code) { … }` (block + `ensure`) | `caller_locations` |
| [Lua](../../bindings/lua.md#hardware--single-step-tracing--hwtrace-optional) | `tr:scope(code, fn)` (callback + `pcall`) | `debug.getinfo` |
| [Rust](../../bindings/rust.md#hardware--single-step-tracing--hwtrace-optional) | `HwTrace::scope(&code, emit)` → `ScopedTrace` (`Drop`) | `#[track_caller]` + `Location::caller` |
| [Go](../../bindings/go.md#hardware--single-step-tracing--hwtrace-optional) | `tr.Scope(code, emit, fn)` (closure + `LockOSThread`) | `runtime.Caller` |
| [.NET](../../bindings/dotnet.md#hardware--single-step-tracing--hwtrace-optional) | `using (new AsmTrace(code))` (`IDisposable`) | `[CallerMemberName]`/`[CallerLineNumber]` |
| [Node](../../bindings/node.md#hardware--single-step-tracing--hwtrace-optional) | `tr.scope(code, fn)` (callback; `using` on Node 24+) | stack-frame parse |
| [Java](../../bindings/java.md#hardware--single-step-tracing--hwtrace-optional) | `try (var t = HwTrace.AsmTrace.scope(code))` | `StackWalker` |

Each per-binding page carries a runnable **Scoped tracing** example.

## Thread-hop caveats by tier

- **Native / no-GC (C++, Rust, Zig).** `begin == end` same-thread holds by
  construction. C++/Rust close via true RAII (`~ScopedTrace` / `Drop`); Zig's `Scope`
  is *not* RAII — it needs an explicit `defer scope.deinit()`, and `defer` is skipped
  on `panic` (the trace is then simply not emitted, never partial-as-complete).
- **Interpreter (Python, Ruby, Lua).** Real import-time arm hooks; Ruby's genuine
  hazard is Ractors and the 3.3+ M:N scheduler (fibers never migrate OS threads), Lua's
  is yield-across-`pcall` under LuaJIT. The tid assert is the backstop.
- **Non-moving GC + M:N scheduler (Go).** Solved by pinning the OS thread
  (`runtime.LockOSThread`) for the region — the fix for the resolved single-step
  flaky-crash. `go func()` fan-out inside the scope runs on other OS threads and is
  **silently untraced** (a disclosed gap), not stitched.
- **Managed JIT (.NET, Node, Java).** The scope over a **known native leaf** works
  today; tracing the runtime's own live JIT output (async-hop stitching, the
  out-of-process ptrace-stealth stepper) is a forward-look managed-tier capability —
  see the [implementation summary](../../scoped-tracing-implementation.md).

## The boundary

You get *which* instructions ran, in what order, and the code bytes rendered to
assembly — **not** register/memory values per step. Value capture stays the
[emulator tier](traces.md)'s job.

## See also

- [Hardware tracing](hardware-tracing.md) — the backends + the shared-core scoped
  primitives (`try_begin` / `arm_tid` / `render`, idempotent `register_region`).
- [Native runtime tracing](native-tracing.md) — the region lifecycle and the
  out-of-process ptrace stepper the managed tier conceals.
- [Scoped-tracing implementation summary](../../scoped-tracing-implementation.md) —
  what is built + tested vs. forward-look.
