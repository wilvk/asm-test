# asm-test — §D3 whole-window out-of-process channel: design proposal

The crash-proof route to tracing a **whole block** of managed code — a scope wrapped
around arbitrary C# (LINQ, exceptions, generics, `pthread_create`-adjacent tiering
work), not a single registered leaf. It is the one investment that turns the
already-shipping out-of-process stepper from a **region** primitive into a
**whole-window** capture the in-process single-step tier is *forbidden* to attempt.

> Status legend: **planned / forward-look.** This document is the design + phasing.
> It builds directly on primitives that already ship and are live-tested
> (`asmtest_ptrace_*`, `asmtest_hwtrace_stealth_trace`, `asmtest_codeimage_*`,
> `asmtest_proc_perfmap_symbol`); the net-new work is the **whole-window live-JIT
> address channel** that lets an out-of-process stepper record *absolute* addresses
> across an entire thread window and resolve them to managed methods afterward.
> Related: [managed-singlestep-posture-plan.md](managed-singlestep-posture-plan.md)
> (why in-process whole-window cannot be crash-proof),
> [scoped-tracing-managed-plan.md](scoped-tracing-managed-plan.md) (§D3 is item 7's
> "concealed out-of-process ptrace stepper scope"),
> [zen2-singlestep-trace-plan.md](zen2-singlestep-trace-plan.md) Phase 5 (the W2 ptrace
> tier this extends).

## The forcing problem

The in-process whole-window form (`new AsmTrace()` on a managed thread — what
[examples/dotnet/localscope](../../../examples/dotnet/localscope/) demonstrates) is
**best-effort by construction and cannot be made crash-proof** (posture plan §"forcing
evidence"): any code that blocks `SIGTRAP` — glibc `pthread_create` around `clone()`,
the CLR's two-pass exception dispatch — turns the next single-step `#DB` into a masked
synchronous fault the kernel force-resets to `SIG_DFL` (exit 133). No `sigaction` flag
beats `force_sig`; interposition is a time bomb (posture plan §Non-goals).

The **out-of-process** stepper does not have this failure mode *at all*: it drives the
tracee with `PTRACE_SINGLESTEP`/`PTRACE_SINGLEBLOCK`, and the `#DB` is delivered to the
**tracer** via `waitpid` — a ptrace-stop is **not** gated by the tracee's signal mask
([src/ptrace_backend.c](../../../src/ptrace_backend.c) `~1264-1299`). So the tracee may
freely block `SIGTRAP` and never dies. This is exactly why the
`ptrace_dotnet`/`blockstep`/`descent_dotnet` examples call themselves "the scenario
in-process single-step is forbidden to do."

**The gap:** that stepper today traces a **region** — a registered `[base, len)`
method body resolved by name from the perf-map (`asmtest_ptrace_trace_attached` +
`run_to`). It does not have a "record *everything* the thread runs between two points,
by absolute address, and resolve the managed methods afterward" mode. Building that —
the **§D3 whole-window live-JIT address channel** — is this plan.

## What already ships (the substrate this builds on)

- **Out-of-process steppers.** `asmtest_ptrace_trace_attached[_blockstep|_versioned]`
  ([asmtest_ptrace.h](../../../include/asmtest_ptrace.h)) — exact per-instruction or
  per-taken-branch (BTF) capture of a region in a foreign/attached process; reads
  target bytes via `process_vm_readv`; `run_to` plants a breakpoint to reach a method
  entry with no cooperative go-flag.
- **Self-attach without an external harness.** `asmtest_hwtrace_stealth_trace`
  ([src/stealth_helper.c](../../../src/stealth_helper.c)) spawns a bundled helper
  **child** that `prctl(PR_SET_PTRACER)` + `PTRACE_SEIZE`es back onto its parent — so a
  process traces *itself* out-of-process, in a plain unprivileged container. This is
  what `AsmTrace.Method(outOfProcess: true)` already rides.
- **Time-versioned code image.** `asmtest_codeimage_*`
  ([asmtest_codeimage.h](../../../include/asmtest_codeimage.h)) records a timestamped
  byte timeline so a stepper decodes against the bytes that were **live when the code
  ran** — correct under re-JIT / address reuse.
- **Managed method resolution.** `asmtest_proc_perfmap_symbol` (perf-map by name) and
  `asmtest_jitdump_find` (binary jitdump, code bytes + load timestamps); and, on the
  .NET side, the `JitMethodMap` `MethodLoadVerbose` listener + `DiagnosticsIpc` rundown
  (`HwTrace.cs`) that already turn a captured **absolute** address stream into named
  methods for the *in-process* whole-window form.

The observation that makes this tractable: the **attribution half already works
whole-window** (the in-process form captures absolute addresses and `_map.Resolve`s
them). Only the **capture half** needs to move out-of-process.

## The design

A whole-window out-of-process scope = *stepper capturing absolute addresses on the
managed thread, for a whole window, driven from a second observer, with the tracee's
signal mask irrelevant.* Four pieces:

1. **Region-free absolute capture (native).** A new
   `asmtest_ptrace_trace_window_attached(pid, tid, &trace, ...)` that steps the target
   thread from a start marker to an end marker with **no `[base, len)` filter**,
   appending each stop's absolute PC to `trace->insns[]` (the same shape
   `begin_window` fills in-process). BTF block-step
   (`PTRACE_SINGLEBLOCK`) is the default driver — ~4–10× fewer stops than
   per-instruction, byte-identical after `blockstep_reconstruct`, and the only thing
   that makes a million-instruction window remotely affordable out-of-process.
2. **Window delimiting without a cooperative flag.** `begin`/`end` plant breakpoints
   (`run_until`) at the scope's entry/exit return addresses in the managed caller, so
   the observer knows the exact `[start, end)` of the window on the target thread —
   reusing `run_to`'s breakpoint-rewind mechanics. The managed side marks the window by
   calling two tiny library thunks (the reverse-P/Invoke landing sites), so no source
   cooperation beyond the `using` block is needed.
3. **Thread targeting.** The window rides **one** managed thread (the arming thread, as
   the in-process form already requires). The observer attaches that tid specifically;
   sibling GC/tiering threads run free (an `int3` on a thread the tracer does not own
   kills the process — the same rule the HotSpot lane already handles by attaching the
   right tid). This side-steps the whole `pthread_create` hazard: the tiering worker
   can spawn freely because it is never stepped.
4. **Attribution reuse.** After close, feed the absolute `insns[]` through the existing
   `JitMethodMap` + jitdump rundown (whole-window attribution already implemented in
   `HwTrace.cs`), decoding against the `asmtest_codeimage` version live at each stop.

The result is a whole-window scope that (a) survives every managed feature by
construction, (b) reuses the shipped attribution, and (c) is honest — BTF-degraded
hosts and dropped stops self-truncate exactly as the region path does.

## Opt-in surface (where it makes sense)

Out-of-process capture costs a ptrace round-trip per stop (~100–1000× the stepped
thread's native speed) and needs ptrace permission (Yama `ptrace_scope`, or the
`PR_SET_PTRACER` self-attach helper). It must therefore be **opt-in**, never a silent
default that changes a whole-window scope's cost/permission profile:

- **.NET:** `new AsmTrace(outOfProcess: true, ...)` on the whole-window ctor —
  mirroring the flag `AsmTrace.Method(outOfProcess:)` already exposes. Default stays
  the in-process best-effort WEAK tier (works anywhere, no ptrace), with the
  `DegradationNote`/`SkipReason` ladder pointing at this flag when the in-process form
  would be fatal. Self-skips cleanly (records `SkipReason`) where ptrace is denied — a
  documented fidelity downgrade the in-process form does not have.
- **C API:** a distinct entry point (`asmtest_ptrace_trace_window_attached`), not a
  mode bit on `begin_window`, so the in-process fast path is untouched and the
  permission/toolchain requirements are explicit at the call site.

## Phasing

1. **W-0 — native region-free absolute capture.** `trace_window_attached` over a
   *native* fixture (no managed runtime needed): fork a child that runs a known blob,
   step the whole window absolute, assert the absolute-address stream matches the
   in-process `begin_window` result byte-for-byte. Host-testable in a plain container
   (`make hwtrace-test`), honoring "no untested hardware code."
2. **W-1 — BTF block-step driver + delimiting.** Add the `PTRACE_SINGLEBLOCK` path and
   the begin/end breakpoint delimiting; assert identical stream vs W-0 at ~1 stop per
   taken branch; self-skip where a hypervisor masks `DEBUGCTL.BTF`
   (`blockstep_available`).
3. **W-2 — .NET whole-window `outOfProcess: true`.** Wire the ctor flag through the
   stealth self-attach helper; capture a whole inline block (the localscope body) on
   the managed thread; fold absolute addresses through the existing `JitMethodMap` +
   rundown; a companion example (`examples/dotnet/localscope_oop/`) proving the caught
   exception + LINQ that crash the in-process form are captured cleanly.
4. **W-3 — cost controls + honesty.** An instruction/stop budget with self-truncation
   (a ~1M managed window is seconds out-of-process — bound it and flag `truncated`
   rather than hang), BTF-degrade / dropped-stop truncation parity, and the
   `SkipReason` wording for ptrace-denied hosts.

## Non-goals

- **Making the in-process whole-window form crash-proof.** Impossible by the §Z1
  form's definition (posture plan). This plan is the crash-proof *alternative*, not a
  fix to the in-process path — which stays as documented best-effort.
- **Whole-*program* (all-threads) capture.** The window is one managed thread by
  design; multi-thread out-of-process stepping is a separate, much larger effort and is
  not required for a scoped block.
- **Exact capture of an arbitrarily large window as the default.** Even out-of-process,
  a million stops is slow; W-3 bounds it. The point is *reliability and reach for
  bounded managed blocks*, not an unbounded profiler (that is the DynamoRIO tier's job).

## Validation

Every phase self-validates on hardware asm-test actually has (plain x86-64 Linux,
unprivileged container) — W-0/W-1 against native fixtures with no managed runtime, W-2
against a live CoreCLR the way the `ptrace_dotnet` lane already does. The BTF path
carries the existing `blockstep_available` functional probe so a `DEBUGCTL.BTF`-masking
hypervisor self-skips rather than silently degrading. No step depends on Intel PT or a
perf-permitted host — the whole point of the out-of-process route is that it runs where
the strong hardware tiers do not.
